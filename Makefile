CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2 -I.
LDFLAGS  := -pthread $(shell mariadb_config --libs) -lssl -lcrypto
TARGET   := http_server

SRCS := core/coroutine.cpp \
        net/connection.cpp \
        net/tcp_server.cpp \
        http/http_request.cpp \
        http/http_response.cpp \
        http/http_parser.cpp \
        http/http_server.cpp \
        db/database.cpp \
        ws/websocket.cpp \
        main.cpp

OBJS := $(SRCS:.cpp=.o)

.PHONY: all clean debug bench

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -MMD -MP -c -o $@ $<

-include $(OBJS:.o=.d)

debug: CXXFLAGS += -O0 -g -fsanitize=address,undefined
debug: $(TARGET)

bench: all
	@bash test/bench.sh

clean:
	find . \( -name "*.o" -o -name "*.d" \) -delete
	rm -f $(TARGET)