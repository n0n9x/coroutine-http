#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

/**
 * http_server.h — HTTP 路由器 + 服务器
 *
 * 在 TcpServer 之上封装 HTTP 语义：
 *   - 注册路由（method + path pattern）
 *   - 支持路径参数：/user/:id
 *   - 每个请求解析完成后匹配路由，调用对应 Handler
 *   - 支持 Keep-Alive（同一连接处理多个请求）
 *   - 404 / 405 默认响应
 *
 * 典型用法：
 *   HttpServer app(sched);
 *
 *   app.get("/hello", [](HttpRequest& req, HttpResponse& res) {
 *       res.text(conn, "Hello!");   // ← conn 由框架传入
 *   });
 *
 *   app.post("/echo", [](HttpRequest& req, HttpResponse& res, Connection& conn) {
 *       res.json(conn, req.body);
 *   });
 *
 *   app.listen(8888);
 *   sched.run();
 */

#include "../core/coroutine.h"
#include "../net/tcp_server.h"
#include "http_request.h"
#include "http_response.h"
#include <memory>
#include <functional>
#include <vector>
#include <string>

// Handler 签名：请求、响应、连接 三件套
using HttpHandler = std::function<void(HttpRequest &, HttpResponse &, Connection &)>;

// ── 路由条目 ──────────────────────────────────────────────────
// 它决定了当客户端（浏览器）访问不同的 URL 地址时，服务器应该执行哪一段代码
struct Route
{
    std::string method;  // "GET" / "POST" / "*"（匹配任意）
    std::string pattern; // 路由模式  "/user/:id"  "/static/*"
    HttpHandler handler;

    /**
     * 尝试将 path 与 pattern 匹配。
     *
     * 规则：
     *   - ":name"  匹配一个路径段，结果写入 params
     *   - "*"      匹配剩余所有路径（通配，用于静态文件等）
     *   - 其余字符 精确匹配
     *
     * @param path    实际请求路径，如 "/user/42"
     * @param params  输出：提取到的路径参数
     * @return        是否匹配成功
     */
    bool match(const std::string &path,
               std::unordered_map<std::string, std::string> &params) const;
};

// ── HttpServer ────────────────────────────────────────────────
class HttpServer
{
public:
    explicit HttpServer(Scheduler &sched);

    // ── 路由注册 ─────────────────────────────────

    HttpServer &get(const std::string &path, HttpHandler handler);  // 获取资源
    HttpServer &post(const std::string &path, HttpHandler handler); // 提交数据
    HttpServer &put(const std::string &path, HttpHandler handler);  // 更新资源
    HttpServer &del(const std::string &path, HttpHandler handler);  // 删除资源
    // 路由注册函数
    HttpServer &route(const std::string &method,
                      const std::string &path, HttpHandler handler);

    // ── 中间件（前置处理，按注册顺序依次执行）───
    // 返回 false 则终止后续处理（可用于鉴权、限流等）
    using Middleware = std::function<bool(HttpRequest &, HttpResponse &, Connection &)>;
    // 添加中间件
    HttpServer &use(Middleware mw);

    // ── 启动 ─────────────────────────────────────

    /**
     * 绑定端口并向调度器注册监听协程。
     * 需要在之后调用 sched.run() 才会真正处理请求。
     */
    using Dispatcher = std::function<void(std::function<void()>)>;
    void listen(uint16_t port, Dispatcher dispatcher = nullptr, int backlog = 128);

private:
    Scheduler &sched_;                      // 调度器
    std::vector<Route> routes_;             // 路由表
    std::vector<Middleware> middlewares_;   // 各个中间件函数
    std::shared_ptr<TcpServer> tcp_server_; // tcp服务器
    Dispatcher dispatcher_;                 // 分发器函数

    /** 连接处理，处理单个 TCP 连接（含 Keep-Alive 循环） */
    void handle_connection(Connection conn);

    /** 路由分发：找到匹配的 Route 并调用 Handler */
    void dispatch(HttpRequest &req, HttpResponse &res, Connection &conn);

    /** 发送默认错误响应 */
    static void send_error(Connection &conn, int code, const std::string &msg);
};

#endif // HTTP_SERVER_H