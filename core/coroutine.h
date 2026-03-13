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

enum class State
{
    READY,
    RUNNING,
    WAITING,
    DONE
};

struct Coroutine
{
    ucontext_t ctx; // 协程上下文
    char *stack;    // 协程栈
    std::function<void()> func;
    State state;
    uint64_t coro_id;

    // explicit 禁止隐式转换
    explicit Coroutine(std::function<void()> f, uint64_t id);
    ~Coroutine();

    Coroutine(const Coroutine &) = delete;            // 禁用拷贝构造
    Coroutine &operator=(const Coroutine &) = delete; // 禁用赋值操作符
};

struct TimerEntry
{
    int64_t expire_ms; // 终止时间
    Coroutine *co;     // 指向阻塞的协程
    // 重载运算符
    bool operator>(const TimerEntry &o) const { return expire_ms > o.expire_ms; }
};

class Scheduler
{
public:
    explicit Scheduler(int id = 0);
    ~Scheduler();

    Scheduler(const Scheduler &) = delete;            // 禁用拷贝构造
    Scheduler &operator=(const Scheduler &) = delete; // 禁用赋值操作符

    Coroutine *create(std::function<void()> f); // 在当前调度器线程创建一个新协程

    /**
     * 线程安全投递：从任意线程向本调度器提交协程任务。
     * 用 pipe 唤醒 epoll_wait。
     */
    void post(std::function<void()> f); // 跨线程投递

    void run();   // 调度器主循环
    void yield(); // 协程主动让出cpu，进就绪队列
    uint32_t wait_event(int fd, uint32_t events);
    void sleep(int64_t ms); // 协程主动阻塞，进阻塞队列

    // 所有协程的统一入口点
    static void co_entry(uint32_t low, uint32_t high);

    // 返回当前协程指针
    Coroutine *current() const { return running_coro; }
    // 返回当前调度器ID
    int id() const { return id_; }

    template <typename T>
    friend class Channel;

private:
    int id_;                 // 调度器编号。在多核并行时，用来区分当前任务跑在哪个核心上
    ucontext_t main_ctx;     // 主控制流的上下文
    Coroutine *running_coro; // 当前协程指针
    int epoll_fd;            // epoll 实例的文件描述符
    uint64_t next_id;        // 自增计数器

    std::deque<Coroutine *> ready_queue;    // 协程就绪队列
    std::map<int, Coroutine *> waiting_map; // IO等待表
    std::vector<TimerEntry> timer_heap;     // 定时器小根堆，堆顶为最早到期的任务

    int pipe_read_fd_;                             // 唤醒管道读端：注册到 epoll，用于接收跨线程唤醒信号
    int pipe_write_fd_;                            // 唤醒管道写端：其他线程通过写入字节，强行唤醒本线程的 epoll_wait
    std::mutex post_mutex_;                        // 互斥锁
    std::deque<std::function<void()>> post_queue_; // 外部任务暂存区

    void drain_post_queue(); // 提取post_queue所有任务转换为协程放入就绪队列

    // 小根堆操作函数
    void push_timer(TimerEntry entry); // 添加定时任务
    TimerEntry pop_timer();            // 取出最紧急的任务

    int fire_timers();           // 检查当前有哪些协程的“睡眠时间”到了，并把它们叫醒放入就绪队列
    int next_timeout_ms() const; // 计算调度器最长睡眠时间
    static int64_t now_ms();     // 获取当前系统的绝对时间
};

// ── 多线程调度器管理器 ────────────────────────────────
class MultiScheduler
{
public:
    explicit MultiScheduler(int num_threads); // 创建指定数量的调度器对象
    ~MultiScheduler();

    void post(std::function<void()> f); // 轮询分发
    void run();                         // 启动所有线程

    Scheduler &get(int i) { return *schedulers_[i]; }
    int size() const { return static_cast<int>(schedulers_.size()); }

private:
    std::vector<std::unique_ptr<Scheduler>> schedulers_; // 调度器数组
    std::vector<std::thread> threads_;                   // 物理线程池
    std::atomic<int> rr_index_{0};                       // 实现轮询调度
};

template <typename T>
class Channel
{
public:
    explicit Channel(Scheduler &sched) : sched_(sched) {}

    void send(T val) // 发送端
    {
        Coroutine *self = sched_.current();
        assert(self && "Channel::send called outside coroutine");
        if (recv_waiter_) // 有协程在等数据
        {
            *recv_slot_ = std::move(val); // 写进数据
            recv_slot_ = nullptr;         // 清理状态
            Coroutine *r = recv_waiter_;  // r指向接收协程
            recv_waiter_ = nullptr;       // 清理状态
            // 唤醒接收者协程
            r->state = State::READY;
            sched_.ready_queue.push_back(r);
            sched_.yield();
        }
        else // 没有协程在等数据
        {
            send_val_ = std::move(val);   // 将数据暂存在channel成员变量里保管
            send_waiter_ = self;          // 留下发送者协程指针
            self->state = State::WAITING; // 阻塞当前发送协程
            Coroutine *cur = self;
            sched_.running_coro = nullptr; // 该调度器上当前无运行进程
            // 将当前cpu上下文保存到cur->ctx中
            // 从 sched_.main_ctx中读取之前保存的上下文
            // cpu跳转到调度器run继续执行
            swapcontext(&cur->ctx, &sched_.main_ctx);
        }
    }

    T recv()
    {
        Coroutine *self = sched_.current();
        assert(self && "Channel::recv called outside coroutine");
        if (send_waiter_) // 已有发送协程被阻塞
        {
            T val = std::move(send_val_);
            Coroutine *s = send_waiter_;
            send_waiter_ = nullptr;
            s->state = State::READY;
            sched_.ready_queue.push_back(s);
            return val;
        }
        else // 无协程发送数据，阻塞自己知道有协程发数据
        {
            T result{};
            recv_slot_ = &result;
            recv_waiter_ = self;
            self->state = State::WAITING;
            Coroutine *cur = self;
            sched_.running_coro = nullptr;
            swapcontext(&cur->ctx, &sched_.main_ctx);
            return result;
        }
    }

private:
    Scheduler &sched_; // 调度器引用

    // 发送端缓冲区
    Coroutine *send_waiter_ = nullptr; // 存放发送协程指针
    T send_val_{};                     // 存放数据

    // 接收端缓冲区
    Coroutine *recv_waiter_ = nullptr; // 存放接收协程指针
    T *recv_slot_ = nullptr;           // 指向接收协程内部变量
};

// 每线程当前调度器指针，供 tcp_server 等模块访问
extern thread_local Scheduler *tl_scheduler;

#endif // COROUTINE_H