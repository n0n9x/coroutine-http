#ifndef WEBSOCKET_H
#define WEBSOCKET_H

/**
 * websocket.h — WebSocket 协议实现
 *
 * 流程：
 *   1. 握手：解析 HTTP Upgrade 请求，返回 101
 *   2. 收帧：parse_frame() 读取并解析 WebSocket 数据帧
 *   3. 发帧：send_text() / send_close() 封装并发送帧
 */

#include "../net/connection.h"
#include <string>
#include <functional>
#include <map>

// WebSocket 帧类型
enum class WsOpcode : uint8_t {
    CONTINUATION = 0x0,
    TEXT         = 0x1,
    BINARY       = 0x2,
    CLOSE        = 0x8,
    PING         = 0x9,
    PONG         = 0xA,
};

// 解析后的 WebSocket 帧
struct WsFrame {
    bool      fin;       // 是否是最后一帧
    WsOpcode  opcode;
    bool      masked;    // 客户端发来的帧必须 masked
    std::string payload; // 解码后的载荷
};

class WebSocket {
public:
    explicit WebSocket(Connection& conn);

    /**
     * 握手：从 HTTP 请求头中提取 Sec-WebSocket-Key，
     * 计算 Accept，返回 101 Switching Protocols。
     * 返回 false 表示握手失败（不是合法的 WebSocket 请求）。
     */
    static bool handshake(Connection& conn,
                          const std::map<std::string,std::string>& headers);

    /**
     * 读取一个完整的 WebSocket 帧。
     * 返回 false 表示连接关闭或出错。
     */
    bool recv_frame(WsFrame& frame);

    /** 发送文本帧 */
    void send_text(const std::string& msg);

    /** 发送关闭帧并关闭连接 */
    void send_close();

    /** 连接是否已关闭 */
    bool is_closed() const { return conn_.is_closed(); }

private:
    Connection& conn_;

    // 计算 Sec-WebSocket-Accept
    static std::string compute_accept(const std::string& key);

    // 封装一个帧并发送
    void send_frame(WsOpcode opcode, const std::string& payload);
};

#endif // WEBSOCKET_H