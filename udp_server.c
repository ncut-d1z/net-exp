#include <memory.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <setjmp.h>
#include <signal.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>
#include "safeio.h"


#define BUFFER_SIZE 1024
#define SERVER_PORT 8080

int main() {
    int sockfd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len;
    char buffer[BUFFER_SIZE];
    ssize_t recv_len;
    int ret;
    char response[BUFFER_SIZE];

    /* 创建UDP套接字 */
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket creation failed");
        exit(EXIT_FAILURE);
    }

    /* 设置服务器地址结构 */
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);  /* 监听所有网络接口 */
    server_addr.sin_port = htons(SERVER_PORT);

    /* 绑定套接字到地址和端口 */
    ret = bind(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr));
    if (ret < 0) {
        perror("bind failed");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    printf("UDP server is running on port %d...\n", SERVER_PORT);

    /* 主循环：接收和处理数据包 */
    while (1) {
        client_len = sizeof(client_addr);
        memset(buffer, 0, BUFFER_SIZE);

        /* 接收UDP数据包 */
        recv_len = recvfrom(sockfd, buffer, BUFFER_SIZE - 1, 0,
                           (struct sockaddr*)&client_addr, &client_len);
        if (recv_len < 0) {
            perror("recvfrom failed");
            continue;  /* 继续处理下一个数据包 */
        }

        /* 确保字符串以null结尾 */
        buffer[recv_len] = '\0';

        /* 解析和处理数据包：打印客户端信息和内容 */
        printf("Received %lu bytes from %s:%d\n",
               (unsigned int) recv_len, inet_ntoa(client_addr.sin_addr),
               ntohs(client_addr.sin_port));
        printf("Content: %s\n", buffer);

        /* 构造响应消息 */
        snprintf(response, BUFFER_SIZE, "Server received your message: %s", buffer);

        /* 发送响应回客户端 */
        ret = sendto(sockfd, response, strlen(response), 0,
                    (struct sockaddr*)&client_addr, client_len);
        if (ret < 0) {
            perror("sendto failed");
        } else {
            printf("Response sent successfully\n");
        }
    }

    /* 关闭套接字（实际上不会执行到这里） */
    close(sockfd);
    return 0;
}
