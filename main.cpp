#include "http/http_server.h"
#include <fstream>
#include <sstream>

// 读取本地文件，失败返回空字符串
static std::string read_file(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// 根据扩展名返回 Content-Type
static std::string mime_type(const std::string& path) {
    if (path.rfind(".html") != std::string::npos) return "text/html; charset=utf-8";
    if (path.rfind(".css")  != std::string::npos) return "text/css";
    if (path.rfind(".js")   != std::string::npos) return "application/javascript";
    return "text/plain";
}

// 发送文件给客户端
static void send_file(const std::string& file_path,
                      HttpResponse& res, Connection& conn) {
    std::string body = read_file(file_path);
    if (body.empty()) {
        res.status(404).text(conn, "404 Not Found");
        return;
    }
    std::string ct = mime_type(file_path);
    conn.write("HTTP/1.1 200 OK\r\n"
               "Content-Type: "   + ct + "\r\n"
               "Content-Length: " + std::to_string(body.size()) + "\r\n"
               "Connection: keep-alive\r\n"
               "\r\n" + body);
}

int main() {
    Scheduler sched;
    HttpServer app(sched);

    // ── 静态文件 ──────────────────────────────────

    // 首页
    app.get("/", [](HttpRequest& /*req*/, HttpResponse& res, Connection& conn) {
        send_file("static/index.html", res, conn);
    });

    // 其他静态资源（css、js 等）
    app.get("/static/*", [](HttpRequest& req, HttpResponse& res, Connection& conn) {
        send_file("static/" + req.param("*"), res, conn);
    });

    // ── 后端 API ──────────────────────────────────

    // GET /api/ping → 服务器心跳
    app.get("/api/ping", [](HttpRequest& /*req*/, HttpResponse& res, Connection& conn) {
        res.json(conn, "{\"status\":\"ok\",\"server\":\"coroutine-http\"}");
    });

    // GET /api/users → 用户列表
    app.get("/api/users", [](HttpRequest& /*req*/, HttpResponse& res, Connection& conn) {
        res.json(conn, R"([
{"id":"1","name":"Alice","role":"admin"},
{"id":"2","name":"Bob","role":"user"},
{"id":"3","name":"Charlie","role":"user"}
])");
    });

    // GET /api/user/:id → 单个用户
    app.get("/api/user/:id", [](HttpRequest& req, HttpResponse& res, Connection& conn) {
        std::string id = req.param("id");
        if (id == "1") {
            res.json(conn, "{\"id\":\"1\",\"name\":\"Alice\",\"role\":\"admin\"}");
        } else if (id == "2") {
            res.json(conn, "{\"id\":\"2\",\"name\":\"Bob\",\"role\":\"user\"}");
        } else if (id == "3") {
            res.json(conn, "{\"id\":\"3\",\"name\":\"Charlie\",\"role\":\"user\"}");
        } else {
            res.status(404).json(conn, "{\"error\":\"用户不存在\",\"id\":\"" + id + "\"}");
        }
    });

    // POST /api/echo → 回显请求体
    app.post("/api/echo", [](HttpRequest& req, HttpResponse& res, Connection& conn) {
        res.json(conn, "{\"echo\":" + req.body + "}");
    });

    app.listen(8888);
    sched.run();
    return 0;
}