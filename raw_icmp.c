#include <stdio.h> /* 标准输入输出库 */
#include <stdlib.h> /* 标准库（exit、malloc 等） */
#include <string.h> /* 字符串处理函数 */
#include <unistd.h> /* POSIX API（close、getpid、sleep 等） */
#include <signal.h> /* 信号处理 */
#include <sys/socket.h> /* 套接字 API */
#include <sys/time.h> /* gettimeofday、struct timeval */
#include <netinet/in.h> /* Internet 地址族 */
#include <netinet/ip.h> /* IP 头结构体 */
#include <netinet/ip_icmp.h> /* ICMP 头结构体 */
#include <arpa/inet.h> /* inet_ntoa、inet_addr */
#include <netdb.h> /* gethostbyname */
#include <errno.h> /* errno */

/* 定义常量 */
#define PACKET_SIZE 64 /* 原有的包定义（用于 ICMP 数据长度参考） */
#define DATA_SIZE 56 /* ICMP 数据区长度（与 PACKET_SIZE 对应） */
#define MAX_WAIT_TIME 3 /* select 等待超时时间（秒） */
#define MAX_PACKETS 5 /* 发送包个数 */
#define TARGET_HOST "127.0.0.1" /* 目标地址常量 */

#ifndef ICMP_ECHO /* 如果系统头文件未定义 ICMP_ECHO，则定义它 */
#define ICMP_ECHO 8 /* ICMP ECHO 请求类型 */
#endif /* 结束 ICMP_ECHO 定义 */

#ifndef ICMP_ECHOREPLY /* 如果系统头文件未定义 ICMP_ECHOREPLY，则定义它 */
#define ICMP_ECHOREPLY 0 /* ICMP ECHO REPLY 类型 */
#endif /* 结束 ICMP_ECHOREPLY 定义 */

/* ICMP 数据包结构：ICMP 头 + 固定长度数据区 */
struct icmp_packet { /* 定义 icmp_packet 结构 */
    struct icmphdr hdr; /* ICMP 头 */
    char data[DATA_SIZE]; /* ICMP 数据区，固定长度 DATA_SIZE */
}; /* 结束 icmp_packet 结构 */

/* 全局变量 */
static int sockfd; /* 原始套接字文件描述符 */
static int packet_count = 0; /* 已发送包计数 */
static pid_t pid; /* 进程 ID，用作 ICMP id 标识 */

/* 函数声明 */
unsigned short calculate_checksum(unsigned short *ptr, int nbytes); /* 计算校验和函数声明 */
void build_icmp_echo_header(struct icmp_packet *packet, int seq); /* 仅构建 ICMP 头并计算 checksum 的函数声明 */
int parse_icmp_reply(char *buf, int len, struct sockaddr_in *from); /* 解析 ICMP 回复函数声明 */
void send_icmp_echo(struct sockaddr_in *dest, int seq); /* 发送 ICMP 请求声明 */
void recv_icmp_reply(); /* 接收并解析 ICMP 回复声明 */
void signal_handler(int sig); /* 信号处理声明 */
int resolve_hostname(const char *hostname, struct sockaddr_in *dest); /* 解析主机名声明 */

/* 计算 ICMP 校验和（RFC 要求） */
unsigned short calculate_checksum(unsigned short *ptr, int nbytes) { /* 计算校验和函数定义开始 */
    unsigned long sum = 0UL; /* 累加和，使用宽类型以避免溢出 */
    unsigned short odd_byte = 0; /* 用于处理奇数字节 */
    unsigned short answer = 0; /* 最终返回值 */

    while (nbytes > 1) { /* 处理两字节为一单元的加法 */
        sum += (unsigned long)(*ptr); /* 将当前 16 位单元加到 sum */
        ptr++; /* 指针前移 */
        nbytes -= 2; /* 剩余字节减少 */
    } /* 结束 while */

    if (nbytes == 1) { /* 若还有 1 字节未处理（奇数字节） */
        /* 将最后一个字节放到 low-order byte */
        *((unsigned char *)&odd_byte) = *((unsigned char *)ptr); /* 提取单字节 */
        sum += (unsigned long)odd_byte; /* 加入 sum */
    } /* 结束 if */

    /* 将高 16 位加到低 16 位 */
    sum = (sum >> 16) + (sum & 0xffff); /* 把高位加到低位 */
    sum += (sum >> 16); /* 若仍有进位再加一次 */
    answer = (unsigned short)(~sum); /* 取反作为校验和 */

    return answer; /* 返回计算结果 */
} /* 结束 calculate_checksum */

/* 构建 ICMP 头并对整个 ICMP（头+数据）计算校验和，要求 data 区已填好 */
void build_icmp_echo_header(struct icmp_packet *packet, int seq) { /* 函数开始 */
    int icmp_len; /* 存放 icmp 的有效长度（头 + data） */

    packet->hdr.type = ICMP_ECHO; /* ICMP 类型设置为 ECHO */
    packet->hdr.code = 0; /* ICMP code 设为 0 */
    packet->hdr.un.echo.id = (unsigned short) htons((unsigned short)(pid & 0xFFFF)); /* id 使用 pid（取低 16 位），网络字节序 */
    packet->hdr.un.echo.sequence = (unsigned short) htons((unsigned short)(seq & 0xFFFF)); /* 序号，网络字节序 */
    packet->hdr.checksum = 0; /* 计算前将 checksum 置 0 */

    icmp_len = sizeof(struct icmphdr) + DATA_SIZE; /* 明确计算要参与校验的字节数，避免结构体填充问题 */
    packet->hdr.checksum = calculate_checksum((unsigned short *)packet, icmp_len); /* 计算并填入 checksum */
} /* 函数结束 build_icmp_echo_header */

/* 解析 ICMP 回复包 */
int parse_icmp_reply(char *buf, int len, struct sockaddr_in *from) { /* 函数开始 */
    struct iphdr *ip_hdr; /* 指向 IP 头 */
    struct icmphdr *icmp_hdr; /* 指向 ICMP 头 */
    int ip_hdr_len; /* IP 头长度（字节） */
    struct timeval tv_recv; /* 接收时间 */
    struct timeval tv_send; /* 发送时间（来自 data） */
    double rtt; /* 往返时延（毫秒） */
    int icmp_data_len; /* ICMP 数据长度 */
    unsigned short recv_seq; /* 从回复中读取到的序列号 */

    if (buf == NULL) { /* 校验参数 */
        return -1; /* 参数错误返回 */
    } /* 结束 if */

    gettimeofday(&tv_recv, NULL); /* 获取接收时间用于 RTT 计算 */

    ip_hdr = (struct iphdr *)buf; /* IP 头起始地址 */
    ip_hdr_len = (int)(ip_hdr->ihl * 4); /* ip->ihl 单位为 32-bit words */

    if (len < ip_hdr_len + (int) sizeof(struct icmphdr)) { /* 若数据不足以包含 ICMP 头则失败 */
        return -1; /* 数据不完整 */
    } /* 结束 if */

    icmp_hdr = (struct icmphdr *)(buf + ip_hdr_len); /* 定位 ICMP 头 */

    if ((int)icmp_hdr->type != ICMP_ECHOREPLY) { /* 只处理 ECHO REPLY */
        return -1; /* 非预期类型 */
    } /* 结束 if */

    /* 检查标识符是否匹配（注意网络序转换） */
    if ((unsigned short) ntohs(icmp_hdr->un.echo.id) != (unsigned short)(pid & 0xFFFF)) { /* id 不匹配忽略 */
        return -1; /* 不是发给本进程的回复 */
    } /* 结束 if */

    /* 解析序列号（网络序 -> 主机序） */
    recv_seq = (unsigned short) ntohs(icmp_hdr->un.echo.sequence); /* 提取并转换序号 */

    /* 计算 RTT：如果 data 部分足够大并且包含发送时的 timeval，则计算 RTT */
    icmp_data_len = len - ip_hdr_len - (int) sizeof(struct icmphdr); /* ICMP 数据区当前实际长度 */
    if (icmp_data_len >= (int) sizeof(struct timeval)) { /* 若数据区能包含 timeval */
        memcpy(&tv_send, buf + ip_hdr_len + sizeof(struct icmphdr), sizeof(struct timeval)); /* 从数据区复制发送时间 */
        rtt = ((double)(tv_recv.tv_sec - tv_send.tv_sec)) * 1000.0 + ((double)(tv_recv.tv_usec - tv_send.tv_usec)) / 1000.0; /* 计算毫秒级 RTT */
    } else { /* 数据区不含时间戳 */
        rtt = 0.0; /* 未能计算 RTT */
    } /* 结束 if */

    /* 打印回复信息：字节数 = 总长度 - IP 头长度 */
    printf("%d bytes from %s: icmp_seq=%u ttl=%d", len - ip_hdr_len, inet_ntoa(from->sin_addr), (unsigned int)recv_seq, (int)ip_hdr->ttl); /* 输出基本信息 */

    if (rtt > 0.0) { /* 若有 RTT 才打印 */
        printf(" time=%.3f ms", rtt); /* 打印 RTT，毫秒格式 */
    } /* 结束 if */

    printf("\n"); /* 换行 */

    return 0; /* 成功解析 */
} /* 结束 parse_icmp_reply */

/* 发送 ICMP ECHO 请求（注意：data 需先填好，再计算 checksum） */
void send_icmp_echo(struct sockaddr_in *dest, int seq) { /* 函数开始 */
    struct icmp_packet packet; /* 声明包变量 */
    struct timeval tv; /* 时间戳变量 */
    int ret; /* sendto 返回值 */
    int i; /* 循环索引 */

    memset(&packet, 0, sizeof(packet)); /* 清零整个 packet，包括 hdr 和 data */

    gettimeofday(&tv, NULL); /* 获取当前时间作为发送时间戳 */
    memcpy(packet.data, &tv, sizeof(tv)); /* 将 timeval 放在 data 区开头（parse_icmp_reply 期望此处） */

    /* 填充剩余 data（可选，用于识别包） */
    i = sizeof(tv); /* 在 C89 中先声明再使用 */
    while (i < DATA_SIZE) { /* 填充 pattern 字节 */
        packet.data[i] = (char)(i % 256); /* 简单填充 */
        i++; /* 增加索引 */
    } /* 结束 while */

    /* 构建 ICMP 头并计算 checksum（data 已经准备好） */
    build_icmp_echo_header(&packet, seq); /* 构建 header 并计算 checksum */

    /* 发送数据包（整体长度为 icmp header + data） */
    ret = sendto(sockfd, (const void *)&packet, sizeof(struct icmphdr) + DATA_SIZE, 0, (struct sockaddr *)dest, sizeof(struct sockaddr_in)); /* 发送到目的地址 */

    if (ret < 0) { /* 发送失败处理 */
        perror("sendto failed"); /* 打印错误信息 */
    } else { /* 发送成功 */
        packet_count++; /* 递增已发送计数 */
        printf("Sent ICMP ECHO request to %s, seq=%d\n", inet_ntoa(dest->sin_addr), seq); /* 打印发送日志 */
    } /* 结束 if */
} /* 结束 send_icmp_echo */

/* 接收 ICMP 回复，并在收到匹配的回复时返回 */
void recv_icmp_reply() { /* 函数开始 */
    fd_set readfds; /* select 使用的集合 */
    struct timeval tv; /* select 超时 */
    char recv_buf[1500]; /* 接收缓冲区，增大到 1500 字节以避免截断 */
    struct sockaddr_in from; /* 源地址 */
    socklen_t from_len; /* 源地址长度 */
    int ret; /* select 返回值 */
    int n; /* recvfrom 返回的字节数 */

    while (1) { /* 循环等待直到超时或收到匹配回复 */
        FD_ZERO(&readfds); /* 清空集合 */
        FD_SET(sockfd, &readfds); /* 将套接字加入集合 */

        tv.tv_sec = MAX_WAIT_TIME; /* 设置秒级超时时间 */
        tv.tv_usec = 0; /* 微秒清零 */

        ret = select(sockfd + 1, &readfds, NULL, NULL, &tv); /* 等待数据可读 */
        if (ret < 0) { /* select 出错处理 */
            if (errno == EINTR) { /* 被信号中断，继续等待 */
                continue; /* 继续循环 */
            } /* 结束 if */
            perror("select failed"); /* 打印错误 */
            break; /* 跳出循环 */
        } else if (ret == 0) { /* 超时 */
            printf("Request timeout\n"); /* 打印超时信息 */
            break; /* 跳出等待循环 */
        } else { /* 有套接字可读 */
            if (FD_ISSET(sockfd, &readfds)) { /* 确认 sockfd 可读 */
                from_len = sizeof(from); /* 设置 from 长度 */
                n = recvfrom(sockfd, recv_buf, sizeof(recv_buf), 0, (struct sockaddr *)&from, &from_len); /* 接收数据 */
                if (n < 0) { /* recvfrom 错误处理 */
                    perror("recvfrom failed"); /* 打印错误信息 */
                    continue; /* 继续等待下一次 */
                } /* 结束 if */

                /* 尝试解析 ICMP 回复，若成功则返回（只接受第一个匹配的回复） */
                if (parse_icmp_reply(recv_buf, n, &from) == 0) { /* 解析成功 */
                    break; /* 收到匹配回复，跳出循环 */
                } /* 结束 if */
            } /* 结束 FD_ISSET 判断 */
        } /* 结束 select 结果判断 */
    } /* 结束 while */
} /* 结束 recv_icmp_reply */

/* 信号处理函数：打印统计并退出 */
void signal_handler(int sig) { /* 函数开始 */
    (void)sig; /* 避免未使用参数的编译警告 */
    printf("\n--- Statistics ---\n"); /* 打印分隔线 */
    printf("%d packets sent\n", packet_count); /* 打印已发送包数量 */
    if (sockfd >= 0) { /* 若套接字有效则关闭 */
        close(sockfd); /* 关闭套接字 */
    } /* 结束 if */
    exit(0); /* 退出进程 */
} /* 结束 signal_handler */

/* 解析主机名到 sockaddr_in */
int resolve_hostname(const char *hostname, struct sockaddr_in *dest) { /* 函数开始 */
    struct hostent *he; /* gethostbyname 返回结构 */

    he = gethostbyname(hostname); /* 调用 DNS/hosts 解析 */
    if (he == NULL) { /* 解析失败 */
        perror("gethostbyname failed"); /* 打印错误信息 */
        return -1; /* 返回错误 */
    } /* 结束 if */

    memset(dest, 0, sizeof(struct sockaddr_in)); /* 清空结构体 */
    dest->sin_family = AF_INET; /* IPv4 */
    memcpy(&dest->sin_addr, he->h_addr_list[0], he->h_length); /* 复制解析得到的第一个地址 */

    return 0; /* 成功返回 */
} /* 结束 resolve_hostname */

/* 主程序入口 */
int main(int argc, char *argv[]) { /* main 开始 */
    struct sockaddr_in dest_addr; /* 目的地址结构 */
    int i; /* 循环索引变量 */

    (void)argc; /* 未使用参数避免警告 */
    (void)argv; /* 未使用参数避免警告 */

    pid = getpid(); /* 获取进程 ID 作为 ICMP id 的来源 */

    sockfd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP); /* 创建原始套接字用于 ICMP */
    if (sockfd < 0) { /* 创建失败处理 */
        perror("socket creation failed"); /* 打印错误原因 */
        if (errno == EPERM) { /* 若是权限问题给出提示 */
            fprintf(stderr, "Note: This program requires root privileges\n"); /* 提示需要 root */
        } /* 结束 if */
        return 1; /* 退出程序 */
    } /* 结束 if */

    signal(SIGINT, signal_handler); /* 捕捉 Ctrl-C（SIGINT） */
    signal(SIGTERM, signal_handler); /* 捕捉终止信号（SIGTERM） */

    if (resolve_hostname(TARGET_HOST, &dest_addr) < 0) { /* 解析目标地址 */
        close(sockfd); /* 解析失败则关闭套接字 */
        return 1; /* 返回错误 */
    } /* 结束 if */

    printf("PING %s (%s): %d data bytes\n", TARGET_HOST, inet_ntoa(dest_addr.sin_addr), DATA_SIZE); /* 打印启动信息 */

    i = 1; /* 初始化循环计数 */
    while (i <= MAX_PACKETS) { /* 循环发送若干次 */
        send_icmp_echo(&dest_addr, i); /* 发送 ICMP ECHO 请求 */
        recv_icmp_reply(); /* 等待并接收回复（或超时） */
        sleep(1); /* 等待 1 秒再发下一次 */
        i++; /* 计数递增 */
    } /* 结束 while */

    signal_handler(SIGINT); /* 手动调用以打印统计并退出 */

    return 0; /* 正常返回 */
} /* 结束 main */
