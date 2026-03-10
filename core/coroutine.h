#ifndef COROUTINE_H
#define COROUTINE_H

/**
 * coroutine.h — 基于 ucontext + epoll 的 C++ 用户态协程调度器
 *
 * 架构概述：
 *   Coroutine  : 代表一个用户态协程，持有独立栈、上下文、状态
 *   Scheduler  : 单线程调度器，维护就绪队列 + epoll 等待表 + 定时器堆
 *   Channel<T> : 协程间同步通信原语（生产者/消费者，无缓冲）
 *
 * 调度流程：
 *   run() → 取就绪协程执行 → 协程调用 yield/wait_event/sleep 主动挂起
 *        → swapcontext 回到调度器主上下文 → epoll_wait 等待 I/O 或定时器到期
 *        → 将就绪协程重新入队 → 循环
 */

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

// ────────────────────────────────────────────────
//  协程栈大小（默认 128 KB，可按需调整）
// ────────────────────────────────────────────────
static constexpr size_t CO_STACK_SIZE = 128 * 1024;

// ────────────────────────────────────────────────
//  协程状态机
//  READY   → 等待被调度器选中执行
//  RUNNING → 当前正在 CPU 上运行
//  WAITING → 挂起等待 I/O 事件或定时器
//  DONE    → 函数已返回，等待销毁
// ────────────────────────────────────────────────
enum class State { READY, RUNNING, WAITING, DONE };

// ────────────────────────────────────────────────
//  Coroutine：协程对象
// ────────────────────────────────────────────────
struct Coroutine {
    ucontext_t          ctx;        // 保存/恢复执行上下文
    char*               stack;      // 独立栈空间
    std::function<void()> func;     // 协程函数体
    State               state;      // 当前状态
    uint64_t            coro_id;    // 唯一 ID（调试用）

    explicit Coroutine(std::function<void()> f, uint64_t id);
    ~Coroutine();

    // 禁止拷贝（栈指针不可共享）
    Coroutine(const Coroutine&)            = delete;
    Coroutine& operator=(const Coroutine&) = delete;
};

// ────────────────────────────────────────────────
//  定时器条目（最小堆节点）
// ────────────────────────────────────────────────
struct TimerEntry {
    int64_t     expire_ms;  // 绝对到期时间（毫秒）
    Coroutine*  co;

    // 最小堆比较：到期时间小的优先
    bool operator>(const TimerEntry& o) const { return expire_ms > o.expire_ms; }
};

// ────────────────────────────────────────────────
//  Scheduler：协程调度器（单例友好，但非强制）
// ────────────────────────────────────────────────
class Scheduler {
public:
    Scheduler();
    ~Scheduler();

    // 禁止拷贝
    Scheduler(const Scheduler&)            = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    // ── 对外接口 ──────────────────────────────────

    /** 创建并入队一个新协程 */
    Coroutine* create(std::function<void()> f);

    /** 启动事件循环，直到所有协程结束 */
    void run();

    /** 主动让出 CPU，当前协程重新入就绪队列 */
    void yield();

    /**
     * 挂起当前协程，等待 fd 上的 epoll 事件。
     * @param fd     要监听的文件描述符
     * @param events epoll 事件掩码（EPOLLIN / EPOLLOUT 等）
     * @return       实际触发的 epoll 事件掩码；若调度器未运行返回 0
     */
    uint32_t wait_event(int fd, uint32_t events);

    /**
     * 挂起当前协程，至少睡眠 ms 毫秒后恢复。
     * @param ms 睡眠时长（毫秒），0 表示仅让出本轮时间片
     */
    void sleep(int64_t ms);

    // ── 内部辅助（public 供 co_entry 访问）────────

    /** 协程入口蹦床（makecontext 要求普通函数） */
    static void co_entry(uint32_t low, uint32_t high);

    /** 返回当前正在运行的协程指针（可为 nullptr） */
    Coroutine* current() const { return running_coro; }

    template<typename T> friend class Channel;

private:
    // ── 内部数据 ──────────────────────────────────
    ucontext_t                  main_ctx;       // 调度器主上下文
    std::deque<Coroutine*>      ready_queue;    // 就绪队列（FIFO）
    std::map<int, Coroutine*>   waiting_map;    // fd → 等待协程
    std::vector<TimerEntry>     timer_heap;     // 定时器最小堆
    Coroutine*                  running_coro;   // 当前运行中的协程
    int                         epoll_fd;       // epoll 实例
    uint64_t                    next_id;        // 协程 ID 计数器

    // ── 内部方法 ──────────────────────────────────

    /** 将定时器条目压入最小堆 */
    void push_timer(TimerEntry entry);

    /** 弹出堆顶（最早到期）定时器 */
    TimerEntry pop_timer();

    /** 处理所有已到期的定时器，返回唤醒的协程数 */
    int  fire_timers();

    /** 计算距最近定时器到期的毫秒数；无定时器时返回 -1（永久阻塞） */
    int  next_timeout_ms() const;

    /** 获取当前时间戳（毫秒） */
    static int64_t now_ms();
};

// ────────────────────────────────────────────────
//  Channel<T>：协程间无缓冲同步通信
//
//  send(val)  : 若有接收方等待则直接交付并唤醒；否则发送方挂起
//  recv()     : 若有发送方等待则取出并唤醒；否则接收方挂起
//
//  实现原理：
//    挂起时直接 swapcontext 回调度器，不经过 yield（避免重复入队）。
//    对端到来时，将挂起的协程放入就绪队列后才 yield 自己。
//
//  注意：Channel 与 Scheduler 绑定，必须在同一调度器上使用。
// ────────────────────────────────────────────────
template<typename T>
class Channel {
public:
    explicit Channel(Scheduler& sched) : sched_(sched) {}

    /** 发送一个值，可能挂起直到接收方就绪 */
    void send(T val) {
        Coroutine* self = sched_.current();
        assert(self && "Channel::send called outside coroutine");

        if (recv_waiter_) {
            // 接收方已在等待：直接交付数据，唤醒接收方，然后让出
            *recv_slot_  = std::move(val);
            recv_slot_   = nullptr;
            Coroutine* r = recv_waiter_;
            recv_waiter_ = nullptr;
            r->state     = State::READY;
            requeue(r);
            sched_.yield(); // 自己让出，让接收方先跑
        } else {
            // 无接收方：保存值并真正挂起（直接 swap 回调度器，不入队）
            send_val_        = std::move(val);
            send_waiter_     = self;
            self->state      = State::WAITING;
            Coroutine* cur   = self;
            sched_.running_coro = nullptr;
            swapcontext(&cur->ctx, &sched_.main_ctx);
            // ← 被 recv() 唤醒后在此恢复
        }
    }

    /** 接收一个值，可能挂起直到发送方就绪 */
    T recv() {
        Coroutine* self = sched_.current();
        assert(self && "Channel::recv called outside coroutine");

        if (send_waiter_) {
            // 发送方已在等待：取出数据，唤醒发送方，立即返回
            T val        = std::move(send_val_);
            Coroutine* s = send_waiter_;
            send_waiter_ = nullptr;
            s->state     = State::READY;
            requeue(s);
            return val;
        } else {
            // 无发送方：挂起等待
            T result{};
            recv_slot_       = &result;
            recv_waiter_     = self;
            self->state      = State::WAITING;
            Coroutine* cur   = self;
            sched_.running_coro = nullptr;
            swapcontext(&cur->ctx, &sched_.main_ctx);
            // ← 被 send() 唤醒后在此恢复，result 已被填充
            return result;
        }
    }

private:
    Scheduler&  sched_;
    Coroutine*  send_waiter_ = nullptr;
    T           send_val_{};
    Coroutine*  recv_waiter_ = nullptr;
    T*          recv_slot_   = nullptr;

    void requeue(Coroutine* co) {
        sched_.ready_queue.push_back(co);
    }
};

#endif // COROUTINE_H