#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "safeio.h"

#define BUFFER_SIZE 1024
#define SERVER_PORT 8080
#define SERVER_IP "127.0.0.1"  /* 本地回环地址 */

/* 数据包封装函数 */
void prepare_packet(const char* input, char* packet, int packet_size) {
    /* 简单的数据封装：添加时间戳前缀 */
    snprintf(packet, packet_size, "[Client] %s", input);
}

int main() {
    int sockfd;
    struct sockaddr_in server_addr;
    char buffer[BUFFER_SIZE];
    char packet[BUFFER_SIZE];
    ssize_t recv_len;
    int ret;

    /* 创建TCP套接字 */
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket creation failed");
        exit(EXIT_FAILURE);
    }

    /* 设置服务器地址结构 */
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    server_addr.sin_addr.s_addr = inet_addr(SERVER_IP);

    /* 连接到服务器 */
    ret = connect(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr));
    if (ret < 0) {
        perror("connect failed");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    printf("Connected to TCP server %s:%d\n", SERVER_IP, SERVER_PORT);
    printf("Enter messages to send (type 'exit' to quit):\n");

    /* 主循环：读取用户输入并发送 */
    while (1) {
        memset(buffer, 0, BUFFER_SIZE);
        memset(packet, 0, BUFFER_SIZE);

        /* 读取用户输入 */
        printf("Client> ");
        if (fgets(buffer, BUFFER_SIZE, stdin) == NULL) {
            perror("fgets failed");
            break;
        }

        /* 去除换行符 */
        buffer[strcspn(buffer, "\n")] = '\0';

        /* 检查退出条件 */
        if (strcmp(buffer, "exit") == 0) {
            printf("Exiting...\n");
            break;
        }

        /* 检查输入是否为空 */
        if (strlen(buffer) == 0) {
            continue;
        }

        /* 封装数据包 */
        prepare_packet(buffer, packet, BUFFER_SIZE);

        /* 发送TCP数据包到服务器 */
        ret = send(sockfd, packet, strlen(packet), 0);
        if (ret < 0) {
            perror("send failed");
            break;
        }

        printf("Message sent: %s\n", packet);

        /* 接收服务器响应 */
        memset(buffer, 0, BUFFER_SIZE);
        recv_len = recv(sockfd, buffer, BUFFER_SIZE - 1, 0);
        if (recv_len < 0) {
            perror("recv failed");
            break;
        } else if (recv_len == 0) {
            printf("Server closed the connection\n");
            break;
        }

        /* 确保字符串以null结尾 */
        buffer[recv_len] = '\0';
        printf("Server response: %s\n", buffer);
    }

    /* 关闭套接字 */
    close(sockfd);
    printf("Disconnected from server\n");
    return 0;
}
