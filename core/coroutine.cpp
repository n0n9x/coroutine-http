#include "coroutine.h"
#include <fcntl.h>
#include <algorithm>
#include <stdexcept>

// ── 每线程调度器指针（co_entry 蹦床用）────────────────
thread_local Scheduler* tl_scheduler = nullptr;

// ============================================================
//  Coroutine
// ============================================================

Coroutine::Coroutine(std::function<void()> f, uint64_t id)
    : stack(nullptr), func(std::move(f)), state(State::READY), coro_id(id)
{
    stack = new char[CO_STACK_SIZE];
    if (getcontext(&ctx) != 0) {
        delete[] stack; stack = nullptr;
        throw std::runtime_error("getcontext failed");
    }
    ctx.uc_stack.ss_sp   = stack;
    ctx.uc_stack.ss_size = CO_STACK_SIZE;
    ctx.uc_link          = nullptr;

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
//  Scheduler
// ============================================================

Scheduler::Scheduler(int id)
    : id_(id), running_coro(nullptr), epoll_fd(-1), next_id(1),
      pipe_read_fd_(-1), pipe_write_fd_(-1)
{
    epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0)
        throw std::runtime_error("epoll_create1 failed");

    // 创建 pipe，用于跨线程唤醒 epoll_wait
    int fds[2];
    if (pipe2(fds, O_NONBLOCK | O_CLOEXEC) < 0)
        throw std::runtime_error("pipe2 failed");
    pipe_read_fd_  = fds[0];
    pipe_write_fd_ = fds[1];

    // 将 pipe 读端注册到 epoll
    struct epoll_event ev{};
    ev.events  = EPOLLIN;
    ev.data.fd = pipe_read_fd_;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, pipe_read_fd_, &ev);
}

Scheduler::~Scheduler() {
    for (auto* co : ready_queue) delete co;
    ready_queue.clear();
    for (auto& [fd, co] : waiting_map) {
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
        delete co;
    }
    waiting_map.clear();
    for (auto& e : timer_heap) delete e.co;
    timer_heap.clear();

    if (pipe_read_fd_  >= 0) { close(pipe_read_fd_);  pipe_read_fd_  = -1; }
    if (pipe_write_fd_ >= 0) { close(pipe_write_fd_); pipe_write_fd_ = -1; }
    if (epoll_fd       >= 0) { close(epoll_fd);        epoll_fd       = -1; }
}

// ── co_entry ──────────────────────────────────────────────

void Scheduler::co_entry(uint32_t low, uint32_t high) {
    uintptr_t ptr = static_cast<uintptr_t>(low)
                  | (static_cast<uintptr_t>(high) << 32);
    Coroutine* co = reinterpret_cast<Coroutine*>(ptr);

    try {
        if (co->func) co->func();
    } catch (const std::exception& e) {
        std::cerr << "[co#" << co->coro_id << "] exception: " << e.what() << "\n";
    } catch (...) {
        std::cerr << "[co#" << co->coro_id << "] unknown exception\n";
    }

    co->state = State::DONE;
    assert(tl_scheduler);
    setcontext(&tl_scheduler->main_ctx);
    __builtin_unreachable();
}

// ── create / post ─────────────────────────────────────────

Coroutine* Scheduler::create(std::function<void()> f) {
    auto* co = new Coroutine(std::move(f), next_id++);
    ready_queue.push_back(co);
    return co;
}

void Scheduler::post(std::function<void()> f) {
    {
        std::lock_guard<std::mutex> lk(post_mutex_);
        post_queue_.push_back(std::move(f));
    }
    // 写一个字节唤醒 epoll_wait
    char c = 1;
    write(pipe_write_fd_, &c, 1);
}

void Scheduler::drain_post_queue() {
    std::deque<std::function<void()>> local;
    {
        std::lock_guard<std::mutex> lk(post_mutex_);
        local.swap(post_queue_);
    }
    for (auto& f : local)
        create(std::move(f));
}

// ── yield ─────────────────────────────────────────────────

void Scheduler::yield() {
    if (!running_coro) return;
    Coroutine* cur = running_coro;
    cur->state     = State::READY;
    ready_queue.push_back(cur);
    running_coro   = nullptr;
    swapcontext(&cur->ctx, &main_ctx);
}

// ── wait_event ────────────────────────────────────────────

uint32_t Scheduler::wait_event(int fd, uint32_t events) {
    if (!running_coro) return 0;

    struct epoll_event ev{};
    ev.events  = events | EPOLLONESHOT | EPOLLET;
    ev.data.fd = fd;

    if (waiting_map.count(fd)) {
        epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev);
    } else {
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0 && errno == EEXIST)
            epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev);
    }

    Coroutine* cur  = running_coro;
    cur->state      = State::WAITING;
    waiting_map[fd] = cur;
    running_coro    = nullptr;
    swapcontext(&cur->ctx, &main_ctx);

    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
    return 0;
}

// ── sleep ─────────────────────────────────────────────────

void Scheduler::sleep(int64_t ms) {
    if (!running_coro) return;
    if (ms <= 0) { yield(); return; }

    Coroutine* cur = running_coro;
    cur->state     = State::WAITING;
    push_timer({ now_ms() + ms, cur });
    running_coro   = nullptr;
    swapcontext(&cur->ctx, &main_ctx);
}

// ── run ───────────────────────────────────────────────────

void Scheduler::run() {
    tl_scheduler = this;

    while (true) {
        // 1. 执行就绪协程
        while (!ready_queue.empty()) {
            Coroutine* co = ready_queue.front();
            ready_queue.pop_front();
            running_coro = co;
            co->state    = State::RUNNING;
            swapcontext(&main_ctx, &co->ctx);
            if (running_coro && running_coro->state == State::DONE) {
                delete running_coro;
            }
            running_coro = nullptr;
        }

        // 2. 定时器
        fire_timers();
        if (!ready_queue.empty()) continue;

        // 3. epoll_wait
        struct epoll_event evs[64];
        int timeout = next_timeout_ms();
        int n = epoll_wait(epoll_fd, evs, 64, timeout < 0 ? 50 : timeout);

        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }

        // 4. 唤醒 I/O 协程 / 处理跨线程投递
        for (int i = 0; i < n; i++) {
            int fd = evs[i].data.fd;

            if (fd == pipe_read_fd_) {
                // 清空 pipe，然后把投递的任务变成协程
                char buf[64];
                while (read(pipe_read_fd_, buf, sizeof(buf)) > 0) {}
                drain_post_queue();
                continue;
            }

            auto it = waiting_map.find(fd);
            if (it != waiting_map.end()) {
                Coroutine* co = it->second;
                waiting_map.erase(it);
                co->state = State::READY;
                ready_queue.push_back(co);
            }
        }

        fire_timers();
    }
}

// ── 定时器 ────────────────────────────────────────────────

int64_t Scheduler::now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()).count();
}

void Scheduler::push_timer(TimerEntry entry) {
    timer_heap.push_back(entry);
    std::push_heap(timer_heap.begin(), timer_heap.end(),
                   [](const TimerEntry& a, const TimerEntry& b) {
                       return a.expire_ms > b.expire_ms;
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
    if (timer_heap.empty()) return -1;
    int64_t diff = timer_heap.front().expire_ms - now_ms();
    return static_cast<int>(diff > 0 ? diff : 0);
}

// ============================================================
//  MultiScheduler
// ============================================================

MultiScheduler::MultiScheduler(int num_threads) {
    for (int i = 0; i < num_threads; i++)
        schedulers_.push_back(std::make_unique<Scheduler>(i));
}

MultiScheduler::~MultiScheduler() {
    // 线程会在 run() 里一直跑，析构时直接 detach
    for (auto& t : threads_)
        if (t.joinable()) t.detach();
}

void MultiScheduler::post(std::function<void()> f) {
    // round-robin 分发
    int idx = rr_index_.fetch_add(1) % static_cast<int>(schedulers_.size());
    schedulers_[idx]->post(std::move(f));
}

void MultiScheduler::run() {
    for (auto& sched : schedulers_) {
        threads_.emplace_back([&sched]() { sched->run(); });
    }
    for (auto& t : threads_)
        if (t.joinable()) t.join();
}

template class Channel<int>;
template class Channel<std::string>;