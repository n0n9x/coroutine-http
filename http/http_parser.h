#ifndef HTTP_PARSER_H
#define HTTP_PARSER_H

/**
 * http_parser.h — HTTP/1.1 请求解析器
 *
 * 从 Connection 中读取原始字节流，解析出完整的 HttpRequest。
 *
 * 解析流程：
 *   1. 读到 "\r\n\r\n"（Header 结束标志）
 *   2. 解析请求行（method / path+query / version）
 *   3. 逐行解析 Headers
 *   4. 根据 Content-Length 读取 Body
 *   5. URL decode + query string 解析
 *
 * 错误处理：
 *   parse() 返回 false 表示解析失败（格式错误或连接断开），
 *   调用方应直接关闭连接。
 */

#include "http_request.h"
#include "../net/connection.h"
#include <string>

class HttpParser {
public:
    /**
     * 从连接中读取并解析一个完整的 HTTP 请求。
     *
     * @param conn  客户端连接（读操作会挂起协程等待数据）
     * @param req   输出参数，解析结果写入此对象
     * @return      true=解析成功，false=连接断开或格式错误
     */
    static bool parse(Connection& conn, HttpRequest& req);

private:
    // ── 各阶段解析 ───────────────────────────────

    /** 解析请求行："GET /path?q=1 HTTP/1.1" */
    static bool parse_request_line(const std::string& line, HttpRequest& req);

    /** 解析单行 Header："Content-Type: application/json" */
    static bool parse_header_line(const std::string& line, HttpRequest& req);

    /** 解析 URL query string："q=hello&page=2" */
    static void parse_query_string(const std::string& query_str, HttpRequest& req);

    // ── 工具函数 ─────────────────────────────────

    /** URL 解码：%20 → 空格，+ → 空格 */
    static std::string url_decode(const std::string& s);

    /** 去除字符串首尾空白（包括 \r \n \t） */
    static std::string trim(const std::string& s);

    /** 将字符串转为小写 */
    static std::string to_lower(const std::string& s);
};

#endif // HTTP_PARSER_H