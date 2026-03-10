#include "tcp_server.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <stdexcept>
#include <iostream>
#include <cstring>

// thread_local 当前调度器指针（由 Scheduler::run() 设置）
// Connection 构造时用它绑定到正确的调度器
extern thread_local Scheduler* tl_scheduler;

TcpServer::TcpServer(Scheduler& accept_sched, uint16_t port,
                     Dispatcher dispatcher, int backlog)
    : accept_sched_(accept_sched), port_(port), backlog_(backlog),
      listen_fd_(-1), dispatcher_(dispatcher)
{}

TcpServer::~TcpServer() {
    if (listen_fd_ >= 0) { ::close(listen_fd_); listen_fd_ = -1; }
}

void TcpServer::set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void TcpServer::set_reuseaddr(int fd) {
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
}

void TcpServer::init_socket() {
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0)
        throw std::runtime_error("socket() failed: " + std::string(strerror(errno)));

    set_reuseaddr(listen_fd_);
    set_nonblocking(listen_fd_);

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port_);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
        throw std::runtime_error("bind() failed: " + std::string(strerror(errno)));

    if (listen(listen_fd_, backlog_) < 0)
        throw std::runtime_error("listen() failed: " + std::string(strerror(errno)));

    std::cout << "[TcpServer] 监听端口 " << port_ << "\n";
}

TcpServer& TcpServer::on_connect(ConnectHandler handler) {
    handler_ = std::move(handler);
    return *this;
}

void TcpServer::start() {
    if (!handler_) throw std::runtime_error("must call on_connect() first");
    init_socket();
    accept_sched_.create([this]() { accept_loop(); });
}

void TcpServer::accept_loop() {
    while (true) {
        accept_sched_.wait_event(listen_fd_, EPOLLIN);

        while (true) {
            sockaddr_in client_addr{};
            socklen_t   addr_len = sizeof(client_addr);
            int client_fd = accept(listen_fd_,
                                   reinterpret_cast<sockaddr*>(&client_addr),
                                   &addr_len);
            if (client_fd < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                std::cerr << "[TcpServer] accept error: " << strerror(errno) << "\n";
                break;
            }

            set_nonblocking(client_fd);

            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
            std::cout << "[TcpServer] 新连接 fd=" << client_fd
                      << " 来自 " << ip << ":" << ntohs(client_addr.sin_port) << "\n";

            // 把处理连接的任务投递到目标 Scheduler
            auto task = [this, client_fd]() {
                // tl_scheduler 是当前线程的调度器
                Scheduler& s = *tl_scheduler;
                Connection conn(client_fd, s);
                handler_(std::move(conn));
            };

            if (dispatcher_) {
                dispatcher_(task);  // 分发给 MultiScheduler 轮询的线程
            } else {
                accept_sched_.create(task);  // 单线程：在本 Scheduler 上创建
            }
        }
    }
}