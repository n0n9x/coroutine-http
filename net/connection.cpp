#include "connection.h"
#include <stdexcept>

// ── 构造 / 析构 ──────────────────────────────────────────────

Connection::Connection(int fd, Scheduler& sched)
    : fd_(fd), sched_(sched), closed_(false)
{}

Connection::~Connection() {
    close();
}

Connection::Connection(Connection&& other) noexcept
    : fd_(other.fd_), sched_(other.sched_),
      closed_(other.closed_), read_buf_(std::move(other.read_buf_))
{
    other.fd_     = -1;
    other.closed_ = true;
}

Connection& Connection::operator=(Connection&& other) noexcept {
    if (this != &other) {
        close();
        fd_       = other.fd_;
        closed_   = other.closed_;
        read_buf_ = std::move(other.read_buf_);
        other.fd_     = -1;
        other.closed_ = true;
    }
    return *this;
}

// ── close ────────────────────────────────────────────────────

void Connection::close() {
    if (!closed_ && fd_ >= 0) {
        //全局作用域解析符 调用Linux 系统内核提供的原生 close(int fd)
        ::close(fd_);
        fd_     = -1;
        closed_ = true;
    }
}

// ── read ─────────────────────────────────────────────────────

/**
 * 读取数据核心逻辑：
 *
 *   1. 先尝试直接 recv（非阻塞，fd 已设置 O_NONBLOCK）
 *   2. 若返回 EAGAIN（当前无数据），挂起协程等待 EPOLLIN
 *   3. epoll 触发后恢复协程，再次 recv
 *   4. 返回 0（EOF）→ 标记连接关闭
 */
std::string Connection::read(size_t max_bytes) {
    if (closed_) return "";

    char buf[max_bytes];

    while (true) {
        ssize_t n = recv(fd_, buf, max_bytes, 0);

        if (n > 0) {
            return std::string(buf, n);
        }

        if (n == 0) {
            // 对端正常关闭（EOF）
            closed_ = true;
            return "";
        }

        // n < 0
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // 当前无数据，挂起等待可读事件
            sched_.wait_event(fd_, EPOLLIN);
            if (closed_) return "";
            continue; // 唤醒后重试
        }

        // 真实错误（连接被 reset 等）
        closed_ = true;
        return "";
    }
}

// ── write ────────────────────────────────────────────────────

/**
 * 发送数据核心逻辑（处理部分写）：
 *
 *   循环 send，直到所有数据发完：
 *   - 若返回 EAGAIN（发送缓冲区满），挂起等待 EPOLLOUT
 *   - 唤醒后继续发送剩余数据
 */
ssize_t Connection::write(const std::string& data) {
    if (closed_ || data.empty()) return 0;

    size_t  total  = data.size();
    size_t  sent   = 0;
    const char* ptr = data.data();

    while (sent < total) {
        // 此处的send是系统调用
        ssize_t n = send(fd_, ptr + sent, total - sent, MSG_NOSIGNAL);

        if (n > 0) {
            sent += n;
            continue;
        }

        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // 发送缓冲区满，等待可写
            sched_.wait_event(fd_, EPOLLOUT);
            if (closed_) return static_cast<ssize_t>(sent);
            continue;
        }

        // 真实错误
        closed_ = true;
        return sent > 0 ? static_cast<ssize_t>(sent) : -1;
    }

    return static_cast<ssize_t>(sent);
}

// ── read_until ───────────────────────────────────────────────

/**
 * 读到分隔符为止。
 *
 * 维护一个内部 read_buf_，每次从网络读一批数据追加进去，
 * 然后检查是否已包含分隔符。
 * 找到后把分隔符之前（含分隔符）的部分返回，剩余留在缓冲区。
 *
 * 这里的设计关键：read_buf_ 是成员变量，多次调用 read_until
 * 之间共享缓冲，不会丢失已读但未消费的数据。
 */
std::string Connection::read_until(const std::string& delim) {
    while (true) {
        // 先在已有缓冲区里找
        size_t pos = read_buf_.find(delim);
        if (pos != std::string::npos) {
            // 截取到分隔符末尾
            std::string result = read_buf_.substr(0, pos + delim.size());
            read_buf_.erase(0, pos + delim.size());
            return result;
        }

        // 缓冲区里还没有，继续从网络读
        std::string chunk = read(4096);
        if (chunk.empty()) {
            // 对端关闭，返回剩余内容（可能是不完整的数据）
            std::string rest;
            rest.swap(read_buf_);
            return rest;
        }
        read_buf_ += chunk;
    }
}

// ── read_exact ───────────────────────────────────────────────

std::string Connection::read_exact(size_t n) {
    // 先消耗 read_buf_ 里已有的数据
    while (read_buf_.size() < n) {
        std::string chunk = read(4096);
        if (chunk.empty()) break; // EOF
        read_buf_ += chunk;
    }

    if (read_buf_.size() >= n) {
        std::string result = read_buf_.substr(0, n);
        read_buf_.erase(0, n);
        return result;
    }

    // 对端提前关闭，返回能读到的所有数据
    std::string rest;
    rest.swap(read_buf_);
    return rest;
}