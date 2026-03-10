#include "websocket.h"
#include <map>
#include <openssl/sha.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/buffer.h>
#include <stdexcept>
#include <iostream>
#include <cstring>

// WebSocket 握手用的魔术字符串（RFC 6455 规定）
static const std::string WS_MAGIC = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

WebSocket::WebSocket(Connection& conn) : conn_(conn) {}

// ── Base64 编码 ───────────────────────────────────────────────

static std::string base64_encode(const unsigned char* data, size_t len) {
    BIO* b64 = BIO_new(BIO_f_base64());
    BIO* mem = BIO_new(BIO_s_mem());
    b64 = BIO_push(b64, mem);
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    BIO_write(b64, data, (int)len);
    BIO_flush(b64);

    BUF_MEM* bptr;
    BIO_get_mem_ptr(b64, &bptr);
    std::string result(bptr->data, bptr->length);
    BIO_free_all(b64);
    return result;
}

// ── compute_accept ────────────────────────────────────────────

std::string WebSocket::compute_accept(const std::string& key) {
    std::string combined = key + WS_MAGIC;
    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char*>(combined.c_str()),
         combined.size(), hash);
    return base64_encode(hash, SHA_DIGEST_LENGTH);
}

// ── handshake ────────────────────────────────────────────────

bool WebSocket::handshake(Connection& conn,
                           const std::map<std::string,std::string>& headers) {
    // headers key 已统一转小写（由 HttpParser 处理）
    auto it = headers.find("sec-websocket-key");
    if (it == headers.end()) return false;
    std::string key = it->second;
    while (!key.empty() && key.front() == ' ') key.erase(key.begin());
    while (!key.empty() && (key.back() == ' ' || key.back() == '\r')) key.pop_back();
    if (key.empty()) return false;

    std::string accept = compute_accept(key);

    // 返回 101 Switching Protocols
    std::string response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + accept + "\r\n"
        "\r\n";

    conn.write(response);
    return true;
}

// ── recv_frame ────────────────────────────────────────────────

/**
 * WebSocket 帧格式（RFC 6455）：
 *
 *  0               1               2               3
 *  0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7
 * +-+-+-+-+-------+-+-------------+-------------------------------+
 * |F|R|R|R| opcode|M| Payload len |    Extended payload length    |
 * |I|S|S|S|  (4)  |A|     (7)    |             (16/64)           |
 * |N|V|V|V|       |S|             |   (if payload len==126/127)   |
 * | |1|2|3|       |K|             |                               |
 * +-+-+-+-+-------+-+-------------+ - - - - - - - - - - - - - - - +
 * |     Extended payload length continued, if payload len == 127  |
 * + - - - - - - - - - - - - - - -+-------------------------------+
 * |                               |Masking-key, if MASK set to 1  |
 * +-------------------------------+-------------------------------+
 * | Masking-key (continued)       |          Payload Data         |
 * +-------------------------------- - - - - - - - - - - - - - - - +
 * :                     Payload Data continued ...                :
 * + - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - +
 * |                     Payload Data continued ...                |
 * +---------------------------------------------------------------+
 */
bool WebSocket::recv_frame(WsFrame& frame) {
    if (conn_.is_closed()) return false;

    // 读前两个字节
    std::string header = conn_.read_exact(2);
    if (header.size() < 2) return false;

    uint8_t b0 = static_cast<uint8_t>(header[0]);
    uint8_t b1 = static_cast<uint8_t>(header[1]);

    frame.fin    = (b0 & 0x80) != 0;
    frame.opcode = static_cast<WsOpcode>(b0 & 0x0F);
    frame.masked = (b1 & 0x80) != 0;

    uint64_t payload_len = b1 & 0x7F;

    // 扩展长度
    if (payload_len == 126) {
        std::string ext = conn_.read_exact(2);
        if (ext.size() < 2) return false;
        payload_len = (static_cast<uint64_t>(static_cast<uint8_t>(ext[0])) << 8)
                    |  static_cast<uint64_t>(static_cast<uint8_t>(ext[1]));
    } else if (payload_len == 127) {
        std::string ext = conn_.read_exact(8);
        if (ext.size() < 8) return false;
        payload_len = 0;
        for (int i = 0; i < 8; i++)
            payload_len = (payload_len << 8) | static_cast<uint8_t>(ext[i]);
    }

    // 防止超大帧（限制 10MB）
    if (payload_len > 10 * 1024 * 1024) return false;

    // 读掩码
    uint8_t mask[4] = {0};
    if (frame.masked) {
        std::string m = conn_.read_exact(4);
        if (m.size() < 4) return false;
        for (int i = 0; i < 4; i++)
            mask[i] = static_cast<uint8_t>(m[i]);
    }

    // 读载荷
    std::string payload = conn_.read_exact(static_cast<size_t>(payload_len));
    if (payload.size() < payload_len) return false;

    // 解掩码
    if (frame.masked) {
        for (size_t i = 0; i < payload.size(); i++)
            payload[i] ^= mask[i % 4];
    }

    frame.payload = std::move(payload);
    return true;
}

// ── send_frame ────────────────────────────────────────────────

void WebSocket::send_frame(WsOpcode opcode, const std::string& payload) {
    std::string frame;
    frame.reserve(10 + payload.size());

    // 第一字节：FIN=1 + opcode
    frame += static_cast<char>(0x80 | static_cast<uint8_t>(opcode));

    // 第二字节起：payload 长度（服务端发送不需要 mask）
    size_t len = payload.size();
    if (len < 126) {
        frame += static_cast<char>(len);
    } else if (len < 65536) {
        frame += static_cast<char>(126);
        frame += static_cast<char>((len >> 8) & 0xFF);
        frame += static_cast<char>(len & 0xFF);
    } else {
        frame += static_cast<char>(127);
        for (int i = 7; i >= 0; i--)
            frame += static_cast<char>((len >> (8 * i)) & 0xFF);
    }

    frame += payload;
    conn_.write(frame);
}

void WebSocket::send_text(const std::string& msg) {
    send_frame(WsOpcode::TEXT, msg);
}

void WebSocket::send_close() {
    send_frame(WsOpcode::CLOSE, "");
    conn_.close();
}