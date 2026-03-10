CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2 -I.
TARGET   := http_server

# ── 源文件 ────────────────────────────────────────────────────
SRCS := core/coroutine.cpp \
        net/connection.cpp \
        net/tcp_server.cpp \
        http/http_request.cpp \
        http/http_response.cpp \
        http/http_parser.cpp \
        http/http_server.cpp \
        main.cpp

OBJS := $(SRCS:.cpp=.o)

# ── 主目标 ────────────────────────────────────────────────────
.PHONY: all clean debug bench

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^

# 通用编译规则（-MMD -MP 自动生成头文件依赖）
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -MMD -MP -c -o $@ $<

# 引入自动生成的依赖文件
-include $(OBJS:.o=.d)

# ── Debug 构建（关闭优化，开启 Address/UB Sanitizer）────────
debug: CXXFLAGS += -O0 -g -fsanitize=address,undefined
debug: $(TARGET)

# ── 压测（需先 make，再 make bench）─────────────────────────
bench: all
	@bash test/bench.sh

# ── 清理 ─────────────────────────────────────────────────────
clean:
	find . \( -name "*.o" -o -name "*.d" \) -delete
	rm -f $(TARGET)