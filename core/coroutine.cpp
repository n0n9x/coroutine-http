#include "coroutine.h"
#include <algorithm>  // std::push_heap / pop_heap
#include <stdexcept>

// ────────────────────────────────────────────────────────────────────────────
//  全局调度器指针（仅用于 co_entry 蹦床，限于单线程场景）
//  多线程扩展时应改为 thread_local。
// ────────────────────────────────────────────────────────────────────────────
static Scheduler* g_scheduler = nullptr;

// ============================================================
//  Coroutine 实现
// ============================================================

Coroutine::Coroutine(std::function<void()> f, uint64_t id)
    : stack(nullptr), func(std::move(f)), state(State::READY), coro_id(id)
{
    // 分配栈空间
    stack = new char[CO_STACK_SIZE];

    // 获取当前上下文作为模板，然后修改为协程专用
    if (getcontext(&ctx) != 0) {
        delete[] stack;
        stack = nullptr;
        throw std::runtime_error("getcontext failed: " + std::string(strerror(errno)));
    }

    ctx.uc_stack.ss_sp   = stack;
    ctx.uc_stack.ss_size = CO_STACK_SIZE;
    ctx.uc_link          = nullptr; // 返回时不自动跳转，由 co_entry 手动 setcontext

    // makecontext 通过两个 uint32 传递 64 位指针（跨平台标准做法）
    uintptr_t ptr = reinterpret_cast<uintptr_t>(this);
    makecontext(&ctx,
                reinterpret_cast<void(*)()>(Scheduler::co_entry),
                2,
                static_cast<uint32_t>(ptr),
                static_cast<uint32_t>(ptr >> 32));
}

Coroutine::~Coroutine() {
    delete[] stack;
    stack = nullptr;
}

// ============================================================
//  Scheduler 实现
// ============================================================

// ── 构造 / 析构 ──────────────────────────────────────────────

Scheduler::Scheduler()
    : running_coro(nullptr), epoll_fd(-1), next_id(1)
{
    // 注册全局指针（单线程）
    g_scheduler = this;

    epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0) {
        throw std::runtime_error("epoll_create1 failed: " + std::string(strerror(errno)));
    }
}

Scheduler::~Scheduler() {
    // 清理就绪队列中未执行的协程
    for (auto* co : ready_queue) delete co;
    ready_queue.clear();

    // 清理等待 I/O 的协程（不应泄漏）
    for (auto& [fd, co] : waiting_map) {
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
        delete co;
    }
    waiting_map.clear();

    // 清理定时器堆中的协程
    // （定时器里的协程指针可能与 waiting_map 重叠，需去重）
    for (auto& entry : timer_heap) {
        // 只清理不在 waiting_map 里的（避免双删）
        // 此处简化：定时器协程均已从 waiting_map 中分离
        delete entry.co;
    }
    timer_heap.clear();

    if (epoll_fd >= 0) {
        close(epoll_fd);
        epoll_fd = -1;
    }

    if (g_scheduler == this) g_scheduler = nullptr;
}

// ── co_entry 蹦床 ─────────────────────────────────────────────

/**
 * 协程函数的统一入口。
 * makecontext 只能传整数参数，故将 64 位指针拆成两个 32 位整数传递。
 */
void Scheduler::co_entry(uint32_t low, uint32_t high) {
    // 还原协程指针
    uintptr_t ptr = static_cast<uintptr_t>(low)
                  | (static_cast<uintptr_t>(high) << 32);
    Coroutine* co = reinterpret_cast<Coroutine*>(ptr);

    // 执行协程函数体（捕获异常，避免调用栈穿越到调度器）
    try {
        if (co->func) co->func();
    } catch (const std::exception& e) {
        std::cerr << "[coroutine #" << co->coro_id
                  << "] uncaught exception: " << e.what() << "\n";
    } catch (...) {
        std::cerr << "[coroutine #" << co->coro_id
                  << "] uncaught unknown exception\n";
    }

    // 标记完成，跳回调度器主上下文
    co->state = State::DONE;
    assert(g_scheduler && "Scheduler destroyed before coroutine finished");
    setcontext(&g_scheduler->main_ctx);

    // 不可达，消除编译器警告
    __builtin_unreachable();
}

// ── 创建协程 ──────────────────────────────────────────────────

Coroutine* Scheduler::create(std::function<void()> f) {
    auto* co = new Coroutine(std::move(f), next_id++);
    ready_queue.push_back(co);
    return co;
}

// ── yield ─────────────────────────────────────────────────────

/**
 * 主动让出 CPU，把自己放回就绪队列末尾，等待下次调度。
 * 必须在协程上下文中调用。
 */
void Scheduler::yield() {
    if (!running_coro) return; // 在调度器主上下文中调用，忽略

    Coroutine* cur  = running_coro;
    cur->state      = State::READY;
    ready_queue.push_back(cur);
    running_coro    = nullptr;
    swapcontext(&cur->ctx, &main_ctx);
    // ↑ 返回此处时，调度器已重新选中本协程继续运行
}

// ── wait_event ────────────────────────────────────────────────

/**
 * 挂起当前协程，等待 fd 上的 epoll 事件触发后再恢复。
 *
 * 使用 EPOLLONESHOT 确保每次只触发一次，避免多次唤醒同一协程。
 * 协程恢复后若需再次等待，需重新调用 wait_event。
 *
 * @return 实际触发的事件掩码（EPOLLIN / EPOLLOUT / EPOLLERR 等）
 */
uint32_t Scheduler::wait_event(int fd, uint32_t events) {
    if (!running_coro) return 0;

    // 使用 EPOLLONESHOT：事件触发一次后自动禁用，防止重复唤醒
    struct epoll_event ev{};
    ev.events   = events | EPOLLONESHOT | EPOLLET; // 边缘触发 + 单次
    ev.data.fd  = fd;

    // 如果 fd 已在监听（例如同一 fd 的读写等待），改用 MOD
    if (waiting_map.count(fd)) {
        if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev) < 0) {
            std::cerr << "[wait_event] epoll_ctl MOD fd=" << fd
                      << " error: " << strerror(errno) << "\n";
            return 0;
        }
    } else {
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
            std::cerr << "[wait_event] epoll_ctl ADD fd=" << fd
                      << " error: " << strerror(errno) << "\n";
            return 0;
        }
    }

    // 挂起：记录映射并切回调度器
    Coroutine* cur        = running_coro;
    cur->state            = State::WAITING;
    waiting_map[fd]       = cur;
    running_coro          = nullptr;

    // 在协程的 ctx 里存储触发事件（复用 coro_id 高位暂存，简单方案）
    // 更优雅的方式是在 Coroutine 结构体中加 triggered_events 字段
    uint32_t triggered = 0; // 见下方：run() 中唤醒时填充

    swapcontext(&cur->ctx, &main_ctx);
    // ← 协程在此处恢复执行

    // 唤醒后清除 epoll 注册（EPOLLONESHOT 已自动禁用，DEL 确保干净）
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);

    return triggered; // 注：简化版返回 0；完整版见扩展说明
}

// ── sleep ─────────────────────────────────────────────────────

/**
 * 挂起当前协程，睡眠至少 ms 毫秒。
 * ms == 0 等价于 yield()（让出一轮时间片）。
 */
void Scheduler::sleep(int64_t ms) {
    if (!running_coro) return;
    if (ms <= 0) { yield(); return; }

    Coroutine* cur = running_coro;
    cur->state     = State::WAITING;

    // 压入定时器堆
    push_timer({ now_ms() + ms, cur });

    running_coro = nullptr;
    swapcontext(&cur->ctx, &main_ctx);
    // ← 定时器到期后，run() 将协程放回 ready_queue，在此处恢复
}

// ── run ───────────────────────────────────────────────────────

/**
 * 主事件循环。
 *
 * 循环条件：就绪队列非空 OR 有协程在等待 I/O/定时器
 *
 * 每轮步骤：
 *   1. 依次执行所有就绪协程（直到就绪队列清空）
 *   2. 处理已到期的定时器，将协程推入就绪队列
 *   3. 调用 epoll_wait（超时时间=下一个定时器到期剩余毫秒）
 *   4. 将触发了 I/O 事件的协程推入就绪队列
 */
void Scheduler::run() {
    while (!ready_queue.empty() || !waiting_map.empty() || !timer_heap.empty()) {

        // ── Step 1：执行所有就绪协程 ─────────────────────────
        while (!ready_queue.empty()) {
            Coroutine* co = ready_queue.front();
            ready_queue.pop_front();

            running_coro = co;
            co->state    = State::RUNNING;

            swapcontext(&main_ctx, &co->ctx);
            // ← 协程主动挂起（yield/wait_event/sleep）或执行完毕后返回此处

            // 协程返回后，running_coro 可能已被协程内部清空（wait_event/sleep）
            // 若仍非空，说明是从 yield 返回（已重新入队）或 DONE
            if (running_coro) {
                if (running_coro->state == State::DONE) {
                    delete running_coro;
                }
                running_coro = nullptr;
            }
        }

        // ── Step 2：触发已到期的定时器 ────────────────────────
        fire_timers();

        // 若定时器唤醒了协程，回到 Step 1 先消耗掉
        if (!ready_queue.empty()) continue;

        // ── Step 3：epoll_wait 等待 I/O ───────────────────────
        if (waiting_map.empty() && timer_heap.empty()) break;

        struct epoll_event evs[64];
        int timeout = next_timeout_ms(); // -1 表示永久阻塞直到有事件
        int n = epoll_wait(epoll_fd, evs, 64, timeout);

        if (n < 0) {
            if (errno == EINTR) continue; // 被信号中断，重试
            std::cerr << "[run] epoll_wait error: " << strerror(errno) << "\n";
            break;
        }

        // ── Step 4：唤醒 I/O 就绪的协程 ──────────────────────
        for (int i = 0; i < n; i++) {
            int fd = evs[i].data.fd;
            auto it = waiting_map.find(fd);
            if (it != waiting_map.end()) {
                Coroutine* co = it->second;
                waiting_map.erase(it);
                co->state = State::READY;
                ready_queue.push_back(co);
            }
        }

        // epoll_wait 超时后也要检查一次定时器
        fire_timers();
    }

    std::cout << ">>> 调度器事件循环结束，共创建 "
              << (next_id - 1) << " 个协程 <<<\n";
}

// ============================================================
//  定时器辅助实现（最小堆）
// ============================================================

int64_t Scheduler::now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()
    ).count();
}

void Scheduler::push_timer(TimerEntry entry) {
    timer_heap.push_back(entry);
    // std::push_heap 维护最大堆；我们用 greater<> 使其成为最小堆
    std::push_heap(timer_heap.begin(), timer_heap.end(),
                   [](const TimerEntry& a, const TimerEntry& b) {
                       return a.expire_ms > b.expire_ms; // 小顶堆
                   });
}

TimerEntry Scheduler::pop_timer() {
    std::pop_heap(timer_heap.begin(), timer_heap.end(),
                  [](const TimerEntry& a, const TimerEntry& b) {
                      return a.expire_ms > b.expire_ms;
                  });
    TimerEntry top = timer_heap.back();
    timer_heap.pop_back();
    return top;
}

int Scheduler::fire_timers() {
    int count = 0;
    int64_t now = now_ms();
    while (!timer_heap.empty() && timer_heap.front().expire_ms <= now) {
        TimerEntry e = pop_timer();
        e.co->state  = State::READY;
        ready_queue.push_back(e.co);
        count++;
    }
    return count;
}

int Scheduler::next_timeout_ms() const {
    if (timer_heap.empty()) return -1; // 永久阻塞
    int64_t diff = timer_heap.front().expire_ms - now_ms();
    return static_cast<int>(diff > 0 ? diff : 0);
}

// ============================================================
//  Channel<T> 友元方法实现
// ============================================================

// 显式实例化常用类型（避免链接错误，也可移至头文件）
template class Channel<int>;
template class Channel<std::string>;