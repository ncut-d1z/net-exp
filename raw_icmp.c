#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>
#include "safeio.h"

/* 定义常量 */
#define PACKET_SIZE 64
#define DATA_SIZE 56
#define MAX_WAIT_TIME 3
#define MAX_PACKETS 5
#define TARGET_HOST "127.0.0.1"

#ifndef ICMP_ECHO
#define ICMP_ECHO 8
#endif
#ifndef ICMP_ECHOREPLY
#define ICMP_ECHOREPLY 0
#endif

/* ICMP数据包结构 */
struct icmp_packet {
    struct icmphdr hdr;
    char data[DATA_SIZE];
};

/* 全局变量 */
static int sockfd;
static int packet_count = 0;
static pid_t pid;

/* 函数声明 */
unsigned short calculate_checksum(unsigned short *ptr, int nbytes);
void build_icmp_echo(struct icmp_packet *packet, int seq);
int parse_icmp_reply(char *buf, int len, struct sockaddr_in *from, int seq);
void send_icmp_echo(struct sockaddr_in *dest, int seq);
void recv_icmp_reply();
void signal_handler(int sig);
int resolve_hostname(const char *hostname, struct sockaddr_in *dest);

/* 计算ICMP校验和 */
unsigned short calculate_checksum(unsigned short *ptr, int nbytes) {
    unsigned long sum = 0;
    unsigned short odd_byte = 0;
    unsigned short answer = 0;

    while (nbytes > 1) {
        sum += *ptr++;
        nbytes -= 2;
    }

    if (nbytes == 1) {
        odd_byte = 0;
        *((unsigned char *)&odd_byte) = *(unsigned char *)ptr;
        sum += odd_byte;
    }

    sum = (sum >> 16) + (sum & 0xffff);
    sum += (sum >> 16);
    answer = (unsigned short)~sum;

    return answer;
}

/* 构建ICMP ECHO请求包 */
void build_icmp_echo(struct icmp_packet *packet, int seq) {
    int i;

    /* 清空数据包 */
    memset(packet, 0, sizeof(struct icmp_packet));

    /* 设置ICMP头部 */
    packet->hdr.type = ICMP_ECHO;
    packet->hdr.code = 0;
    packet->hdr.un.echo.id = htons(pid);
    packet->hdr.un.echo.sequence = htons(seq);

    /* 填充数据部分 */
    for (i = 0; i < DATA_SIZE; i++) {
        packet->data[i] = i % 256;
    }

    /* 计算校验和 */
    packet->hdr.checksum = 0;
    packet->hdr.checksum = calculate_checksum((unsigned short *)packet,
                                             sizeof(struct icmp_packet));
}

/* 解析ICMP回复包 */
int parse_icmp_reply(char *buf, int len, struct sockaddr_in *from, int seq) {
    struct iphdr *ip_hdr;
    struct icmphdr *icmp_hdr;
    int ip_hdr_len;
    struct timeval tv_recv, tv_send;
    double rtt;

    /* 获取当前时间 */
    gettimeofday(&tv_recv, NULL);

    /* 解析IP头部 */
    ip_hdr = (struct iphdr *)buf;
    ip_hdr_len = ip_hdr->ihl * 4;

    /* 检查数据包长度 */
    if (len < ip_hdr_len + (int) sizeof(struct icmphdr)) {
        return -1;
    }

    /* 解析ICMP头部 */
    icmp_hdr = (struct icmphdr *)(buf + ip_hdr_len);

    /* 检查是否是ICMP ECHO REPLY */
    if (icmp_hdr->type != ICMP_ECHOREPLY) {
        return -1;
    }

    /* 检查标识符是否匹配 */
    if (ntohs(icmp_hdr->un.echo.id) != pid) {
        return -1;
    }

    /* 提取发送时间（从数据部分） */
    if (len >= ip_hdr_len + (int) sizeof(struct icmphdr) + (int) sizeof(struct timeval)) {
        memcpy(&tv_send, buf + ip_hdr_len + sizeof(struct icmphdr),
               sizeof(struct timeval));

        /* 计算往返时间 */
        rtt = (tv_recv.tv_sec - tv_send.tv_sec) * 1000.0 +
              (tv_recv.tv_usec - tv_send.tv_usec) / 1000.0;
    } else {
        rtt = 0.0;
    }

    /* 打印回复信息 */
    printf("%d bytes from %s: icmp_seq=%d ttl=%d",
           len - ip_hdr_len,
           inet_ntoa(from->sin_addr),
           seq,
           ip_hdr->ttl);

    if (rtt > 0) {
        printf(" time=%.3f ms", rtt);
    }
    printf("\n");

    return 0;
}

/* 发送ICMP ECHO请求 */
void send_icmp_echo(struct sockaddr_in *dest, int seq) {
    struct icmp_packet packet;
    struct timeval tv;
    int ret;

    /* 构建ICMP包 */
    build_icmp_echo(&packet, seq);

    /* 添加时间戳到数据部分 */
    gettimeofday(&tv, NULL);
    memcpy(packet.data, &tv, sizeof(tv));

    /* 发送数据包 */
    ret = sendto(sockfd, &packet, sizeof(packet), 0,
                (struct sockaddr *)dest, sizeof(struct sockaddr_in));

    if (ret < 0) {
        perror("sendto failed");
    } else {
        packet_count++;
        printf("Sent ICMP ECHO request to %s, seq=%d\n",
               inet_ntoa(dest->sin_addr), seq);
    }
}

/* 接收ICMP回复 */
void recv_icmp_reply() {
    fd_set readfds;
    struct timeval tv;
    char recv_buf[PACKET_SIZE];
    struct sockaddr_in from;
    socklen_t from_len;
    int ret, n;

    while (1) {
        FD_ZERO(&readfds);
        FD_SET(sockfd, &readfds);

        tv.tv_sec = MAX_WAIT_TIME;
        tv.tv_usec = 0;

        /* 使用select等待数据到达 */
        ret = select(sockfd + 1, &readfds, NULL, NULL, &tv);

        if (ret < 0) {
            if (errno == EINTR) {
                continue; /* 信号中断 */
            }
            perror("select failed");
            break;
        } else if (ret == 0) {
            /* 超时 */
            printf("Request timeout\n");
            break;
        } else {
            if (FD_ISSET(sockfd, &readfds)) {
                /* 接收数据 */
                from_len = sizeof(from);
                n = recvfrom(sockfd, recv_buf, sizeof(recv_buf), 0,
                           (struct sockaddr *)&from, &from_len);

                if (n < 0) {
                    perror("recvfrom failed");
                    continue;
                }

                /* 尝试解析ICMP回复 */
                if (parse_icmp_reply(recv_buf, n, &from, packet_count) == 0) {
                    break; /* 成功接收到匹配的回复 */
                }
            }
        }
    }
}

/* 信号处理函数 */
void signal_handler(int sig) {
    (void)sig; /* 避免未使用参数警告 */
    printf("\n--- Statistics ---\n");
    printf("%d packets sent\n", packet_count);
    close(sockfd);
    exit(0);
}

/* 解析主机名 */
int resolve_hostname(const char *hostname, struct sockaddr_in *dest) {
    struct hostent *he;

    he = gethostbyname(hostname);
    if (he == NULL) {
        perror("gethostbyname failed");
        return -1;
    }

    memset(dest, 0, sizeof(struct sockaddr_in));
    dest->sin_family = AF_INET;
    memcpy(&dest->sin_addr, he->h_addr_list[0], he->h_length);

    return 0;
}

int main(int argc, char *argv[]) {
    struct sockaddr_in dest_addr;
    int i;

    /* 获取进程ID作为标识符 */
    pid = getpid();

    /* 创建原始套接字 */
    sockfd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (sockfd < 0) {
        perror("socket creation failed");
        if (errno == EPERM) {
            fprintf(stderr, "Note: This program requires root privileges\n");
        }
        return 1;
    }

    /* 设置信号处理 */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* 解析目标地址 */
    if (resolve_hostname(TARGET_HOST, &dest_addr) < 0) {
        close(sockfd);
        return 1;
    }

    printf("PING %s (%s): %d data bytes\n",
           TARGET_HOST, inet_ntoa(dest_addr.sin_addr), DATA_SIZE);

    /* 发送多个ICMP包 */
    for (i = 1; i <= MAX_PACKETS; i++) {
        send_icmp_echo(&dest_addr, i);
        recv_icmp_reply();
        sleep(1); /* 等待1秒 */
    }

    signal_handler(SIGINT); /* 显示统计信息 */

    return 0;
}
