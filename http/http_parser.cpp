#include "http_parser.h"
#include <sstream>
#include <algorithm>
#include <cctype>

// ── 工具函数 ─────────────────────────────────────────────────

std::string HttpParser::trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::string HttpParser::to_lower(const std::string& s) {
    std::string result = s;
    std::transform(result.begin(), result.end(),
                   result.begin(), ::tolower);
    return result;
}

/**
 * URL 解码
 *
 * 规则：
 *   %XX → 对应 ASCII 字符（XX 为十六进制）
 *   +   → 空格（application/x-www-form-urlencoded 规范）
 *   其余 → 原样保留
 */
std::string HttpParser::url_decode(const std::string& s) {
    std::string result;
    result.reserve(s.size());

    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size()
            && isxdigit(s[i+1]) && isxdigit(s[i+2]))
        {
            // 将两位十六进制转换为字符
            char hex[3] = { s[i+1], s[i+2], '\0' };
            result += static_cast<char>(strtol(hex, nullptr, 16));
            i += 2;
        } else if (s[i] == '+') {
            result += ' ';
        } else {
            result += s[i];
        }
    }
    return result;
}

// ── parse_query_string ───────────────────────────────────────

/**
 * 解析 query string："q=hello+world&page=2&flag"
 *
 * 格式：key=value 对，以 & 分隔；没有 = 的 key，value 为空字符串。
 */
void HttpParser::parse_query_string(const std::string& query_str,
                                    HttpRequest& req) {
    if (query_str.empty()) return;

    std::istringstream ss(query_str);
    std::string pair;

    while (std::getline(ss, pair, '&')) {
        if (pair.empty()) continue;
        size_t eq = pair.find('=');
        if (eq == std::string::npos) {//没有等号
            req.query[url_decode(pair)] = "";
        } else {
            std::string key = url_decode(pair.substr(0, eq));
            std::string val = url_decode(pair.substr(eq + 1));
            req.query[key]  = val;
        }
    }
}

// ── parse_request_line ───────────────────────────────────────

/**
 * 解析请求行，例如：
 *   "GET /search?q=hello HTTP/1.1"
 *
 * 将 path 和 query string 分开：
 *   req.path  = "/search"
 *   req.query = { "q": "hello" }
 */
bool HttpParser::parse_request_line(const std::string& line,
                                    HttpRequest& req) {
    std::istringstream ss(line);
    std::string url;

    if (!(ss >> req.method >> url >> req.version))
        return false;

    // 转大写 method（规范要求区分大小写，但容错处理）
    std::transform(req.method.begin(), req.method.end(),
                   req.method.begin(), ::toupper);

    // 分离 path 和 query string
    size_t q = url.find('?');
    if (q == std::string::npos) {//url不含问号
        req.path = url_decode(url);
    } else {
        req.path = url_decode(url.substr(0, q));
        parse_query_string(url.substr(q + 1), req);
    }

    // 路径安全检查：防止路径穿越攻击（../../etc/passwd）
    if (req.path.find("..") != std::string::npos)
        return false;

    return true;
}

// ── parse_header_line ────────────────────────────────────────

/**
 * 解析单行 Header，例如：
 *   "Content-Type: application/json"
 *   "X-Custom-Header:   value with spaces  "
 *
 * key 统一转为小写，value 去除首尾空白。
 */
bool HttpParser::parse_header_line(const std::string& line,
                                   HttpRequest& req) {
    size_t colon = line.find(':');
    if (colon == std::string::npos) return false;

    std::string key = to_lower(trim(line.substr(0, colon)));
    std::string val = trim(line.substr(colon + 1));

    if (key.empty()) return false;

    req.headers[key] = val;
    return true;
}

// ── parse（主入口）──────────────────────────────────────────

/**
 * 完整解析流程：
 *
 *  Step 1. 从连接读取数据直到 "\r\n\r\n"（Header 结束）
 *  Step 2. 按 "\r\n" 切割各行
 *  Step 3. 第一行 → parse_request_line
 *  Step 4. 其余行 → parse_header_line
 *  Step 5. 若有 Content-Length，继续读取 Body
 */
bool HttpParser::parse(Connection& conn, HttpRequest& req) {
    // ── Step 1：读到 Header 结束标志 ─────────────
    std::string header_block = conn.read_until("\r\n\r\n");
    if (header_block.empty()) return false;  // 连接断开

    // 移除末尾的 "\r\n\r\n"
    if (header_block.size() >= 4)
        header_block.erase(header_block.size() - 4);

    // ── Step 2：按行切割 ──────────────────────────
    std::vector<std::string> lines;
    std::istringstream ss(header_block);
    std::string line;
    while (std::getline(ss, line)) {
        // getline 只去掉 \n，需手动去掉 \r
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        lines.push_back(line);
    }

    if (lines.empty()) return false;

    // ── Step 3：解析请求行 ────────────────────────
    if (!parse_request_line(lines[0], req)) return false;

    // ── Step 4：解析 Headers ──────────────────────
    for (size_t i = 1; i < lines.size(); i++) {
        if (lines[i].empty()) break;  // 空行 = Header 结束
        parse_header_line(lines[i], req);
    }

    // ── Step 5：读取 Body ─────────────────────────
    int cl = req.content_length();
    if (cl > 0) {
        // 防止超大 body（简单限制：最大 10MB）
        if (cl > 10 * 1024 * 1024) return false;
        req.body = conn.read_exact(static_cast<size_t>(cl));
    }

    return true;
}