#include <stdio.h>                      /* 标准输入输出 */
#include <stdlib.h>                     /* 标准库：malloc, free, atoi, exit */
#include <string.h>                     /* 字符串处理，如 memset, memcpy, strcmp, strncpy */
#include <errno.h>                      /* errno 与错误解释 */
#include <unistd.h>                     /* close, getpid, _exit */
#include <sys/types.h>                  /* 基本系统类型定义 */
#include <sys/socket.h>                 /* socket, bind, sendto, recvfrom 等 */
#include <netdb.h>                      /* getaddrinfo, getnameinfo, gai_strerror */
#include <arpa/inet.h>                  /* inet_ntop 等地址文本转换 */
#include <netinet/in.h>                 /* sockaddr_in6, in6addr_any */
#include <sys/time.h>                   /* gettimeofday, struct timeval */
#include <signal.h>                     /* signal */
#include <netinet/icmp6.h>              /* ICMPv6 报文结构与常量（在 Linux 下通常可用） */

/* 常量定义 */
#define DEFAULT_MAX_HOPS 30            /* 默认最大跳数 */
#define DEFAULT_PROBES 3               /* 默认每跳探测次数 */
#define DEFAULT_TIMEOUT_MS 3000        /* 默认超时 3000 毫秒 */
#define PACKET_SIZE 56                 /* ICMP payload 大小（字节） */
#define INET6_STRLEN 46                /* IPv6 文本最长长度（INET6_ADDRSTRLEN 常量通常为46） */

/* 全局套接字变量，便于在信号处理时关闭 */
static int g_sock = -1;                 /* 用于发送与接收 ICMPv6 的原始套接字 */

/* 信号处理函数，确保在中断时关闭套接字并退出 */
static void cleanup_and_exit(int signo) /* 信号处理函数声明 */
{
    (void)signo; /* 标记参数为未使用，消除警告 */
    if (g_sock >= 0) {                  /* 若套接字已打开 */
        close(g_sock);                  /* 关闭原始套接字 */
        g_sock = -1;                    /* 标记已关闭 */
    }
    _exit(1);                           /* 使用 _exit 在信号处理上下文安全退出 */
}

/* 计算 timeval 差异并以毫秒返回（整数） */
static long time_diff_ms(struct timeval *endtv, struct timeval *starttv) /* 返回毫秒差 */
{
    long sec_diff;                      /* 秒差变量 */
    long usec_diff;                     /* 微秒差变量 */
    long msec;                          /* 结果毫秒变量 */

    sec_diff = endtv->tv_sec - starttv->tv_sec; /* 计算秒差 */
    usec_diff = endtv->tv_usec - starttv->tv_usec; /* 计算微秒差 */
    msec = sec_diff * 1000L + usec_diff / 1000L;   /* 转换并合并为毫秒 */
    return msec;                        /* 返回毫秒差 */
}

/* 将 sockaddr_in6 地址转换为文本字符串，buf 长度应至少为 INET6_STRLEN */
static const char *addr6_to_str(const struct sockaddr_in6 *sin6, char *buf, size_t buflen) /* 地址文本化 */
{
    if (inet_ntop(AF_INET6, &(sin6->sin6_addr), buf, (socklen_t)buflen) == NULL) { /* 转换失败 */
        strncpy(buf, "?", buflen);      /* 失败时写入占位符 */
        buf[buflen - 1] = '\0';         /* 确保以 NUL 结尾 */
    }
    return buf;                         /* 返回缓冲区指针 */
}

/* 打印 ICMPv6 类型与代码的可读信息（简要） */
static void print_icmp6_info(unsigned char type, unsigned char code) /* 根据类型/代码打印信息 */
{
    if (type == ICMP6_TIME_EXCEEDED) {  /* 如果类型为 Time Exceeded */
        printf(" (ICMP6: Time Exceeded)"); /* 打印说明 */
    } else if (type == ICMP6_DST_UNREACH) { /* 如果类型为 Destination Unreachable */
        printf(" (ICMP6: Destination Unreachable, code=%u)", (unsigned int)code); /* 打印并包含 code */
    } else if (type == ICMP6_ECHO_REPLY) { /* 如果类型为 Echo Reply */
        printf(" (ICMP6: Echo Reply)");    /* 打印说明 */
    } else {                            /* 其它类型 */
        printf(" (ICMP6: type=%u code=%u)", (unsigned int)type, (unsigned int)code); /* 打印类型与代码 */
    }
}

/* 主程序 */
int main(int argc, char *argv[]) /* argc/argv 参数 */
{
    struct addrinfo hints;              /* getaddrinfo 提供的 hints 结构 */
    struct addrinfo *res = NULL;        /* getaddrinfo 返回的结果指针 */
    int ret;                            /* 通用返回值变量 */
    char *target;                       /* 目标主机名或 IPv6 文本 */
    int max_hops;                       /* 最大跳数 */
    int probes;                         /* 每跳探测次数 */
    int timeout_ms;                     /* 超时毫秒 */
    int pid;                            /* 进程 id，用作 ICMP id 字段 */
    int hop;                            /* 当前 hop 索引 */
    int probe;                          /* 当前 probe 索引 */
    struct timeval tv_start;            /* 发送时间 */
    struct timeval tv_end;              /* 接收时间 */
    fd_set readfds;                     /* select 的读集合 */
    struct timeval select_tv;           /* select 超时时间 */
    int nfds;                           /* select 的第一个参数 */
    int rv;                             /* select 返回值 */
    ssize_t n;                          /* recvfrom/sendto 返回值 */
    char sendbuf[PACKET_SIZE];          /* 发送缓冲（包含 ICMPv6 头和 payload） */
    char recvbuf[1500];                 /* 接收缓冲 */
    struct sockaddr_in6 dest_sa;        /* 目标 IPv6 地址 */
    socklen_t dest_len;                 /* 目标地址长度 */
    struct sockaddr_in6 from_sa;        /* 接收方地址 */
    socklen_t from_len;                 /* 接收方地址长度 */
    char addrstr[INET6_STRLEN];         /* 源地址文本缓冲 */
    int hop_limit_opt;                  /* hop limit 用于 setsockopt */
    int seq;                            /* ICMP 序列号 */
    struct icmp6_hdr *icmp6;            /* 指向发送/接收缓冲中 icmp6 头的指针 */
    long rtt;                           /* 单次 RTT（毫秒） */
    long rtt_min;                       /* 本跳最小 RTT */
    long rtt_max;                       /* 本跳最大 RTT */
    long rtt_sum;                       /* 本跳 RTT 总和（用于计算平均） */
    char hostbuf[NI_MAXHOST];           /* getnameinfo 主机名缓冲 */
    int have_name;                      /* 是否成功解析到主机名 */

    /* 参数检查与解析 */
    if (argc < 2) {                     /* 如果没有提供目标 */
        fprintf(stderr, "Usage: %s <destination> [max_hops] [probes] [timeout_ms]\n", argv[0]); /* 打印用法 */
        return 1;                       /* 退出 */
    }
    target = argv[1];                   /* 取得目标字符串 */
    if (argc >= 3) {                    /* 若提供了 max_hops */
        max_hops = atoi(argv[2]);       /* 转换为整数 */
        if (max_hops <= 0) max_hops = DEFAULT_MAX_HOPS; /* 非法值用默认 */
    } else {
        max_hops = DEFAULT_MAX_HOPS;    /* 未提供则用默认 */
    }
    if (argc >= 4) {                    /* 若提供了 probes */
        probes = atoi(argv[3]);         /* 转换为整数 */
        if (probes <= 0) probes = DEFAULT_PROBES; /* 非法值用默认 */
    } else {
        probes = DEFAULT_PROBES;        /* 默认探测次数 */
    }
    if (argc >= 5) {                    /* 若提供了超时毫秒 */
        timeout_ms = atoi(argv[4]);     /* 转换为整数 */
        if (timeout_ms <= 0) timeout_ms = DEFAULT_TIMEOUT_MS; /* 非法值用默认 */
    } else {
        timeout_ms = DEFAULT_TIMEOUT_MS;/* 默认超时 */
    }

    /* 注册信号处理器，确保 Ctrl-C 等能干净退出 */
    signal(SIGINT, cleanup_and_exit);   /* 注册 SIGINT （Ctrl-C） */
    signal(SIGTERM, cleanup_and_exit);  /* 注册 SIGTERM */

    /* 解析目标地址（只接受 IPv6） */
    memset(&hints, 0, sizeof(hints));   /* 清零 hints */
    hints.ai_family = AF_INET6;         /* 只解析 IPv6 */
    hints.ai_socktype = SOCK_RAW;       /* raw type（仅用于协助 getaddrinfo，但不实际创建 socket） */
    hints.ai_flags = 0;                 /* 无特别标志 */
    ret = getaddrinfo(target, NULL, &hints, &res); /* 解析目标 */
    if (ret != 0) {                     /* 解析失败 */
        fprintf(stderr, "getaddrinfo(%s) failed: %s\n", target, gai_strerror(ret)); /* 打印错误 */
        return 1;                       /* 退出 */
    }

    /* 复制目标地址并设置端口为0（ICMP 不使用 UDP/TCP 端口） */
    memset(&dest_sa, 0, sizeof(dest_sa)); /* 清零目标结构 */
    if (res->ai_family == AF_INET6) {
        memcpy(&dest_sa, res->ai_addr, res->ai_addrlen); /* 复制地址结构 */
    } else {
        fprintf(stderr, "Error: Target is not an IPv6 address\n");
        freeaddrinfo(res);
        return 1;
    }
    dest_sa.sin6_port = htons(0);        /* 端口设为 0 （与 ICMP 无关） */
    dest_len = (socklen_t)res->ai_addrlen; /* 保存地址长度 */
    freeaddrinfo(res);                  /* 释放 getaddrinfo 结果 */

    /* 创建原始 ICMPv6 套接字用于发送与接收 */
    g_sock = socket(AF_INET6, SOCK_RAW, IPPROTO_ICMPV6); /* 创建原始套接字，协议 ICMPv6 */
    if (g_sock < 0) {                    /* 创建失败 */
        perror("socket (raw ICMPv6)");  /* 打印错误 */
        return 1;                        /* 退出 */
    }

    /* 取得进程 id 并用作 ICMP 标识 id 字段 */
    pid = getpid() & 0xFFFF;             /* 使用低 16 位作为 id */

    /* 打印启动信息 */
    printf("tr6_icmp_echo_traceroute to %s, max_hops %d, probes %d, timeout %d ms\n", target, max_hops, probes, timeout_ms); /* 输出配置信息 */

    /* 主循环：逐跳发送 ICMPv6 Echo 报文 */
    seq = 0;                             /* 初始化序列号 */
    for (hop = 1; hop <= max_hops; hop++) { /* 对每一跳从 1 开始到 max_hops */
        int replied_count;               /* 本跳收到回复的次数计数 */
        replied_count = 0;               /* 初始化为 0 */
        rtt_min = 0L;                    /* 初始化最小 RTT */
        rtt_max = 0L;                    /* 初始化最大 RTT */
        rtt_sum = 0L;                    /* 初始化 RTT 总和 */

        printf("%2d  ", hop);            /* 打印当前跳号 */
        fflush(stdout);                  /* 立即刷新输出以便实时显示 */

        for (probe = 0; probe < probes; probe++) { /* 对每跳进行 probes 次探测 */
            /* 设置发送报文的 hop limit（IPv6 的 TTL） */
            hop_limit_opt = hop;         /* 将 hop 值赋给 hop_limit_opt */
            if (setsockopt(g_sock, IPPROTO_IPV6, IPV6_UNICAST_HOPS, (void *)&hop_limit_opt, sizeof(hop_limit_opt)) < 0) { /* 设置 hop limit */
                perror("setsockopt IPV6_UNICAST_HOPS"); /* 若失败则打印但继续 */
            }

            /* 构造 ICMPv6 Echo 请求报文，放在 sendbuf 中 */
            memset(sendbuf, 0, sizeof(sendbuf)); /* 清零发送缓冲 */
            icmp6 = (struct icmp6_hdr *)sendbuf; /* 将 icmp6 指针指向缓冲起始处以填充头部 */
            icmp6->icmp6_type = ICMP6_ECHO_REQUEST; /* 设置类型为 Echo Request */
            icmp6->icmp6_code = 0;        /* Echo 请求的 code 为 0 */
            icmp6->icmp6_cksum = 0;       /* 检查和由内核计算（通常对 IPv6 raw socket 内核会计算） */
            icmp6->icmp6_id = htons((unsigned short)pid); /* 设置标识（使用进程 id） */
            icmp6->icmp6_seq = htons((unsigned short)seq); /* 设置序列号（网络字节序） */

            /* 在 echo payload 中放一些标识信息（如 time 或 seq 等）以供调试 */
            /* 这里 payload 起始于 sendbuf + sizeof(struct icmp6_hdr) */
            /* 我们把当前时间微秒（低 4 字节）放到 payload 的前 4 字节以便后续分析（可选） */
            if (gettimeofday(&tv_start, NULL) < 0) { /* 获取当前时间作为发送时间 */
                perror("gettimeofday");    /* 若失败则打印但继续 */
            }
            /* 将 tv_start.tv_usec 拷贝到 payload 中（4 字节） */
            sendbuf[sizeof(struct icmp6_hdr) + 0] = (char)((tv_start.tv_usec >> 24) & 0xFF); /* payload 字节0 */
            sendbuf[sizeof(struct icmp6_hdr) + 1] = (char)((tv_start.tv_usec >> 16) & 0xFF); /* payload 字节1 */
            sendbuf[sizeof(struct icmp6_hdr) + 2] = (char)((tv_start.tv_usec >> 8) & 0xFF);  /* payload 字节2 */
            sendbuf[sizeof(struct icmp6_hdr) + 3] = (char)(tv_start.tv_usec & 0xFF);         /* payload 字节3 */

            /* 记录发送时刻（再次记录，确保用于 RTT 计算的是发送时间） */
            if (gettimeofday(&tv_start, NULL) < 0) { /* 获取发送时间 */
                perror("gettimeofday");    /* 打印但继续 */
            }

            /* 发送 ICMPv6 Echo 请求到目标地址 */
            n = sendto(g_sock, sendbuf, sizeof(struct icmp6_hdr) + PACKET_SIZE - sizeof(struct icmp6_hdr), 0, (struct sockaddr *)&dest_sa, dest_len); /* 发送 raw ICMPv6 报文 */
            if (n < 0) {                  /* 发送失败 */
                perror("sendto");         /* 打印错误 */
                printf(" *");             /* 打印星号表示该次 probe 失败 */
                fflush(stdout);          /* 刷新输出 */
                seq++;                   /* 序列号自增以避免重用 */
                continue;                /* 继续下一个 probe */
            }

            /* 使用 select 等待接收响应或超时 */
            FD_ZERO(&readfds);            /* 清空 readfds */
            FD_SET(g_sock, &readfds);     /* 将原始套接字加入集合 */
            select_tv.tv_sec = timeout_ms / 1000; /* 设置 select 超时秒 */
            select_tv.tv_usec = (timeout_ms % 1000) * 1000; /* 设置 select 超时微秒 */
            nfds = g_sock + 1;            /* nfds 为最大 fd + 1 */

            rv = select(nfds, &readfds, NULL, NULL, &select_tv); /* 等待可读事件或超时 */
            if (rv == -1) {               /* select 出错 */
                if (errno == EINTR) {     /* 被信号打断 */
                    printf(" *");         /* 标注为星号 */
                    fflush(stdout);      /* 刷新 */
                    seq++;               /* 自增序列号 */
                    continue;            /* 继续下一个 probe */
                }
                perror("select");         /* 其它错误打印 */
                printf(" *");             /* 标注星号 */
                fflush(stdout);          /* 刷新 */
                seq++;                   /* 自增序列号 */
                continue;                /* 继续下一个 probe */
            } else if (rv == 0) {         /* 超时 */
                printf(" *");             /* 打印星号 */
                fflush(stdout);          /* 刷新 */
                seq++;                   /* 自增序列号 */
                continue;                /* 继续下一个 probe */
            } else {                       /* 有可读数据 */
                from_len = sizeof(from_sa); /* 初始化接收地址长度 */
                n = recvfrom(g_sock, recvbuf, sizeof(recvbuf), 0, (struct sockaddr *)&from_sa, &from_len); /* 接收数据 */
                if (n < 0) {              /* recvfrom 失败 */
                    if (errno == EINTR) { /* 信号中断 */
                        printf(" *");     /* 打星号 */
                        fflush(stdout);  /* 刷新 */
                        seq++;           /* 自增序列号 */
                        continue;        /* 继续下一个 probe */
                    }
                    perror("recvfrom");    /* 其它错误打印 */
                    printf(" *");         /* 标注 */
                    fflush(stdout);      /* 刷新 */
                    seq++;               /* 自增序列号 */
                    continue;            /* 继续下一个 probe */
                }

                /* 接收到回应，记录时间并计算 RTT */
                if (gettimeofday(&tv_end, NULL) < 0) { /* 获取接收时间 */
                    perror("gettimeofday"); /* 打印但继续 */
                }
                rtt = time_diff_ms(&tv_end, &tv_start); /* 计算 RTT（毫秒） */

                /* 解析 ICMPv6 报文的类型与代码 */
                /* 对于原始 ICMPv6 套接字，recvbuf 起始一般就是 icmp6 头 */
                icmp6 = (struct icmp6_hdr *)recvbuf; /* 指向接收到的数据首部 */
                /* 打印来源地址（以及可能的反向 DNS 名称） */
                addr6_to_str(&from_sa, addrstr, sizeof(addrstr)); /* 将来源地址转为文本 */
                have_name = 0;                /* 初始化没有解析到主机名 */
                memset(hostbuf, 0, sizeof(hostbuf)); /* 清零主机名缓冲 */
                ret = getnameinfo((struct sockaddr *)&from_sa, from_len, hostbuf, sizeof(hostbuf), NULL, 0, 0); /* 反向 DNS 解析 */
                if (ret == 0) {              /* 若解析成功 */
                    have_name = 1;           /* 标记成功 */
                }

                /* 打印本次 probe 的结果信息（地址/主机名/RTT/ICMP 类型说明） */
                if (have_name) {             /* 如果解析到了主机名 */
                    printf(" %s (%s)  %ld ms", hostbuf, addrstr, rtt); /* 打印主机名 (地址) RTT */
                } else {
                    printf(" %s  %ld ms", addrstr, rtt); /* 仅打印地址与 RTT */
                }
                /* 打印 ICMPv6 类型/代码的可读信息 */
                if (n >= (ssize_t)sizeof(struct icmp6_hdr)) { /* 确保接收到的数据至少包含 icmp6 头 */
                    print_icmp6_info(icmp6->icmp6_type, icmp6->icmp6_code); /* 打印类型/代码说明 */
                }

                fflush(stdout);             /* 刷新输出 */

                /* 累计 RTT 统计量（最小/最大/和） */
                if (replied_count == 0) {    /* 如果是本跳首次收到回复 */
                    rtt_min = rtt;           /* 初始化最小 RTT */
                    rtt_max = rtt;           /* 初始化最大 RTT */
                    rtt_sum = rtt;           /* 初始化和 */
                } else {
                    if (rtt < rtt_min) rtt_min = rtt; /* 更新最小 */
                    if (rtt > rtt_max) rtt_max = rtt; /* 更新最大 */
                    rtt_sum += rtt;         /* 累计和 */
                }
                replied_count++;            /* 增加回复计数 */

                /* 如果收到的 ICMP 类型是 Echo Reply 且来源地址就是目标地址，则说明到达目的地 */
                if ((icmp6->icmp6_type == ICMP6_ECHO_REPLY)) { /* 如果是 Echo Reply */
                    /* 比较来源地址与目标地址是否相同以判断是否到达目标 */
                    if (memcmp(&from_sa.sin6_addr, &dest_sa.sin6_addr, sizeof(struct in6_addr)) == 0) { /* 地址相同 */
                        /* 打印换行并打印本跳统计 */
                        if (replied_count > 0) { /* 如果本跳有回复 */
                            long avg;          /* 平均 RTT 变量 */
                            avg = rtt_sum / replied_count; /* 计算平均（整数） */
                            printf("  min/avg/max = %ld/%ld/%ld ms\n", rtt_min, avg, rtt_max); /* 打印统计 */
                        } else {
                            printf("\n");     /* 否则仅换行 */
                        }
                        close(g_sock);       /* 关闭原始套接字 */
                        return 0;            /* 成功退出（到达目标） */
                    }
                }

                /* 如果收到 Destination Unreachable 或 Time Exceeded，且来源地址为目标地址中的一种情况，也可能表示到达目标（例如目标返回不可达） */
                if ((icmp6->icmp6_type == ICMP6_DST_UNREACH) || (icmp6->icmp6_type == ICMP6_TIME_EXCEEDED)) { /* 检查类型 */
                    /* 如果来源地址就是目标地址，则我们到达了目的地（Destination Unreachable 的情形下） */
                    if (memcmp(&from_sa.sin6_addr, &dest_sa.sin6_addr, sizeof(struct in6_addr)) == 0) { /* 地址比较 */
                        if (replied_count > 0) { /* 打印本跳统计 */
                            long avg2;         /* 平均 RTT 变量 */
                            avg2 = rtt_sum / replied_count; /* 计算平均 */
                            printf("  min/avg/max = %ld/%ld/%ld ms\n", rtt_min, avg2, rtt_max); /* 打印统计 */
                        } else {
                            printf("\n");     /* 仅换行 */
                        }
                        close(g_sock);       /* 关闭套接字 */
                        return 0;            /* 成功退出 */
                    }
                }

                seq++;                     /* 自增序列号以区别下次发送 */
            } /* end else (rv > 0) */
        } /* end for probes */

        /* 在 probes 循环结束后打印本跳汇总（若有收到回复） */
        if (replied_count > 0) {         /* 若至少收到一次回复 */
            long avg_main;               /* 本跳平均 RTT */
            avg_main = rtt_sum / replied_count; /* 计算平均 RTT（整数） */
            printf("  min/avg/max = %ld/%ld/%ld ms\n", rtt_min, avg_main, rtt_max); /* 打印本跳统计 */
        } else {
            printf("\n");               /* 没有收到回复则换行 */
        }
    } /* end for hop */

    /* 如果走完所有 hop 仍未到达目标，关闭套接字并退出 */
    close(g_sock);                      /* 关闭原始套接字 */
    return 0;                            /* 正常结束 */
}
