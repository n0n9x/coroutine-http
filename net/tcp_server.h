#ifndef TCP_SERVER_H
#define TCP_SERVER_H

/**
 * tcp_server.h — TCP 监听服务器封装
 *
 * 职责：
 *   - 创建、绑定、监听 socket
 *   - 在协程内循环 accept 新连接
 *   - 每个新连接创建一个 Connection 对象，
 *     并 create 一个新协程调用用户注册的回调
 *
 * 典型用法：
 *   TcpServer server(sched, 8888);
 *   server.on_connect([](Connection conn) {
 *       std::string data = conn.read();
 *       conn.write(data);  // echo
 *   });
 *   server.start();  // 向调度器注册监听协程（非阻塞）
 *   sched.run();     // 启动事件循环
 */

#include "../core/coroutine.h"
#include "connection.h"
#include <functional>
#include <string>
#include <netinet/in.h>

class TcpServer {
public:
    // 连接回调类型：每个新连接触发一次，Connection 以移动语义传入
    using ConnectHandler = std::function<void(Connection)>;

    // ── 构造 / 析构 ──────────────────────────────

    /**
     * @param sched    调度器引用
     * @param port     监听端口
     * @param backlog  listen() 的 backlog 参数（默认 128）
     */
    TcpServer(Scheduler& sched, uint16_t port, int backlog = 128);
    ~TcpServer();

    TcpServer(const TcpServer&)            = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    // ── 接口 ─────────────────────────────────────

    /**
     * 注册新连接回调（必须在 start() 前调用）。
     * 每当有新客户端连接时，调度器会 create 一个新协程执行 handler。
     */
    TcpServer& on_connect(ConnectHandler handler);

    /**
     * 向调度器注册监听协程，立即返回（非阻塞）。
     * 需要在之后调用 sched.run() 才会真正开始处理连接。
     */
    void start();

    /** 获取监听端口 */
    uint16_t port() const { return port_; }

private:
    Scheduler&      sched_;
    uint16_t        port_;
    int             backlog_;
    int             listen_fd_;
    ConnectHandler  handler_;

    // 监听协程的主循环（由 start() 创建的协程执行）
    void accept_loop();

    // 初始化 listen socket（bind + listen + 设置非阻塞）
    void init_socket();

    static void set_nonblocking(int fd);
    static void set_reuseaddr(int fd);
};

#endif // TCP_SERVER_H