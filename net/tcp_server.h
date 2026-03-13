#ifndef TCP_SERVER_H
#define TCP_SERVER_H

#include "../core/coroutine.h"
#include "connection.h"
#include <functional>
#include <netinet/in.h>

class TcpServer
{
public:
    using ConnectHandler = std::function<void(Connection)>;
    // 分发器：把一个任务投递到某个 Scheduler
    using Dispatcher = std::function<void(std::function<void()>)>;

    TcpServer(Scheduler &accept_sched, uint16_t port,
              Dispatcher dispatcher = nullptr, int backlog = 128);
    ~TcpServer();

    TcpServer(const TcpServer &) = delete;
    TcpServer &operator=(const TcpServer &) = delete;

    TcpServer &on_connect(ConnectHandler handler);
    void start();
    uint16_t port() const { return port_; }

private:
    Scheduler &accept_sched_; // 监听调度器：负责执行 accept 协程
    uint16_t port_;           // 服务器监听的端口号
    int backlog_;             // 全连接队列最大长度
    int listen_fd_;           // 监听套接字
    ConnectHandler handler_;  // 分发函数
    Dispatcher dispatcher_;   // 分发器：新连接分发给哪个 Scheduler

    void accept_loop();
    void init_socket();                  // 封装端口为一个监听实体
    static void set_nonblocking(int fd); // 切换为非阻塞模式
    static void set_reuseaddr(int fd);   // 设置地址复用
};

#endif