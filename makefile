# 编译器设置
CC = gcc
CFLAGS = -std=c89 -Wall -Wextra -pedantic
LDFLAGS = -lpthread

# 定义目标文件
TARGETS = tcp_server tcp_client udp_server udp_client print_mac raw_icmp

# 获取所有.c文件
SRCS = $(wildcard *.c)
OBJS = $(SRCS:.c=.o)

# 默认目标：编译所有程序
all: $(TARGETS)
	@echo "=== 所有目标已编译完成 ==="

# TCP服务器编译规则
tcp_server: tcp_server.o
	$(CC) -o $@ $^ $(LDFLAGS)
	@echo "TCP服务器编译完成: $@"

# TCP客户端编译规则
tcp_client: tcp_client.o
	$(CC) -o $@ $^ $(LDFLAGS)
	@echo "TCP客户端编译完成: $@"

# UDP服务器编译规则
udp_server: udp_server.o
	$(CC) -o $@ $^ $(LDFLAGS)
	@echo "UDP服务器编译完成: $@"

# UDP客户端编译规则
udp_client: udp_client.o
	$(CC) -o $@ $^ $(LDFLAGS)
	@echo "UDP客户端编译完成: $@"

# MAC地址打印程序编译规则
print_mac: print_mac.o
	$(CC) -o $@ $^ $(LDFLAGS)
	@echo "MAC地址打印程序编译完成: $@"

# ICMP程序编译规则
raw_icmp: raw_icmp.o
	$(CC) -o $@ $^ $(LDFLAGS)
	@echo "ICMP程序编译完成: $@"

# 通用规则：从.c文件生成.o文件
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@
	@echo "编译对象文件: $@"

# 伪目标声明
.PHONY: all clean install debug release help

# 清理编译产物
clean:
	rm -f $(TARGETS) *.o
	@echo "=== 已清理所有编译产物 ==="

# 安装到系统目录（需要sudo权限）
install: all
	@if [ -d "/usr/local/bin" ]; then \
		sudo cp $(TARGETS) /usr/local/bin/; \
		echo "=== 程序已安装到 /usr/local/bin ==="; \
	else \
		echo "错误: /usr/local/bin 目录不存在"; \
	fi

# 调试版本编译
debug: CFLAGS += -g -DDEBUG -O0
debug: clean all
	@echo "=== 调试版本编译完成 ==="

# 发布版本编译
release: CFLAGS += -O2 -DNDEBUG
release: clean all
	@echo "=== 发布版本编译完成 ==="

# 显示帮助信息
help:
	@echo "=== Makefile 使用说明 ==="
	@echo "可用目标:"
	@echo "  all      : 编译所有程序（默认目标）"
	@echo "  tcp_server: 仅编译TCP服务器"
	@echo "  tcp_client: 仅编译TCP客户端"
	@echo "  udp_server: 仅编译UDP服务器"
	@echo "  udp_client: 仅编译UDP客户端"
	@echo "  print_mac : 仅编译MAC地址打印程序"
	@echo "  clean     : 清理所有编译产物"
	@echo "  install   : 安装到系统目录"
	@echo "  debug     : 编译调试版本"
	@echo "  release   : 编译发布版本"
	@echo "  help      : 显示此帮助信息"
	@echo ""
	@echo "示例:"
	@echo "  make                       # 编译所有程序"
	@echo "  make tcp_server            # 仅编译TCP服务器"
	@echo "  make debug                  # 编译调试版本"
	@echo "  make clean && make release # 清理后编译发布版本"

# 依赖关系声明
tcp_server.o: tcp_server.c
tcp_client.o: tcp_client.c
udp_server.o: udp_server.c
udp_client.o: udp_client.c
print_mac.o: print_mac.c
raw_icmp.o: raw_icmp.c
