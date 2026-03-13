#include "http/http_server.h"
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

int main() {
    int num_threads = std::thread::hardware_concurrency();
    if (num_threads < 1) num_threads = 2;
    std::cout << "[Main] 启动 " << num_threads << " 个调度器线程\n";

    ChatRoom room;

    MultiScheduler ms(num_threads);
    Scheduler& accept_sched = ms.get(0);
    HttpServer app(accept_sched);

    // ── 首页跳转 ──────────────────────────────────
    app.get("/", [](HttpRequest& /*req*/, HttpResponse& /*res*/, Connection& conn) {
        conn.write("HTTP/1.1 302 Found\r\n"
                   "Location: /static/chat.html\r\n"
                   "Content-Length: 0\r\n\r\n");
    });

    // ── 静态文件 ──────────────────────────────────
    app.get("/static/*", [](HttpRequest& req, HttpResponse& res, Connection& conn) {
        send_file("static/" + req.param("*"), res, conn);
    });

    // ── 心跳 ──────────────────────────────────────
    app.get("/api/ping", [](HttpRequest& /*req*/, HttpResponse& res, Connection& conn) {
        res.json(conn, "{\"status\":\"ok\",\"server\":\"coroutine-http-mt\"}");
    });

    // ── WebSocket 聊天室 ──────────────────────────
    app.get("/ws", [&room](HttpRequest& req, HttpResponse& /*res*/, Connection& conn) {
        if (!WebSocket::handshake(conn, req.headers)) {
            std::cerr << "[WS] 握手失败\n";
            return;
        }

        WebSocket ws(conn);
        std::string nick = req.query_param("nick", "匿名");

        room.join(&ws, nick);
        std::cout << "[WS] " << nick << " 加入聊天室，在线: " << room.size() << "\n";

        // 广播加入消息，附带最新用户列表
        room.broadcast("{\"type\":\"system\","
                       "\"msg\":\"" + nick + " 加入了聊天室\","
                       "\"online\":" + std::to_string(room.size()) + ","
                       "\"users\":" + room.nicks_json() + "}");

        WsFrame frame;
        while (ws.recv_frame(frame)) {
            if (frame.opcode == WsOpcode::CLOSE) break;
            if (frame.opcode == WsOpcode::PING) {
                ws.send_text("");
                continue;
            }
            if (frame.opcode == WsOpcode::TEXT && !frame.payload.empty()) {
                room.broadcast("{\"type\":\"message\","
                               "\"nick\":\"" + nick + "\","
                               "\"msg\":\"" + frame.payload + "\"}");
            }
        }

        room.leave(&ws);
        std::cout << "[WS] " << nick << " 离开聊天室，在线: " << room.size() << "\n";

        // 广播离开消息，附带最新用户列表
        room.broadcast("{\"type\":\"system\","
                       "\"msg\":\"" + nick + " 离开了聊天室\","
                       "\"online\":" + std::to_string(room.size()) + ","
                       "\"users\":" + room.nicks_json() + "}");
    });

    app.listen(8888, [&ms](std::function<void()> f) { ms.post(std::move(f)); });
    ms.run();
    return 0;
}