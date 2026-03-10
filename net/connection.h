#ifndef CONNECTION_H
#define CONNECTION_H

/**
 * connection.h — 封装单个 TCP 客户端连接
 *
 * 提供协程友好的 read / write 接口：
 *   - 内部自动调用 sched.wait_event() 挂起协程等待 I/O
 *   - 调用方写起来和同步代码一样，不感知 epoll
 *
 * 典型用法（在协程内）：
 *   Connection conn(fd, sched);
 *   std::string data = conn.read();
 *   conn.write("HTTP/1.1 200 OK\r\n\r\nHello");
 */

#include "../core/coroutine.h"
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

class Connection {
public:
    // ── 构造 / 析构 ──────────────────────────────

    Connection(int fd, Scheduler& sched);
    ~Connection();

    // 禁止拷贝，允许移动（fd 所有权转移）
    Connection(const Connection&)            = delete;
    Connection& operator=(const Connection&) = delete;
    Connection(Connection&& other) noexcept;
    Connection& operator=(Connection&& other) noexcept;

    // ── 核心 I/O 接口 ────────────────────────────

    /**
     * 读取数据，返回本次读到的内容。
     *
     * - 若当前无数据可读，挂起协程等待 EPOLLIN，数据就绪后恢复
     * - 返回空字符串表示对端已关闭连接（EOF）
     * - 出错时同样返回空字符串，可通过 is_closed() 判断
     *
     * @param max_bytes 单次最多读取的字节数（默认 4096）
     */
    std::string read(size_t max_bytes = 4096);

    /**
     * 发送数据，保证全部写出（处理部分写）。
     *
     * - 若发送缓冲区满，挂起协程等待 EPOLLOUT，可写后继续
     * - 返回实际发送字节数；-1 表示出错
     *
     * @param data 要发送的数据
     */
    ssize_t write(const std::string& data);

    /**
     * 读取数据直到遇到指定分隔符（常用于读一行 HTTP header）。
     *
     * - 内部循环调用 read()，直到缓冲区中出现 delim
     * - 返回包含 delim 在内的完整数据段
     * - 对端关闭时返回缓冲区中剩余内容
     *
     * @param delim 分隔符，默认 "\r\n"（HTTP 行尾）
     */
    std::string read_until(const std::string& delim = "\r\n");

    /**
     * 读取恰好 n 个字节（常用于读 HTTP Body）。
     *
     * - 内部循环调用 read() 直到凑够 n 字节或对端关闭
     */
    std::string read_exact(size_t n);

    // ── 状态查询 ─────────────────────────────────

    /** 连接是否已关闭（对端 EOF 或出错） */
    bool is_closed() const { return closed_; }

    /** 获取底层 fd */
    int fd() const { return fd_; }

    /** 主动关闭连接 */
    void close();

private:
    int         fd_;
    Scheduler&  sched_;
    bool        closed_;
    std::string read_buf_;  // 读缓冲区（存放尚未被上层消费的数据）
};

#endif // CONNECTION_H