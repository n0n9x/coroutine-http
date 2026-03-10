#include "http_server.h"
#include "http_parser.h"
#include <iostream>
#include <sstream>
#include <memory>

// ── Route::match ─────────────────────────────────────────────

/**
 * 路径匹配算法。
 *
 * 将 pattern 和 path 都按 "/" 切割成段，逐段比对：
 *   - 普通段    ：精确匹配（大小写敏感）
 *   - ":name"   ：匹配任意一段，结果存入 params["name"]
 *   - "*"       ：匹配剩余所有内容（必须是最后一段）
 *
 * 示例：
 *   pattern="/user/:id/posts"  path="/user/42/posts"
 *   → 匹配，params={"id":"42"}
 *
 *   pattern="/static/*"        path="/static/css/app.css"
 *   → 匹配，params={"*":"css/app.css"}
 */
bool Route::match(const std::string& path,
                  std::unordered_map<std::string, std::string>& params) const {
    // 按 "/" 分割路径段的 lambda
    auto split = [](const std::string& s) {
        std::vector<std::string> parts;
        std::istringstream ss(s);
        std::string part;
        while (std::getline(ss, part, '/'))
            if (!part.empty()) parts.push_back(part);
        return parts;
    };

    auto p_parts = split(pattern);
    auto r_parts = split(path);

    size_t pi = 0, ri = 0;
    while (pi < p_parts.size() && ri < r_parts.size()) {
        const std::string& seg = p_parts[pi];

        if (seg == "*") {
            // 通配：把剩余 path 拼接成一个字符串存入 params["*"]
            std::string rest;
            for (size_t i = ri; i < r_parts.size(); i++) {
                if (i > ri) rest += "/";
                rest += r_parts[i];
            }
            params["*"] = rest;
            return true;
        } else if (!seg.empty() && seg[0] == ':') {
            // 路径参数：:name
            params[seg.substr(1)] = r_parts[ri];
        } else {
            // 普通段精确匹配
            if (seg != r_parts[ri]) return false;
        }
        pi++; ri++;
    }

    // 两边都走完才算完全匹配
    return pi == p_parts.size() && ri == r_parts.size();
}

// ── HttpServer ────────────────────────────────────────────────

HttpServer::HttpServer(Scheduler& sched) : sched_(sched) {}

// ── 路由注册 ─────────────────────────────────────────────────

HttpServer& HttpServer::get(const std::string& path, HttpHandler handler) {
    return route("GET", path, std::move(handler));
}
HttpServer& HttpServer::post(const std::string& path, HttpHandler handler) {
    return route("POST", path, std::move(handler));
}
HttpServer& HttpServer::put(const std::string& path, HttpHandler handler) {
    return route("PUT", path, std::move(handler));
}
HttpServer& HttpServer::del(const std::string& path, HttpHandler handler) {
    return route("DELETE", path, std::move(handler));
}
HttpServer& HttpServer::route(const std::string& method,
                               const std::string& path,
                               HttpHandler handler) {
    routes_.push_back({ method, path, std::move(handler) });
    return *this;
}

HttpServer& HttpServer::use(Middleware mw) {
    middlewares_.push_back(std::move(mw));
    return *this;
}

// ── listen ───────────────────────────────────────────────────

void HttpServer::listen(uint16_t port, int backlog) {
    // TcpServer 用 shared_ptr 管理，生命周期绑定到 lambda
    auto tcp = std::make_shared<TcpServer>(sched_, port, backlog);

    tcp->on_connect([this](Connection conn) {
        handle_connection(std::move(conn));
    });

    tcp->start();

    std::cout << "[HttpServer] 启动完成，监听端口 " << port << "\n"
              << "[HttpServer] 已注册路由 " << routes_.size() << " 条\n";
}

// ── handle_connection ─────────────────────────────────────────

/**
 * 处理单个连接的协程函数。
 *
 * Keep-Alive 循环：
 *   HTTP/1.1 默认复用连接，一个连接可能连续发多个请求。
 *   只有以下情况才退出循环：
 *     1. 客户端发送 "Connection: close"
 *     2. 解析失败（格式错误）
 *     3. 连接断开（read 返回空）
 */
void HttpServer::handle_connection(Connection conn) {
    while (!conn.is_closed()) {
        // ── 解析请求 ──────────────────────────────
        HttpRequest req;
        if (!HttpParser::parse(conn, req)) {
            // 解析失败：可能是连接断开，也可能是格式错误
            if (!conn.is_closed()) {
                send_error(conn, 400, "Bad Request");
            }
            break;
        }

        // ── 执行中间件 ────────────────────────────
        HttpResponse res;
        bool pass = true;
        for (auto& mw : middlewares_) {
            if (!mw(req, res, conn)) {
                pass = false;
                break;
            }
        }

        // ── 路由分发 ──────────────────────────────
        if (pass) {
            dispatch(req, res, conn);
        }

        // ── Keep-Alive 判断 ───────────────────────
        if (!req.keep_alive()) break;
    }
}

// ── dispatch ─────────────────────────────────────────────────

/**
 * 路由匹配：按注册顺序遍历路由表。
 *
 * 匹配逻辑：
 *   1. method 匹配（或路由注册的是 "*"）
 *   2. path pattern 匹配
 *   3. 两者都满足 → 调用 Handler，停止继续匹配
 *
 * 未匹配：
 *   - 有 path 匹配但 method 不对 → 405 Method Not Allowed
 *   - 完全没有匹配 → 404 Not Found
 */
void HttpServer::dispatch(HttpRequest& req, HttpResponse& res, Connection& conn) {
    bool path_matched = false;

    for (auto& r : routes_) {
        std::unordered_map<std::string, std::string> params;
        if (!r.match(req.path, params)) continue;

        path_matched = true;

        if (r.method != "*" && r.method != req.method) continue;

        // 找到匹配的路由，将路径参数写入请求对象
        req.params = std::move(params);

        try {
            r.handler(req, res, conn);
        } catch (const std::exception& e) {
            std::cerr << "[HttpServer] Handler 异常: " << e.what() << "\n";
            if (!conn.is_closed())
                send_error(conn, 500, "Internal Server Error");
        } catch (...) {
            std::cerr << "[HttpServer] Handler 未知异常\n";
            if (!conn.is_closed())
                send_error(conn, 500, "Internal Server Error");
        }
        return;
    }

    // 没有完全匹配
    if (path_matched) {
        send_error(conn, 405, "Method Not Allowed");
    } else {
        send_error(conn, 404, "Not Found");
    }
}

// ── send_error ───────────────────────────────────────────────

void HttpServer::send_error(Connection& conn, int code,
                             const std::string& msg) {
    HttpResponse res;
    std::string body = "{\"error\":\"" + msg + "\",\"code\":" +
                        std::to_string(code) + "}";
    res.status(code)
       .header("Connection", "close")
       .json(conn, body);
}