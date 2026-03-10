#ifndef COROUTINE_H
#define COROUTINE_H

#include <iostream>
#include <deque>
#include <map>
#include <vector>
#include <functional>
#include <ucontext.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <stdint.h>
#include <chrono>
#include <cassert>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <thread>
#include <atomic>

static constexpr size_t CO_STACK_SIZE = 128 * 1024;

enum class State { READY, RUNNING, WAITING, DONE };

struct Coroutine {
    ucontext_t            ctx;
    char*                 stack;
    std::function<void()> func;
    State                 state;
    uint64_t              coro_id;

    explicit Coroutine(std::function<void()> f, uint64_t id);
    ~Coroutine();

    Coroutine(const Coroutine&)            = delete;
    Coroutine& operator=(const Coroutine&) = delete;
};

struct TimerEntry {
    int64_t    expire_ms;
    Coroutine* co;
    bool operator>(const TimerEntry& o) const { return expire_ms > o.expire_ms; }
};

class Scheduler {
public:
    explicit Scheduler(int id = 0);
    ~Scheduler();

    Scheduler(const Scheduler&)            = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    Coroutine* create(std::function<void()> f);

    /**
     * 线程安全投递：从任意线程向本调度器提交协程任务。
     * 用 pipe 唤醒 epoll_wait。
     */
    void post(std::function<void()> f);

    void     run();
    void     yield();
    uint32_t wait_event(int fd, uint32_t events);
    void     sleep(int64_t ms);

    static void co_entry(uint32_t low, uint32_t high);
    Coroutine*  current() const { return running_coro; }
    int         id()      const { return id_; }

    template<typename T> friend class Channel;

private:
    int                         id_;
    ucontext_t                  main_ctx;
    std::deque<Coroutine*>      ready_queue;
    std::map<int, Coroutine*>   waiting_map;
    std::vector<TimerEntry>     timer_heap;
    Coroutine*                  running_coro;
    int                         epoll_fd;
    uint64_t                    next_id;

    int        pipe_read_fd_;
    int        pipe_write_fd_;
    std::mutex post_mutex_;
    std::deque<std::function<void()>> post_queue_;

    void drain_post_queue();
    void push_timer(TimerEntry entry);
    TimerEntry pop_timer();
    int  fire_timers();
    int  next_timeout_ms() const;
    static int64_t now_ms();
};

// ── 多线程调度器管理器 ────────────────────────────────
class MultiScheduler {
public:
    explicit MultiScheduler(int num_threads);
    ~MultiScheduler();

    void post(std::function<void()> f);  // 轮询分发
    void run();                           // 启动所有线程

    Scheduler& get(int i) { return *schedulers_[i]; }
    int size() const { return static_cast<int>(schedulers_.size()); }

private:
    std::vector<std::unique_ptr<Scheduler>> schedulers_;
    std::vector<std::thread>                threads_;
    std::atomic<int>                        rr_index_{0};
};

template<typename T>
class Channel {
public:
    explicit Channel(Scheduler& sched) : sched_(sched) {}

    void send(T val) {
        Coroutine* self = sched_.current();
        assert(self && "Channel::send called outside coroutine");
        if (recv_waiter_) {
            *recv_slot_  = std::move(val);
            recv_slot_   = nullptr;
            Coroutine* r = recv_waiter_;
            recv_waiter_ = nullptr;
            r->state     = State::READY;
            sched_.ready_queue.push_back(r);
            sched_.yield();
        } else {
            send_val_           = std::move(val);
            send_waiter_        = self;
            self->state         = State::WAITING;
            Coroutine* cur      = self;
            sched_.running_coro = nullptr;
            swapcontext(&cur->ctx, &sched_.main_ctx);
        }
    }

    T recv() {
        Coroutine* self = sched_.current();
        assert(self && "Channel::recv called outside coroutine");
        if (send_waiter_) {
            T val        = std::move(send_val_);
            Coroutine* s = send_waiter_;
            send_waiter_ = nullptr;
            s->state     = State::READY;
            sched_.ready_queue.push_back(s);
            return val;
        } else {
            T result{};
            recv_slot_          = &result;
            recv_waiter_        = self;
            self->state         = State::WAITING;
            Coroutine* cur      = self;
            sched_.running_coro = nullptr;
            swapcontext(&cur->ctx, &sched_.main_ctx);
            return result;
        }
    }

private:
    Scheduler& sched_;
    Coroutine* send_waiter_ = nullptr;
    T          send_val_{};
    Coroutine* recv_waiter_ = nullptr;
    T*         recv_slot_   = nullptr;
};

// 每线程当前调度器指针，供 tcp_server 等模块访问
extern thread_local Scheduler* tl_scheduler;

#endif // COROUTINE_H