#include "http_response.h"
#include <sstream>

// ── 构造 ─────────────────────────────────────────────────────

HttpResponse::HttpResponse() : status_code_(200) {}

// ── 状态码文本映射 ────────────────────────────────────────────

std::string HttpResponse::status_text(int code) {
    switch (code) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 500: return "Internal Server Error";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        default:  return "Unknown";
    }
}

// ── 链式设置接口 ──────────────────────────────────────────────

HttpResponse& HttpResponse::status(int code) {
    status_code_ = code;
    return *this;
}

HttpResponse& HttpResponse::header(const std::string& key,
                                   const std::string& value) {
    headers_[key] = value;
    return *this;
}

// ── 内部：设置 body ───────────────────────────────────────────

void HttpResponse::set_body(const std::string& content_type,
                             const std::string& body) {
    headers_["Content-Type"]   = content_type;
    headers_["Content-Length"] = std::to_string(body.size());
    headers_["\x01body"]       = body;
}

// ── 内部：序列化并写入连接 ────────────────────────────────────

void HttpResponse::flush(Connection& conn) {
    std::string body;
    auto it = headers_.find("\x01body");
    if (it != headers_.end()) {
        body = it->second;
        headers_.erase(it);
    }

    //填充默认请求头
    if (headers_.find("Server") == headers_.end())
        headers_["Server"] = "coroutine-http/1.0";
    if (headers_.find("Connection") == headers_.end())
        headers_["Connection"] = "keep-alive";

    //拼接状态行
    std::ostringstream oss;
    oss << "HTTP/1.1 " << status_code_ << " "
        << status_text(status_code_) << "\r\n";
    for (auto& [k, v] : headers_)
        oss << k << ": " << v << "\r\n";
    oss << "\r\n" << body;

    conn.write(oss.str());
}

// ── 终结方法 ──────────────────────────────────────────────────

void HttpResponse::text(Connection& conn, const std::string& body) {
    set_body("text/plain; charset=utf-8", body);
    flush(conn);
}

void HttpResponse::json(Connection& conn, const std::string& body) {
    set_body("application/json", body);
    flush(conn);
}

void HttpResponse::html(Connection& conn, const std::string& body) {
    set_body("text/html; charset=utf-8", body);
    flush(conn);
}

void HttpResponse::send(Connection& conn) {
    flush(conn);
}