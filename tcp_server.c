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
#define MAX_PENDING 5

/* 数据包处理函数 */
void process_packet(const char* data, ssize_t data_len, char* response) {
    int prefix_len;
    int i;

    /* 简单的数据处理：将接收到的数据转换为大写并添加前缀 */
    snprintf(response, BUFFER_SIZE, "Processed[%lu bytes]: ", (unsigned long int) data_len);
    prefix_len = strlen(response);
    /* 处理数据内容（简单示例） */
    for (i = 0; i < data_len && i < BUFFER_SIZE - prefix_len - 1; i++) {
        if (data[i] >= 'a' && data[i] <= 'z') {
            response[prefix_len + i] = data[i] - 32; /* 小写转大写 */
        } else {
            response[prefix_len + i] = data[i];
        }
    }
    response[prefix_len + i] = '\0';
}

int main() {
    int server_fd, client_fd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len;
    char buffer[BUFFER_SIZE];
    char response[BUFFER_SIZE];
    ssize_t recv_len;
    int ret;
    int opt;

    /* 创建TCP套接字 */
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket creation failed");
        exit(EXIT_FAILURE);
    }

    /* 设置套接字选项，允许地址重用 */
    opt = 1;
    ret = setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    if (ret < 0) {
        perror("setsockopt failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    /* 设置服务器地址结构 */
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);  /* 监听所有网络接口 */
    server_addr.sin_port = htons(SERVER_PORT);

    /* 绑定套接字到地址和端口 */
    ret = bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr));
    if (ret < 0) {
        perror("bind failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    /* 开始监听连接请求 */
    ret = listen(server_fd, MAX_PENDING);
    if (ret < 0) {
        perror("listen failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    printf("TCP server is running on port %d...\n", SERVER_PORT);
    printf("Waiting for incoming connections...\n");

    /* 主循环：接受和处理客户端连接 */
    while (1) {
        client_len = sizeof(client_addr);
        memset(&client_addr, 0, client_len);

        /* 接受客户端连接 */
        client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            perror("accept failed");
            continue;  /* 继续等待下一个连接 */
        }

        /* 打印客户端连接信息 */
        printf("New connection from %s:%d\n",
               inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

        /* 处理客户端数据 */
        while (1) {
            memset(buffer, 0, BUFFER_SIZE);

            /* 接收TCP数据包 */
            recv_len = recv(client_fd, buffer, BUFFER_SIZE - 1, 0);
            if (recv_len < 0) {
                perror("recv failed");
                break;
            } else if (recv_len == 0) {
                printf("Client disconnected\n");
                break;
            }

            /* 确保字符串以null结尾 */
            buffer[recv_len] = '\0';

            /* 解析和处理数据包 */
            printf("Received %lu bytes from %s:%d\n",
                   (unsigned long int) recv_len, inet_ntoa(client_addr.sin_addr),
                   ntohs(client_addr.sin_port));
            printf("Raw data: %s\n", buffer);

            /* 处理数据包内容 */
            memset(response, 0, BUFFER_SIZE);
            process_packet(buffer, recv_len, response);

            /* 发送处理后的响应回客户端 */
            ret = send(client_fd, response, strlen(response), 0);
            if (ret < 0) {
                perror("send failed");
                break;
            } else {
                printf("Response sent: %s\n", response);
            }
        }

        /* 关闭客户端连接 */
        close(client_fd);
        printf("Connection closed\n");
    }

    /* 关闭服务器套接字（实际上不会执行到这里） */
    close(server_fd);
    return 0;
}
