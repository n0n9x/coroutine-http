#ifndef CHAT_ROOM_H
#define CHAT_ROOM_H

/**
 * chat_room.h — WebSocket 聊天室
 *
 * 维护所有在线连接和昵称，收到消息后广播给所有人。
 * 线程安全：多个 Scheduler 线程都可能同时调用 broadcast。
 */

#include "websocket.h"
#include <vector>
#include <mutex>
#include <string>
#include <algorithm>

class ChatRoom {
public:
    /** 注册一个新连接，附带昵称 */
    void join(WebSocket* ws, const std::string& nick) {
        std::lock_guard<std::mutex> lk(mutex_);
        clients_.push_back({ws, nick});
    }

    /** 移除一个连接 */
    void leave(WebSocket* ws) {
        std::lock_guard<std::mutex> lk(mutex_);
        clients_.erase(
            std::remove_if(clients_.begin(), clients_.end(),
                           [ws](const Client& c){ return c.ws == ws; }),
            clients_.end());
    }

    /** 广播消息给所有在线客户端 */
    void broadcast(const std::string& msg) {
        std::lock_guard<std::mutex> lk(mutex_);
        for (auto& c : clients_) {
            if (!c.ws->is_closed()) {
                try { c.ws->send_text(msg); }
                catch (...) {}
            }
        }
    }

    /** 当前在线人数 */
    size_t size() {
        std::lock_guard<std::mutex> lk(mutex_);
        return clients_.size();
    }

    /** 返回在线用户昵称列表的 JSON 数组字符串，如 ["Alice","Bob"] */
    std::string nicks_json() {
        std::lock_guard<std::mutex> lk(mutex_);
        std::string j = "[";
        for (size_t i = 0; i < clients_.size(); i++) {
            if (i > 0) j += ",";
            j += "\"" + clients_[i].nick + "\"";
        }
        return j + "]";
    }

private:
    struct Client {
        WebSocket*  ws;
        std::string nick;
    };

    std::vector<Client> clients_;
    std::mutex          mutex_;
};

#endif // CHAT_ROOM_H