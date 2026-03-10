#include "http/http_server.h"

int main() {
    Scheduler sched;
    HttpServer app(sched);

    // GET /hello → 纯文本响应
    app.get("/hello", [](HttpRequest& req, HttpResponse& res, Connection& conn) {
        res.status(200).text(conn, "Hello, World!\n");
    });

    // GET /ping → JSON 响应
    app.get("/ping", [](HttpRequest& req, HttpResponse& res, Connection& conn) {
        res.json(conn, "{\"status\":\"ok\"}");
    });

    // GET /user/:id → 路径参数
    app.get("/user/:id", [](HttpRequest& req, HttpResponse& res, Connection& conn) {
        std::string id = req.param("id");
        res.json(conn, "{\"id\":\"" + id + "\",\"name\":\"user_" + id + "\"}");
    });

    // POST /echo → 读取 body 原样返回
    app.post("/echo", [](HttpRequest& req, HttpResponse& res, Connection& conn) {
        res.status(200).text(conn, req.body);
    });

    // GET /search?q=xxx → query 参数
    app.get("/search", [](HttpRequest& req, HttpResponse& res, Connection& conn) {
        std::string q = req.query_param("q", "(empty)");
        res.json(conn, "{\"query\":\"" + q + "\"}");
    });

    app.listen(8888);
    sched.run();
    return 0;
}