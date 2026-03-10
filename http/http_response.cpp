#include "http_request.h"
#include <iostream>
#include <algorithm>
#include <stdexcept>

// ── 查询接口 ─────────────────────────────────────────────────

std::string HttpRequest::header(const std::string& key,
                                const std::string& default_val) const {
    // headers 的 key 已在解析时统一转为小写
    std::string lower_key = key;
    std::transform(lower_key.begin(), lower_key.end(),
                   lower_key.begin(), ::tolower);
    auto it = headers.find(lower_key);
    return it != headers.end() ? it->second : default_val;
}

std::string HttpRequest::query_param(const std::string& key,
                                     const std::string& default_val) const {
    auto it = query.find(key);
    return it != query.end() ? it->second : default_val;
}

std::string HttpRequest::param(const std::string& key,
                               const std::string& default_val) const {
    auto it = params.find(key);
    return it != params.end() ? it->second : default_val;
}

// ── 语义判断 ─────────────────────────────────────────────────

bool HttpRequest::keep_alive() const {
    // HTTP/1.1 默认 Keep-Alive，除非显式 Connection: close
    // HTTP/1.0 默认 close，除非显式 Connection: keep-alive
    std::string conn_hdr = header("connection");
    std::transform(conn_hdr.begin(), conn_hdr.end(), conn_hdr.begin(), ::tolower);

    if (version == "HTTP/1.1") {
        return conn_hdr != "close";
    } else {
        return conn_hdr == "keep-alive";
    }
}

bool HttpRequest::is_json() const {
    std::string ct = header("content-type");
    return ct.find("application/json") != std::string::npos;
}

int HttpRequest::content_length() const {
    std::string cl = header("content-length");
    if (cl.empty()) return -1;
    try {
        return std::stoi(cl);
    } catch (...) {
        return -1;
    }
}

// ── 调试 ─────────────────────────────────────────────────────

void HttpRequest::dump() const {
    std::cout << "── HttpRequest ──────────────────\n"
              << "  " << method << " " << path << " " << version << "\n";
    for (auto& [k, v] : headers)
        std::cout << "  " << k << ": " << v << "\n";
    if (!query.empty()) {
        std::cout << "  Query:\n";
        for (auto& [k, v] : query)
            std::cout << "    " << k << "=" << v << "\n";
    }
    if (!body.empty())
        std::cout << "  Body(" << body.size() << "): "
                  << body.substr(0, 80)
                  << (body.size() > 80 ? "..." : "") << "\n";
    std::cout << "─────────────────────────────────\n";
}