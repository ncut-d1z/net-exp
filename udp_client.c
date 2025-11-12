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

int main() {
    int sockfd;
    struct sockaddr_in server_addr;
    char buffer[BUFFER_SIZE];
    ssize_t recv_len;
    int ret;
    socklen_t server_len;

    /* 创建UDP套接字 */
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket creation failed");
        exit(EXIT_FAILURE);
    }

    /* 设置服务器地址结构 */
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    server_addr.sin_addr.s_addr = inet_addr(SERVER_IP);

    printf("UDP client connected to %s:%d\n", SERVER_IP, SERVER_PORT);
    printf("Enter messages to send (type 'exit' to quit):\n");

    /* 主循环：读取用户输入并发送 */
    while (1) {
        memset(buffer, 0, BUFFER_SIZE);

        /* 读取用户输入 */
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

        /* 发送UDP数据包到服务器 */
        ret = sendto(sockfd, buffer, strlen(buffer), 0,
                    (struct sockaddr*)&server_addr, sizeof(server_addr));
        if (ret < 0) {
            perror("sendto failed");
            continue;
        }

        printf("Message sent: %s\n", buffer);

        /* 接收服务器响应 */
        memset(buffer, 0, BUFFER_SIZE);
        server_len = sizeof(server_addr);

        recv_len = recvfrom(sockfd, buffer, BUFFER_SIZE - 1, 0,
                           (struct sockaddr*)&server_addr, &server_len);
        if (recv_len < 0) {
            perror("recvfrom failed");
            continue;
        }

        /* 确保字符串以null结尾 */
        buffer[recv_len] = '\0';
        printf("Server response: %s\n", buffer);
    }

    /* 关闭套接字 */
    close(sockfd);
    return 0;
}
