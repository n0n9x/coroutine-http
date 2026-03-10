#include "tcp_server.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <stdexcept>
#include <iostream>
#include <cstring>

// ── 构造 / 析构 ──────────────────────────────────────────────

TcpServer::TcpServer(Scheduler& sched, uint16_t port, int backlog)
    : sched_(sched), port_(port), backlog_(backlog), listen_fd_(-1)
{}

TcpServer::~TcpServer() {
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
}

// ── 静态工具 ─────────────────────────────────────────────────

void TcpServer::set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) throw std::runtime_error("fcntl F_GETFL failed");
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        throw std::runtime_error("fcntl F_SETFL failed");
}

void TcpServer::set_reuseaddr(int fd) {
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
}

// ── init_socket ──────────────────────────────────────────────

void TcpServer::init_socket() {
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0)
        throw std::runtime_error(std::string("socket() failed: ") + strerror(errno));

    set_reuseaddr(listen_fd_);
    set_nonblocking(listen_fd_);

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port_);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
        throw std::runtime_error(std::string("bind() failed: ") + strerror(errno));

    if (listen(listen_fd_, backlog_) < 0)
        throw std::runtime_error(std::string("listen() failed: ") + strerror(errno));

    std::cout << "[TcpServer] 监听端口 " << port_ << " ...\n";
}

// ── on_connect / start ───────────────────────────────────────

TcpServer& TcpServer::on_connect(ConnectHandler handler) {
    handler_ = std::move(handler);
    return *this;
}

void TcpServer::start() {
    if (!handler_)
        throw std::runtime_error("TcpServer: 必须先调用 on_connect() 注册回调");

    init_socket();

    // 向调度器注册监听协程，start() 本身立即返回
    sched_.create([this]() { accept_loop(); });
}

// ── accept_loop ──────────────────────────────────────────────

/**
 * 监听协程主循环。
 *
 * 核心流程：
 *   1. wait_event(listen_fd, EPOLLIN) → 挂起，等待新连接到来
 *   2. 循环 accept 直到 EAGAIN（边缘触发下一次可能有多个连接积压）
 *   3. 为每个连接 create 一个新协程，执行用户的 handler_
 *   4. 回到步骤 1
 */
void TcpServer::accept_loop() {
    while (true) {
        // 挂起，等待 listen_fd 可读（有新连接排队）
        sched_.wait_event(listen_fd_, EPOLLIN);

        // 边缘触发：一次性 accept 所有积压的连接
        while (true) {
            sockaddr_in client_addr{};
            socklen_t   addr_len = sizeof(client_addr);

            int client_fd = accept(listen_fd_,
                                   reinterpret_cast<sockaddr*>(&client_addr),
                                   &addr_len);

            if (client_fd < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break; // 所有连接都 accept 完毕
                }
                std::cerr << "[TcpServer] accept() error: " << strerror(errno) << "\n";
                break;
            }

            set_nonblocking(client_fd);

            // 打印客户端信息（调试用）
            char ip_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, sizeof(ip_str));
            std::cout << "[TcpServer] 新连接 fd=" << client_fd
                      << " 来自 " << ip_str
                      << ":" << ntohs(client_addr.sin_port) << "\n";

            // 为每个连接创建独立协程
            // Connection 以移动语义传入 lambda，避免析构关闭 fd
            sched_.create([this, client_fd]() {
                Connection conn(client_fd, sched_);
                handler_(std::move(conn));
                // 协程结束后 conn 析构，自动 close(fd)
            });
        }
    }
}