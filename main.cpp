#include "http/http_server.h"
#include "db/database.h"
#include "ws/websocket.h"
#include "ws/chat_room.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <thread>

static std::string read_file(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static std::string mime_type(const std::string& path) {
    if (path.rfind(".html") != std::string::npos) return "text/html; charset=utf-8";
    if (path.rfind(".css")  != std::string::npos) return "text/css";
    if (path.rfind(".js")   != std::string::npos) return "application/javascript";
    return "text/plain";
}

static void send_file(const std::string& fp, HttpResponse& res, Connection& conn) {
    std::string body = read_file(fp);
    if (body.empty()) { res.status(404).text(conn, "404 Not Found"); return; }
    conn.write("HTTP/1.1 200 OK\r\nContent-Type: " + mime_type(fp) +
               "\r\nContent-Length: " + std::to_string(body.size()) +
               "\r\nConnection: keep-alive\r\n\r\n" + body);
}

static std::string row_to_json(const Row& row) {
    std::string j = "{";
    bool first = true;
    for (auto& [k, v] : row) {
        if (!first) j += ",";
        j += "\"" + k + "\":\"" + v + "\"";
        first = false;
    }
    return j + "}";
}

static std::string result_to_json(const Result& rows) {
    std::string j = "[";
    for (size_t i = 0; i < rows.size(); i++) {
        if (i > 0) j += ",";
        j += row_to_json(rows[i]);
    }
    return j + "]";
}

int main() {
    int num_threads = std::thread::hardware_concurrency();
    if (num_threads < 1) num_threads = 2;
    std::cout << "[Main] 启动 " << num_threads << " 个调度器线程\n";

    Database db("127.0.0.1", "root", "", "coroutine_http");
    std::cout << "[DB] 数据库连接成功\n";

    // 全局聊天室（线程安全）
    ChatRoom room;

    MultiScheduler ms(num_threads);
    Scheduler& accept_sched = ms.get(0);
    HttpServer app(accept_sched);

    // ── 静态文件 ──────────────────────────────────
    app.get("/", [](HttpRequest& /*req*/, HttpResponse& res, Connection& conn) {
        send_file("static/index.html", res, conn);
    });
    app.get("/static/*", [](HttpRequest& req, HttpResponse& res, Connection& conn) {
        send_file("static/" + req.param("*"), res, conn);
    });

    // ── WebSocket 聊天室 ──────────────────────────
    app.get("/ws", [&room](HttpRequest& req, HttpResponse& /*res*/, Connection& conn) {
        if (!WebSocket::handshake(conn, req.headers)) {
            std::cerr << "[WS] 握手失败\n";
            return;
        }

        WebSocket ws(conn);
        std::string nick = req.query_param("nick", "匿名");
        std::cout << "[WS] " << nick << " 加入聊天室，在线: "
                  << room.size() + 1 << "\n";

        room.join(&ws);
        room.broadcast("{\"type\":\"system\",\"msg\":\"" + nick + " 加入了聊天室\","
                       "\"online\":" + std::to_string(room.size()) + "}");

        // 消息循环
        WsFrame frame;
        while (ws.recv_frame(frame)) {
            if (frame.opcode == WsOpcode::CLOSE) break;
            if (frame.opcode == WsOpcode::PING) {
                // 回复 PONG
                ws.send_text("");
                continue;
            }
            if (frame.opcode == WsOpcode::TEXT && !frame.payload.empty()) {
                // 广播消息
                std::string msg = "{\"type\":\"message\","
                                  "\"nick\":\"" + nick + "\","
                                  "\"msg\":\"" + frame.payload + "\"}";
                room.broadcast(msg);
            }
        }

        room.leave(&ws);
        room.broadcast("{\"type\":\"system\",\"msg\":\"" + nick + " 离开了聊天室\","
                       "\"online\":" + std::to_string(room.size()) + "}");
        std::cout << "[WS] " << nick << " 离开聊天室，在线: " << room.size() << "\n";
    });

    // ── REST API ──────────────────────────────────
    app.get("/api/ping", [](HttpRequest& /*req*/, HttpResponse& res, Connection& conn) {
        res.json(conn, "{\"status\":\"ok\",\"server\":\"coroutine-http-mt\"}");
    });
    app.get("/api/users", [&db](HttpRequest& /*req*/, HttpResponse& res, Connection& conn) {
        try {
            res.json(conn, result_to_json(
                db.query("SELECT id,name,email,role,created_at FROM users")));
        } catch (std::exception& e) {
            res.status(500).json(conn, "{\"error\":\"" + std::string(e.what()) + "\"}");
        }
    });
    app.get("/api/user/:id", [&db](HttpRequest& req, HttpResponse& res, Connection& conn) {
        try {
            std::string id = db.escape(req.param("id"));
            auto rows = db.query("SELECT id,name,email,role,created_at FROM users WHERE id=" + id);
            if (rows.empty()) res.status(404).json(conn, "{\"error\":\"用户不存在\"}");
            else              res.json(conn, row_to_json(rows[0]));
        } catch (std::exception& e) {
            res.status(500).json(conn, "{\"error\":\"" + std::string(e.what()) + "\"}");
        }
    });
    app.post("/api/users", [&db](HttpRequest& req, HttpResponse& res, Connection& conn) {
        auto extract = [&](const std::string& key) {
            std::string p = "\"" + key + "\":\"";
            size_t pos = req.body.find(p);
            if (pos == std::string::npos) return std::string("");
            pos += p.size();
            size_t end = req.body.find("\"", pos);
            return end == std::string::npos ? std::string("") : req.body.substr(pos, end - pos);
        };
        std::string name  = db.escape(extract("name"));
        std::string email = db.escape(extract("email"));
        std::string role  = db.escape(extract("role"));
        if (role.empty()) role = "user";
        if (name.empty() || email.empty()) {
            res.status(400).json(conn, "{\"error\":\"name 和 email 不能为空\"}"); return;
        }
        try {
            db.execute("INSERT INTO users (name,email,role) VALUES ('" +
                       name + "','" + email + "','" + role + "')");
            res.status(201).json(conn, "{\"id\":\"" +
                std::to_string(db.last_insert_id()) + "\",\"name\":\"" + name + "\"}");
        } catch (std::exception& e) {
            res.status(500).json(conn, "{\"error\":\"" + std::string(e.what()) + "\"}");
        }
    });
    app.del("/api/user/:id", [&db](HttpRequest& req, HttpResponse& res, Connection& conn) {
        try {
            std::string id = db.escape(req.param("id"));
            int n = db.execute("DELETE FROM users WHERE id=" + id);
            if (n == 0) res.status(404).json(conn, "{\"error\":\"用户不存在\"}");
            else        res.json(conn, "{\"status\":\"deleted\",\"id\":\"" + id + "\"}");
        } catch (std::exception& e) {
            res.status(500).json(conn, "{\"error\":\"" + std::string(e.what()) + "\"}");
        }
    });
    app.post("/api/echo", [](HttpRequest& req, HttpResponse& res, Connection& conn) {
        res.json(conn, "{\"echo\":" + req.body + "}");
    });

    app.listen(8888, [&ms](std::function<void()> f) { ms.post(std::move(f)); });
    ms.run();
    return 0;
}