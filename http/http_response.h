#ifndef HTTP_RESPONSE_H
#define HTTP_RESPONSE_H

/**
 * http_response.h — HTTP 响应构造器
 *
 * 链式调用风格，最终通过 send(conn) 将数据写入连接。
 *
 * 典型用法：
 *   res.status(200).header("X-Custom", "val").json("{\"ok\":true}");
 *   res.status(404).text("Not Found");
 *   res.status(302).header("Location", "/login").send(conn);
 */

#include "../net/connection.h"
#include <string>
#include <map>

class HttpResponse {
public:
    HttpResponse();

    // ── 链式设置接口 ─────────────────────────────

    /** 设置状态码（默认 200） */
    HttpResponse& status(int code);

    /** 添加/覆盖一个响应 Header */
    HttpResponse& header(const std::string& key, const std::string& value);

    // ── 终结方法（设置 body 并发送）─────────────

    /**
     * 发送纯文本响应。
     * 自动设置 Content-Type: text/plain; charset=utf-8
     */
    void text(Connection& conn, const std::string& body);

    /**
     * 发送 JSON 响应（body 已是 JSON 字符串）。
     * 自动设置 Content-Type: application/json
     *
     * 注：不做序列化，body 需调用方自行构造 JSON 字符串。
     *     第二阶段可引入 nlohmann/json 做自动序列化。
     */
    void json(Connection& conn, const std::string& body);

    /**
     * 发送 HTML 响应。
     * 自动设置 Content-Type: text/html; charset=utf-8
     */
    void html(Connection& conn, const std::string& body);

    /**
     * 发送空响应（仅状态行 + Headers，无 Body）。
     * 常用于 204 No Content 或 重定向。
     */
    void send(Connection& conn);

    // ── 状态码转文本 ─────────────────────────────
    static std::string status_text(int code);

private:
    int                                status_code_;
    std::map<std::string, std::string> headers_;  // 有序，输出稳定

    /** 设置 body 内容，自动补全 Content-Length */
    void set_body(const std::string& content_type, const std::string& body);

    /** 将完整响应序列化为字符串并写入连接 */
    void flush(Connection& conn);
};

#endif // HTTP_RESPONSE_H