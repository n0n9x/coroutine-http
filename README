# coroutine-http

> 从零实现的 C++ HTTP 服务器，底层基于 `ucontext + epoll` 的用户态协程调度器。  
> 以同步代码风格处理异步 I/O，无任何第三方依赖，Linux only。

---

## 在线 Demo

启动服务器后浏览器打开 `http://localhost:8888`，可直接体验前后端联动页面。

![demo](https://img.shields.io/badge/demo-localhost:8888-4f6ef7?style=flat-square)

---

## 性能数据

> 测试环境：本机，wrk -t4 -c100 -d10s

| 路由 | QPS | 平均延迟 | P97 延迟 |
|------|-----|---------|---------|
| GET /api/ping | **79,928** | 1.39ms | 28.75ms |
| GET /hello | **79,896** | 1.53ms | 71.43ms |
| GET /api/user/:id | **74,442** | 1.35ms | 19.17ms |

单机 QPS 稳定在 **7.4～8 万**，4 线程 100 并发连接下平均延迟 1.5ms 以内。

---

## 技术栈

- **语言**：C++17
- **平台**：Linux（依赖 `ucontext`、`epoll`）
- **依赖**：无第三方库
- **构建**：GNU Make + g++

---

## 架构

```
┌─────────────────────────────────────────────────┐
│               前端 (static/index.html)           │
│         fetch API → JSON  ←→  后端路由           │
├─────────────────────────────────────────────────┤
│                   HttpServer                     │
│       路由匹配 / 中间件 / Keep-Alive 循环         │
├──────────────────┬──────────────────────────────┤
│   HttpParser     │   HttpRequest / HttpResponse  │
├──────────────────┴──────────────────────────────┤
│          Connection        TcpServer             │
│    协程友好 read/write    非阻塞 accept 循环      │
├─────────────────────────────────────────────────┤
│                  Scheduler                       │
│   ready_queue │ waiting_map │ timer_heap         │
│          ucontext + epoll 事件驱动               │
└─────────────────────────────────────────────────┘
```

**调度流程：**
```
while (有就绪任务 || 有 I/O 等待 || 有定时器)
  1. 执行所有就绪协程（swapcontext）
  2. 触发到期定时器 → 放入就绪队列
  3. epoll_wait（超时 = 下一个定时器剩余毫秒）
  4. I/O 就绪 → 对应协程放入就绪队列
```

---

## 功能

### 协程调度器
- `ucontext` 上下文切换，每个协程独立 128KB 栈
- `epoll` 事件驱动，边缘触发（ET）
- 最小堆定时器，`sleep(ms)` 精确挂起
- `Channel<T>` 协程间同步通信（类 Go channel）
- 协程函数异常捕获，不影响调度器稳定性

### TCP 层
- 非阻塞 `accept`，ET 模式下一次性抽干积压连接
- `Connection` 封装读写，自动处理 `EAGAIN` 和部分写
- `read_until(delim)` / `read_exact(n)` 流式读取

### HTTP 层
- HTTP/1.1 完整解析（请求行、headers、body）
- URL decode，query string 解析
- 路径参数路由（`:id`）和通配路由（`*`）
- Keep-Alive 连接复用
- 中间件支持
- 链式响应 API：`res.status(200).json(conn, body)`
- 静态文件服务，自动识别 MIME 类型

### 前端 Demo
- 纯 HTML/CSS/JS，无框架依赖
- 通过 `fetch` 调用后端 REST API
- 服务器心跳检测 + 延迟显示
- 用户查询 / 用户列表 / Echo 接口演示

---

## 文件结构

```
.
├── core/
│   ├── coroutine.h       # Coroutine / Scheduler / Channel<T>
│   └── coroutine.cpp
├── net/
│   ├── connection.h/cpp  # TCP 连接封装
│   └── tcp_server.h/cpp  # 监听 + accept
├── http/
│   ├── http_request.h/cpp
│   ├── http_response.h/cpp
│   ├── http_parser.h/cpp
│   └── http_server.h/cpp  # 路由器 + 服务器
├── static/
│   └── index.html         # 前端 Demo 页面
├── test/
│   └── bench.sh           # wrk 压测脚本
├── main.cpp
├── Makefile
└── README.md
```

---

## 编译与运行

```bash
# 编译
make

# 运行
./http_server
# 输出：[HttpServer] 启动完成，监听端口 8888

# 浏览器访问
open http://localhost:8888
```

**Debug 构建（开启 AddressSanitizer）：**
```bash
make debug
```

**压测：**
```bash
sudo apt install wrk
wrk -t4 -c100 -d10s http://localhost:8888/api/ping
```

---

## API 路由

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/` | 前端 Demo 页面 |
| GET | `/static/*` | 静态文件服务 |
| GET | `/api/ping` | 服务器心跳 |
| GET | `/api/users` | 用户列表 |
| GET | `/api/user/:id` | 查询单个用户 |
| POST | `/api/echo` | 请求体回显 |

---

## 关键设计

**1. 64 位指针拆分传递**  
`makecontext` 参数只能是 32 位整数，在 64 位系统上传指针需拆成高低两个 `uint32_t`，在 `co_entry` 里再拼回来。

**2. epoll ADD/MOD 容错**  
同一个 fd 重复注册 epoll 会返回 `EEXIST`，`wait_event` 里检测到后自动降级为 `MOD` 而非直接报错，保证 listen socket 的 accept 循环稳定运行。

**3. TcpServer 生命周期管理**  
`HttpServer::listen()` 将 `TcpServer` 保存为成员变量（`shared_ptr`），而非函数内局部变量，避免 `listen()` 返回后 `TcpServer` 析构、`listen_fd` 被 close，导致 accept 协程拿到 fd=-1。

**4. HTTP Keep-Alive 循环**  
同一连接持续处理多个请求，HTTP/1.1 默认开启，直到客户端发送 `Connection: close` 或解析失败才关闭连接，减少 TCP 握手开销。

**5. 定时器最小堆**  
`epoll_wait` 的超时参数动态设为下一个定时器的剩余毫秒数，到期后精确唤醒协程，不做无谓轮询。

---

## 已知限制

- 单线程，不支持多核并行（可扩展为每核一个 Scheduler）
- 无抢占，计算密集型任务会阻塞整个调度器
- 协程栈固定 128KB，深递归需调整 `CO_STACK_SIZE`