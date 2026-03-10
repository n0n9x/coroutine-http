#ifndef HTTP_REQUEST_H
#define HTTP_REQUEST_H

/**
 * http_request.h — HTTP 请求数据对象
 *
 * 纯数据类，由 HttpParser 填充，由用户 Handler 读取。
 *
 * 覆盖的 HTTP/1.1 特性：
 *   - 请求行：method / path / version
 *   - Query string 解析：/search?q=hello&page=2
 *   - 路由参数：/user/:id 中的 id
 *   - Headers（大小写不敏感）
 *   - Body（原始字节串）
 *   - JSON body 的简单判断
 *   - Keep-Alive 判断
 */

#include <string>
#include <map>
#include <unordered_map>

class HttpRequest {
public:
    // ── 请求基本字段 ─────────────────────────────
    std::string method;   // "GET" "POST" "PUT" "DELETE" ...
    std::string path;     // "/api/user/42"（不含 query string）
    std::string version;  // "HTTP/1.1"
    std::string body;     // 请求体原始内容

    // ── Headers（key 统一转小写，方便查询）────────
    // 使用有序 map 保证遍历顺序稳定（调试友好）
    std::map<std::string, std::string> headers;

    // ── Query 参数（从 URL ? 后解析）─────────────
    // /search?q=hello&page=2  →  { "q": "hello", "page": "2" }
    std::unordered_map<std::string, std::string> query;

    // ── 路由参数（由 Router 填充）────────────────
    // 路由 "/user/:id" 匹配 "/user/42"  →  { "id": "42" }
    std::unordered_map<std::string, std::string> params;

    // ── 查询接口 ─────────────────────────────────

    /** 获取 header 值，不存在返回默认值 */
    std::string header(const std::string& key,
                       const std::string& default_val = "") const;

    /** 获取 query 参数，不存在返回默认值 */
    std::string query_param(const std::string& key,
                            const std::string& default_val = "") const;

    /** 获取路由参数，不存在返回默认值 */
    std::string param(const std::string& key,
                      const std::string& default_val = "") const;

    /** 是否为 Keep-Alive 连接 */
    bool keep_alive() const;

    /** Content-Type 是否包含 application/json */
    bool is_json() const;

    /** 返回 Content-Length，无此 header 返回 -1 */
    int content_length() const;

    /** 调试用：打印请求摘要 */
    void dump() const;
};

#endif // HTTP_REQUEST_H