#ifndef TCP_SERVER_H
#define TCP_SERVER_H

#include "../core/coroutine.h"
#include "connection.h"
#include <functional>
#include <netinet/in.h>

class TcpServer {
public:
    using ConnectHandler = std::function<void(Connection)>;
    // 分发器：把一个任务投递到某个 Scheduler（可以是 MultiScheduler::post）
    using Dispatcher = std::function<void(std::function<void()>)>;

    TcpServer(Scheduler& accept_sched, uint16_t port,
              Dispatcher dispatcher = nullptr, int backlog = 128);
    ~TcpServer();

    TcpServer(const TcpServer&)            = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    TcpServer& on_connect(ConnectHandler handler);
    void start();
    uint16_t port() const { return port_; }

private:
    Scheduler&     accept_sched_;  // accept 协程跑在这个 Scheduler
    uint16_t       port_;
    int            backlog_;
    int            listen_fd_;
    ConnectHandler handler_;
    Dispatcher     dispatcher_;    // 新连接分发给哪个 Scheduler

    void accept_loop();
    void init_socket();
    static void set_nonblocking(int fd);
    static void set_reuseaddr(int fd);
};

#endif