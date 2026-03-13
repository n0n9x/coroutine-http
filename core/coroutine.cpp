#include "coroutine.h"
#include <fcntl.h>
#include <algorithm>
#include <stdexcept>

// ── 每线程调度器指针（co_entry 蹦床用）────────────────
thread_local Scheduler *tl_scheduler = nullptr;

// ============================================================
//  Coroutine
// ============================================================

Coroutine::Coroutine(std::function<void()> f, uint64_t id)
    // 初始化列表
    : stack(nullptr), func(std::move(f)), state(State::READY), coro_id(id)
{
    stack = new char[CO_STACK_SIZE];
    if (getcontext(&ctx) != 0) // 获取当前 CPU 环境并存入 ctx
    {
        delete[] stack;
        stack = nullptr;
        throw std::runtime_error("getcontext failed");
    }
    ctx.uc_stack.ss_sp = stack;
    ctx.uc_stack.ss_size = CO_STACK_SIZE;
    ctx.uc_link = nullptr; // 规定该协程执行完入口函数后，不需要自动切换到其他上下文

    // 将 C++ 的对象指针安全地转换为一个整数，以便进行后续的位运算
    uintptr_t ptr = reinterpret_cast<uintptr_t>(this);
    // 初始化协程执行现场，设置指令指针
    makecontext(&ctx,
                reinterpret_cast<void (*)()>(Scheduler::co_entry),
                2,
                static_cast<uint32_t>(ptr),
                static_cast<uint32_t>(ptr >> 32));
}

Coroutine::~Coroutine()
{
    delete[] stack;
    stack = nullptr;
}

// ============================================================
//  Scheduler
// ============================================================

Scheduler::Scheduler(int id)
    : id_(id), running_coro(nullptr), epoll_fd(-1), next_id(1),
      pipe_read_fd_(-1), pipe_write_fd_(-1)
{
    // 创建 epoll 句柄
    epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0)
        throw std::runtime_error("epoll_create1 failed");

    // 创建 pipe，用于跨线程唤醒 epoll_wait
    int fds[2]; // 用于接收内核分配的文件描述符
    if (pipe2(fds, O_NONBLOCK | O_CLOEXEC) < 0)
        throw std::runtime_error("pipe2 failed");
    pipe_read_fd_ = fds[0];  // 管道读端
    pipe_write_fd_ = fds[1]; // 管道写端

    // 将 pipe 读端注册到 epoll
    struct epoll_event ev{};    // 在栈上创建一个 epoll 事件结构体，并初始化为零
    ev.events = EPOLLIN;        // 设置监控类型为 EPOLLIN（可读事件）
    ev.data.fd = pipe_read_fd_; // 在事件结构体中绑定文件描述符
    //epoll_fd监听pipe_read_fd_
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, pipe_read_fd_, &ev);
}

Scheduler::~Scheduler()
{
    //清理就绪队列
    for (auto *co : ready_queue)
        delete co;
    ready_queue.clear();
    //清理IO等待表
    for (auto &[fd, co] : waiting_map)
    {
        //取消监控
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
        delete co;
    }
    waiting_map.clear();
    //清理定时器堆
    for (auto &e : timer_heap)
        delete e.co;
    timer_heap.clear();

    //销毁通信管道
    if (pipe_read_fd_ >= 0)
    {
        close(pipe_read_fd_);
        pipe_read_fd_ = -1;
    }
    //销毁通信管道
    if (pipe_write_fd_ >= 0)
    {
        close(pipe_write_fd_);
        pipe_write_fd_ = -1;
    }
    //关闭监控中心
    if (epoll_fd >= 0)
    {
        close(epoll_fd);
        epoll_fd = -1;
    }
}

// ── co_entry ──────────────────────────────────────────────

void Scheduler::co_entry(uint32_t low, uint32_t high)
{
    //拼接成64位协程指针
    uintptr_t ptr = static_cast<uintptr_t>(low) | (static_cast<uintptr_t>(high) << 32);
    Coroutine *co = reinterpret_cast<Coroutine *>(ptr);

    try
    {
        if (co->func)
            co->func();
    }
    catch (const std::exception &e)
    {
        std::cerr << "[co#" << co->coro_id << "] exception: " << e.what() << "\n";
    }
    catch (...)
    {
        std::cerr << "[co#" << co->coro_id << "] unknown exception\n";
    }

    co->state = State::DONE;
    assert(tl_scheduler);
    //回复调度器主上下文/run函数
    setcontext(&tl_scheduler->main_ctx);
    __builtin_unreachable();
}

// ── create / post ─────────────────────────────────────────

Coroutine *Scheduler::create(std::function<void()> f)
{
    auto *co = new Coroutine(std::move(f), next_id++);
    ready_queue.push_back(co);
    return co;
}

void Scheduler::post(std::function<void()> f)//f  外部线程想要执行的任务代码块
{
    {
        std::lock_guard<std::mutex> lk(post_mutex_);
        post_queue_.push_back(std::move(f));
        //析构函数自动调用 post_mutex_.unlock()
    }
    // 写一个字节唤醒 epoll_wait
    char c = 1;
    write(pipe_write_fd_, &c, 1);
}

void Scheduler::drain_post_queue()
{
    std::deque<std::function<void()>> local;
    {
        std::lock_guard<std::mutex> lk(post_mutex_);
        //交换指向内存的指针
        local.swap(post_queue_);
    }
    for (auto &f : local)
        create(std::move(f));
}

// ── yield ─────────────────────────────────────────────────

void Scheduler::yield()
{
    if (!running_coro)
        return;
    Coroutine *cur = running_coro;
    cur->state = State::READY;
    ready_queue.push_back(cur);
    running_coro = nullptr;
    swapcontext(&cur->ctx, &main_ctx);
}

// ── wait_event ────────────────────────────────────────────

uint32_t Scheduler::wait_event(int fd, uint32_t events)
{
    // fd 协程想监听的文件描述符 events 想要监听的事件
    if (!running_coro)
        return 0;

    struct epoll_event ev{};
    // EPOLLONESHOT 一个时间只触发一次   EPOLLET 边缘触发
    ev.events = events | EPOLLONESHOT | EPOLLET;
    ev.data.fd = fd;

    if (waiting_map.count(fd))
    {
        epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev);
    }
    else
    {
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0 && errno == EEXIST)
            epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev);
    }

    Coroutine *cur = running_coro;
    cur->state = State::WAITING;
    waiting_map[fd] = cur;
    running_coro = nullptr;
    swapcontext(&cur->ctx, &main_ctx);//回到run

    //解除监听，待此协程再次被调度时继续进行
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
    return 0;
}

// ── sleep ─────────────────────────────────────────────────

void Scheduler::sleep(int64_t ms)
{
    if (!running_coro)
        return;
    if (ms <= 0)
    {
        yield();
        return;
    }

    Coroutine *cur = running_coro;
    cur->state = State::WAITING;
    push_timer({now_ms() + ms, cur});
    running_coro = nullptr;
    swapcontext(&cur->ctx, &main_ctx);
}

// ── run ───────────────────────────────────────────────────

void Scheduler::run()
{
    // 赋值当前调度器对象的地址
    tl_scheduler = this;

    while (true)
    {
        // 1. 执行就绪协程
        while (!ready_queue.empty())
        {
            Coroutine *co = ready_queue.front();
            ready_queue.pop_front();
            running_coro = co;
            co->state = State::RUNNING;
            swapcontext(&main_ctx, &co->ctx);
            if (running_coro && running_coro->state == State::DONE)
            {
                delete running_coro;
            }
            running_coro = nullptr;
        }

        // 2. 定时器
        fire_timers();
        if (!ready_queue.empty())//若产生了新的就绪协程，回到第一阶段去执行他们
            continue;

        // 3. epoll_wait 没有任何任务能做，线程进入“睡眠”状态
        struct epoll_event evs[64];
        int timeout = next_timeout_ms();
        int n = epoll_wait(epoll_fd, evs, 64, timeout < 0 ? 50 : timeout);

        // n>0 有n个文件描述符发生了事件，信息在evs数组里 n==0 时间到期 n<0 出错
        if (n < 0)
        {
            //ENTER 假错误，Ctrl+C
            if (errno == EINTR)
                continue;
            break;
        }

        // 4. 唤醒 I/O 协程 / 处理跨线程投递
        for (int i = 0; i < n; i++)
        {
            int fd = evs[i].data.fd;

            //外部线程想给调度器发任务
            if (fd == pipe_read_fd_)
            {
                // 清空 pipe，然后把投递的任务变成协程
                char buf[64];
                while (read(pipe_read_fd_, buf, sizeof(buf)) > 0)
                {
                }
                drain_post_queue();
                continue;
            }

            //唤醒等待 I/O 的协程
            auto it = waiting_map.find(fd);
            if (it != waiting_map.end())
            {
                Coroutine *co = it->second;
                waiting_map.erase(it);
                co->state = State::READY;
                ready_queue.push_back(co);
            }
        }

        fire_timers();
    }
}

// ── 定时器 ────────────────────────────────────────────────

int64_t Scheduler::now_ms()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(
               steady_clock::now().time_since_epoch())
        .count();
}

void Scheduler::push_timer(TimerEntry entry)
{
    timer_heap.push_back(entry);
    std::push_heap(timer_heap.begin(), timer_heap.end(),
                   [](const TimerEntry &a, const TimerEntry &b)
                   {
                       return a.expire_ms > b.expire_ms;
                   });
}

TimerEntry Scheduler::pop_timer()
{
    std::pop_heap(timer_heap.begin(), timer_heap.end(),
                  [](const TimerEntry &a, const TimerEntry &b)
                  {
                      return a.expire_ms > b.expire_ms;
                  });
    TimerEntry top = timer_heap.back();
    timer_heap.pop_back();
    return top;
}

int Scheduler::fire_timers()
{
    int count = 0;
    int64_t now = now_ms();
    while (!timer_heap.empty() && timer_heap.front().expire_ms <= now)
    {
        TimerEntry e = pop_timer();
        e.co->state = State::READY;
        ready_queue.push_back(e.co);
        count++;
    }
    return count;
}

int Scheduler::next_timeout_ms() const
{
    if (timer_heap.empty())
        return -1;
    int64_t diff = timer_heap.front().expire_ms - now_ms();
    return static_cast<int>(diff > 0 ? diff : 0);
}

// ============================================================
//  MultiScheduler
// ============================================================

MultiScheduler::MultiScheduler(int num_threads)
{
    // num_threads cpu核心数
    for (int i = 0; i < num_threads; i++)
        schedulers_.push_back(std::make_unique<Scheduler>(i));
}

MultiScheduler::~MultiScheduler()
{
    // 线程会在 run() 里一直跑，析构时直接 detach
    for (auto &t : threads_)
        if (t.joinable())
            t.detach();
}

void MultiScheduler::post(std::function<void()> f)
{
    // round-robin 分发
    int idx = rr_index_.fetch_add(1) % static_cast<int>(schedulers_.size());
    schedulers_[idx]->post(std::move(f));
}

void MultiScheduler::run()
{
    //遍历调度器数组
    for (auto &sched : schedulers_)
    {
        threads_.emplace_back([&sched]()
                              { sched->run(); });
    }
    for (auto &t : threads_)
        if (t.joinable())
            t.join();
}

template class Channel<int>;
template class Channel<std::string>;