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
void print_usage(const char *program_name);

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
    if (len < ip_hdr_len + ICMP_ECHOREPLY) {
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
    if (len >= ip_hdr_len + sizeof(struct icmphdr) + sizeof(struct timeval)) {
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

/* 处理ICMP ECHO请求（作为服务器） */
void handle_icmp_echo_request(char *buf, int len, struct sockaddr_in *from) {
    struct iphdr *ip_hdr;
    struct icmphdr *icmp_hdr, reply_hdr;
    int ip_hdr_len;
    char send_buf[PACKET_SIZE];
    int ret;

    /* 解析IP头部 */
    ip_hdr = (struct iphdr *)buf;
    ip_hdr_len = ip_hdr->ihl * 4;

    /* 解析ICMP头部 */
    icmp_hdr = (struct icmphdr *)(buf + ip_hdr_len);

    /* 构建回复包 */
    memset(&reply_hdr, 0, sizeof(reply_hdr));
    reply_hdr.type = ICMP_ECHOREPLY;
    reply_hdr.code = 0;
    reply_hdr.un.echo.id = icmp_hdr->un.echo.id;
    reply_hdr.un.echo.sequence = icmp_hdr->un.echo.sequence;

    /* 复制数据 */
    if (len > ip_hdr_len + sizeof(struct icmphdr)) {
        memcpy(send_buf + sizeof(reply_hdr),
               buf + ip_hdr_len + sizeof(struct icmphdr),
               len - ip_hdr_len - sizeof(struct icmphdr));
    }

    /* 计算校验和 */
    reply_hdr.checksum = 0;
    reply_hdr.checksum = calculate_checksum((unsigned short *)&reply_hdr,
                                          sizeof(reply_hdr) +
                                          (len - ip_hdr_len - sizeof(struct icmphdr)));

    /* 发送回复 */
    memcpy(send_buf, &reply_hdr, sizeof(reply_hdr));
    ret = sendto(sockfd, send_buf, sizeof(reply_hdr) +
                (len - ip_hdr_len - sizeof(struct icmphdr)), 0,
                (struct sockaddr *)from, sizeof(struct sockaddr_in));

    if (ret > 0) {
        printf("Sent ICMP ECHO reply to %s\n", inet_ntoa(from->sin_addr));
    }
}

/* 信号处理函数 */
void signal_handler(int sig) {
    printf("\n--- Statistics ---\n");
    printf("%d packets sent, %d packets received\n", packet_count, packet_count);
    close(sockfd);
    exit(0);
}

/* 解析主机名 */
int resolve_hostname(const char *hostname, struct sockaddr_in *dest) {
    struct hostent *he;

    he = gethostbyname(hostname);
    if (he == NULL) {
        herror("gethostbyname failed");
        return -1;
    }

    memset(dest, 0, sizeof(struct sockaddr_in));
    dest->sin_family = AF_INET;
    memcpy(&dest->sin_addr, he->h_addr_list[0], he->h_length);

    return 0;
}

/* 打印使用说明 */
void print_usage(const char *program_name) {
    printf("Usage: %s [options] <hostname|IP>\n", program_name);
    printf("Options:\n");
    printf("  -c count    Number of packets to send (default: 5)\n");
    printf("  -s          Run as server (listen for ICMP ECHO requests)\n");
    printf("  -h          Show this help message\n");
}

int main(int argc, char *argv[]) {
    struct sockaddr_in dest_addr;
    int server_mode = 0;
    int packet_limit = MAX_PACKETS;
    int opt;
    int i;

    /* 解析命令行参数 */
    while ((opt = getopt(argc, argv, "c:sh")) != -1) {
        switch (opt) {
            case 'c':
                packet_limit = atoi(optarg);
                if (packet_limit <= 0) {
                    packet_limit = MAX_PACKETS;
                }
                break;
            case 's':
                server_mode = 1;
                break;
            case 'h':
                print_usage(argv[0]);
                return 0;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    /* 检查参数 */
    if (optind >= argc && !server_mode) {
        fprintf(stderr, "Error: Destination host required in client mode\n");
        print_usage(argv[0]);
        return 1;
    }

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

    if (server_mode) {
        /* 服务器模式：监听并回复ICMP ECHO请求 */
        char recv_buf[PACKET_SIZE * 2];
        struct sockaddr_in client_addr;
        socklen_t addr_len;
        int n;

        printf("ICMP ECHO Server started (PID: %d)\n", pid);
        printf("Listening for ICMP ECHO requests...\n");

        while (1) {
            addr_len = sizeof(client_addr);
            n = recvfrom(sockfd, recv_buf, sizeof(recv_buf), 0,
                        (struct sockaddr *)&client_addr, &addr_len);

            if (n < 0) {
                perror("recvfrom failed");
                continue;
            }

            /* 处理ICMP ECHO请求 */
            handle_icmp_echo_request(recv_buf, n, &client_addr);
        }
    } else {
        /* 客户端模式：发送ICMP ECHO请求并等待回复 */
        if (resolve_hostname(argv[optind], &dest_addr) < 0) {
            close(sockfd);
            return 1;
        }

        printf("PING %s (%s): %d data bytes\n",
               argv[optind], inet_ntoa(dest_addr.sin_addr), DATA_SIZE);

        /* 发送多个ICMP包 */
        for (i = 1; i <= packet_limit; i++) {
            send_icmp_echo(&dest_addr, i);
            recv_icmp_reply();
            sleep(1); /* 等待1秒 */
        }

        signal_handler(SIGINT); /* 显示统计信息 */
    }

    return 0;
}
