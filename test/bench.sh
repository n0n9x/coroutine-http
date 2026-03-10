#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────
# bench.sh — 对 http_server 做 wrk 压测，对比 Nginx
#
# 依赖：wrk（apt install wrk）
# 用法：
#   make bench          # 从 Makefile 调用
#   bash test/bench.sh  # 直接运行
# ─────────────────────────────────────────────────────────────

set -euo pipefail

# ── 配置 ──────────────────────────────────────────────────────
SERVER_BIN="./http_server"
SERVER_PORT=8888
SERVER_URL="http://127.0.0.1:${SERVER_PORT}/ping"

WRK_THREADS=4       # wrk 线程数（建议 = CPU 核心数）
WRK_CONNECTIONS=100 # 并发连接数
WRK_DURATION=10     # 每轮压测秒数

# ── 检查依赖 ──────────────────────────────────────────────────
if ! command -v wrk &>/dev/null; then
    echo "[bench] 未找到 wrk，请先安装：sudo apt install wrk"
    exit 1
fi

# ── 启动服务器 ────────────────────────────────────────────────
echo "[bench] 启动 ${SERVER_BIN} ..."
${SERVER_BIN} &
SERVER_PID=$!

# 等待监听就绪（最多 3 秒）
for i in $(seq 1 30); do
    if curl -sf "${SERVER_URL}" &>/dev/null; then break; fi
    sleep 0.1
done

if ! kill -0 "${SERVER_PID}" 2>/dev/null; then
    echo "[bench] 服务器启动失败"
    exit 1
fi

echo "[bench] 服务器 PID=${SERVER_PID} 已就绪"
echo ""

# ── 压测函数 ──────────────────────────────────────────────────
run_wrk() {
    local label="$1"
    local url="$2"
    echo "════════════════════════════════════════"
    echo "  ${label}"
    echo "  URL         : ${url}"
    echo "  线程 / 连接 : ${WRK_THREADS}T / ${WRK_CONNECTIONS}C"
    echo "  持续时间    : ${WRK_DURATION}s"
    echo "════════════════════════════════════════"
    wrk -t"${WRK_THREADS}" -c"${WRK_CONNECTIONS}" -d"${WRK_DURATION}s" \
        --latency "${url}"
    echo ""
}

# ── 各路由压测 ────────────────────────────────────────────────
run_wrk "GET /ping  (JSON 响应)" \
    "http://127.0.0.1:${SERVER_PORT}/ping"

run_wrk "GET /hello (文本响应)" \
    "http://127.0.0.1:${SERVER_PORT}/hello"

run_wrk "GET /user/:id (路径参数)" \
    "http://127.0.0.1:${SERVER_PORT}/user/42"

# ── POST 压测（需要 wrk Lua 脚本）────────────────────────────
POST_SCRIPT=$(mktemp /tmp/bench_post_XXXX.lua)
cat > "${POST_SCRIPT}" << 'LUA'
wrk.method = "POST"
wrk.body   = '{"msg":"hello"}'
wrk.headers["Content-Type"] = "application/json"
LUA

echo "════════════════════════════════════════"
echo "  POST /echo (请求体回显)"
echo "════════════════════════════════════════"
wrk -t"${WRK_THREADS}" -c"${WRK_CONNECTIONS}" -d"${WRK_DURATION}s" \
    --latency -s "${POST_SCRIPT}" \
    "http://127.0.0.1:${SERVER_PORT}/echo"
rm -f "${POST_SCRIPT}"
echo ""

# ── 停止服务器 ────────────────────────────────────────────────
kill "${SERVER_PID}" 2>/dev/null
wait "${SERVER_PID}" 2>/dev/null || true

echo "[bench] 完成。将上述 Requests/sec 填入 README.md 的压测表格。"