#ifndef CHAT_ROOM_H
#define CHAT_ROOM_H

/**
 * chat_room.h — WebSocket 聊天室
 *
 * 维护所有在线连接，收到消息后广播给所有人。
 * 线程安全：多个 Scheduler 线程都可能同时调用 broadcast。
 */

#include "websocket.h"
#include <vector>
#include <mutex>
#include <string>
#include <algorithm>

class ChatRoom {
public:
    /** 注册一个新连接 */
    void join(WebSocket* ws) {
        std::lock_guard<std::mutex> lk(mutex_);
        clients_.push_back(ws);
    }

    /** 移除一个连接 */
    void leave(WebSocket* ws) {
        std::lock_guard<std::mutex> lk(mutex_);
        clients_.erase(
            std::remove(clients_.begin(), clients_.end(), ws),
            clients_.end());
    }

    /** 广播消息给所有在线客户端 */
    void broadcast(const std::string& msg) {
        std::lock_guard<std::mutex> lk(mutex_);
        for (auto* ws : clients_) {
            if (!ws->is_closed()) {
                try { ws->send_text(msg); }
                catch (...) {}
            }
        }
    }

    /** 当前在线人数 */
    size_t size() {
        std::lock_guard<std::mutex> lk(mutex_);
        return clients_.size();
    }

private:
    std::vector<WebSocket*> clients_;
    std::mutex              mutex_;
};

#endif // CHAT_ROOM_H