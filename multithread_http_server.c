/* simple_http.c: 一个简单的 IPv4/IPv6 回环上监听的 HTTP/1.1 多线程服务器，C89 标准 */
#include <stdio.h>               /* 标准输入输出 */
#include <stdlib.h>              /* 标准库：malloc、free、exit */
#include <string.h>              /* 字符串操作 */
#include <unistd.h>              /* POSIX：close */
#include <errno.h>               /* errno */
#include <sys/types.h>           /* 套接字类型 */
#include <sys/socket.h>          /* 套接字函数 */
#include <netdb.h>               /* getaddrinfo */
#include <arpa/inet.h>           /* inet_ntop */
#include <netinet/in.h>          /* sockaddr_in, sockaddr_in6 */
#include <pthread.h>             /* pthreads */
#include <signal.h>              /* 信号处理 */

/* 全局变量：在程序退出时关闭这些监听套接字 */
static int listen_fd_v4 = -1;    /* IPv4 监听套接字 */
static int listen_fd_v6 = -1;    /* IPv6 监听套接字 */

/* 简单响应常量 */
static const char response[] = /* HTTP/1.1 200 响应及 Hello World 正文 */
    "HTTP/1.1 200 OK\r\n"
    "Content-Length: 11\r\n"
    "Content-Type: text/plain\r\n"
    "Connection: close\r\n"
    "\r\n"
    "Hello World";

/* 客户端处理线程函数参数结构 */
struct client_arg {                /* 参数结构 */
    int fd;                        /* 已接受的已连接套接字 */
    char addrstr[INET6_ADDRSTRLEN];/* 对端地址的文本形式 */
};

/* 关闭监听套接字并安全退出的信号处理函数（例如 SIGINT） */
static void handle_sigint(int signo) { /* 信号处理函数 */
    (void)signo;                   /* 避免未使用警告 */
    if (listen_fd_v4 >= 0) {       /* 若 IPv4 监听存在 */
        close(listen_fd_v4);       /* 关闭 IPv4 监听套接字 */
        listen_fd_v4 = -1;         /* 标记为已关闭 */
    }
    if (listen_fd_v6 >= 0) {       /* 若 IPv6 监听存在 */
        close(listen_fd_v6);       /* 关闭 IPv6 监听套接字 */
        listen_fd_v6 = -1;         /* 标记为已关闭 */
    }
    /* 直接正常退出进程 */
    _exit(0);                      /* 使用 _exit 避免在信号处理时复杂清理 */
}

/* 处理单个客户端连接的线程主函数 */
static void *client_thread(void *arg) { /* pthread 线程入口 */
    struct client_arg *carg = (struct client_arg *)arg; /* 强制转换参数 */
    int connfd = carg->fd;         /* 取出已连接套接字 */
    char buf[1024];                /* 用于接收请求的缓冲区 */
    ssize_t n;                     /* 接收返回值 */
    (void)buf;                     /* 避免未使用时的警告（下面会使用） */

    /* 首先试图读取请求（读取但不作严格解析），以便等待客户端发送 */
    n = recv(connfd, buf, sizeof(buf) - 1, 0); /* 从套接字读取数据 */
    if (n > 0) {                   /* 若读取到了数据 */
        buf[n] = '\0';             /* 终止字符串（安全） */
        /* 这里我们不解析 HTTP 请求，只打印一行简要日志 */
        /* 使用 fprintf 打印到 stderr，以免与客户端数据冲突 */
        fprintf(stderr, "Received request from %s: %.40s\n", carg->addrstr, buf); /* 打印 */
    } else if (n == 0) {           /* 对端关闭连接 */
        fprintf(stderr, "Client %s closed connection before sending data\n", carg->addrstr); /* 打印 */
    } else {                       /* 读取出错 */
        fprintf(stderr, "recv error from %s: %s\n", carg->addrstr, strerror(errno)); /* 打印 */
    }

    /* 发送固定的 HTTP/1.1 响应 */
    {                             /* 新的块用于在 C89 下声明变量 */
        const char *p = response; /* 指针指向响应 */
        size_t tosend = strlen(response); /* 计算要发送的字节数 */
        ssize_t sent_total = 0;   /* 已发送的累计字节数 */
        while ((size_t)sent_total < tosend) { /* 循环直至全部发送 */
            ssize_t s = send(connfd, p + sent_total, tosend - sent_total, 0); /* 发送 */
            if (s <= 0) {         /* 发送失败或连接被中断 */
                if (s < 0) {      /* 出错 */
                    fprintf(stderr, "send error to %s: %s\n", carg->addrstr, strerror(errno)); /* 打印 */
                }
                break;            /* 跳出发送循环 */
            }
            sent_total += s;     /* 累计已发送字节数 */
        }
    }

    /* 关闭已连接套接字并释放参数结构 */
    close(connfd);                /* 关闭连接套接字 */
    free(carg);                   /* 释放分配的参数结构 */
    return NULL;                  /* 线程返回 */
}

/* 为给定的文字地址和端口创建、绑定并监听套接字，返回套接字 fd 或 -1 */
static int make_and_bind(const char *host, const char *port, int v6only) { /* 创建并绑定 */
    struct addrinfo hints;        /* getaddrinfo 的 hints */
    struct addrinfo *res = NULL;  /* 结果链表指针 */
    struct addrinfo *rp;          /* 遍历用指针 */
    int sfd = -1;                 /* 返回的套接字 */
    int err;                      /* 临时错误码 */
    int reuse = 1;                /* SO_REUSEADDR 选项值 */

    memset(&hints, 0, sizeof(hints)); /* 清零 hints 结构 */
    hints.ai_family = AF_UNSPEC;  /* 同时支持 IPv4 和 IPv6 的解析 */
    hints.ai_socktype = SOCK_STREAM; /* TCP 流套接字 */
    hints.ai_flags = AI_NUMERICHOST; /* 要求 host 是数字地址，不做 DNS 查询 */

    err = getaddrinfo(host, port, &hints, &res); /* 解析地址字符串 */
    if (err != 0) {               /* 解析失败 */
        fprintf(stderr, "getaddrinfo(%s) failed: %s\n", host, gai_strerror(err)); /* 打印 */
        return -1;                /* 返回错误 */
    }

    /* 遍历所有候选地址并尝试创建和绑定套接字 */
    for (rp = res; rp != NULL; rp = rp->ai_next) { /* 遍历 */
        sfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol); /* 创建套接字 */
        if (sfd == -1) continue; /* 创建失败则尝试下一个地址 */

        /* 允许重用本地地址以便快速重启服务器 */
        if (setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, (void *)&reuse, sizeof(reuse)) < 0) { /* 设置 SO_REUSEADDR */
            /* 若设置失败，不是致命错误，但记录之 */
            fprintf(stderr, "setsockopt SO_REUSEADDR failed: %s\n", strerror(errno)); /* 打印 */
        }

        /* 如果是 IPv6，并且用户要求设置为仅 IPv6（防止 IPv4 映射地址） */
        if (rp->ai_family == AF_INET6 && v6only) { /* 检查 */
            int on = 1;           /* 选项值 */
            if (setsockopt(sfd, IPPROTO_IPV6, IPV6_V6ONLY, (void *)&on, sizeof(on)) < 0) { /* 设置 IPV6_V6ONLY */
                fprintf(stderr, "setsockopt IPV6_V6ONLY failed: %s\n", strerror(errno)); /* 打印 */
                /* 继续尝试（不立刻关闭） */
            }
        }

        /* 绑定套接字到指定地址 */
        if (bind(sfd, rp->ai_addr, rp->ai_addrlen) == 0) { /* 绑定成功 */
            /* 监听该套接字 */
            if (listen(sfd, 16) < 0) { /* 开始监听，backlog = 16 */
                fprintf(stderr, "listen failed: %s\n", strerror(errno)); /* 打印错误 */
                close(sfd);         /* 关闭套接字 */
                sfd = -1;           /* 标记失败 */
                continue;           /* 尝试下一个地址 */
            }
            /* 绑定并监听成功，退出循环 */
            break;                  /* 跳出 for 循环 */
        } else {                    /* 绑定失败 */
            fprintf(stderr, "bind(%s) failed: %s\n", host, strerror(errno)); /* 打印 */
            close(sfd);             /* 关闭套接字 */
            sfd = -1;               /* 标记失败 */
            continue;               /* 尝试下一个地址 */
        }
    }

    freeaddrinfo(res);            /* 释放 getaddrinfo 结果 */
    return sfd;                   /* 返回成功的套接字或 -1 */
}

/* 接受循环线程：对一个监听套接字不断 accept 并为每个连接创建处理线程 */
struct accept_arg {               /* 接受线程参数结构 */
    int listen_fd;                /* 监听套接字 */
};

static void *accept_loop(void *arg) { /* 接受循环线程入口 */
    struct accept_arg *aarg = (struct accept_arg *)arg; /* 参数 */
    int lfd = aarg->listen_fd;    /* 获取监听套接字 */
    int connfd;                   /* 已接受的连接套接字 */
    struct sockaddr_storage peer; /* 存放对端地址，支持 IPv4/IPv6 */
    socklen_t peerlen;            /* 地址长度 */
    char addrstr[INET6_ADDRSTRLEN]; /* 文本形式地址 */
    pthread_t tid;                /* 客户端线程 id */

    for (;;) {                    /* 永久循环，直到进程被信号终止 */
        peerlen = sizeof(peer);   /* 初始化长度 */
        connfd = accept(lfd, (struct sockaddr *)&peer, &peerlen); /* 接受连接 */
        if (connfd < 0) {        /* accept 出错 */
            if (errno == EINTR) continue; /* 被信号中断时重试 */
            fprintf(stderr, "accept error: %s\n", strerror(errno)); /* 打印并继续 */
            continue;            /* 继续接受下一个连接 */
        }

        /* 将对端地址转换为文本形式，便于日志 */
        if (peer.ss_family == AF_INET) { /* IPv4 */
            struct sockaddr_in *s4 = (struct sockaddr_in *)&peer; /* 强制转换 */
            inet_ntop(AF_INET, &s4->sin_addr, addrstr, sizeof(addrstr)); /* 转换文本 */
        } else {                /* IPv6 */
            struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)&peer; /* 强制转换 */
            inet_ntop(AF_INET6, &s6->sin6_addr, addrstr, sizeof(addrstr)); /* 转换文本 */
        }

        /* 为每个客户端分配参数结构（malloc）并填充 */
        {                       /* 新的块用于变量声明 */
            struct client_arg *carg = (struct client_arg *)malloc(sizeof(struct client_arg)); /* 分配 */
            if (carg == NULL) {  /* 分配失败 */
                fprintf(stderr, "malloc failed\n"); /* 打印 */
                close(connfd);   /* 关闭连接套接字以避免泄漏 */
                continue;        /* 继续接受下一个连接 */
            }
            carg->fd = connfd;  /* 填充连接套接字 */
            strncpy(carg->addrstr, addrstr, sizeof(carg->addrstr) - 1); /* 复制地址文本 */
            carg->addrstr[sizeof(carg->addrstr) - 1] = '\0'; /* 确保终止 */

            /* 创建线程处理客户端连接 */
            if (pthread_create(&tid, NULL, client_thread, (void *)carg) != 0) { /* 创建失败 */
                fprintf(stderr, "pthread_create failed\n"); /* 打印 */
                close(connfd);   /* 关闭连接套接字 */
                free(carg);      /* 释放参数结构 */
                continue;        /* 继续接受下一个连接 */
            }
            /* 分离该线程，避免需要 join */
            pthread_detach(tid); /* 分离线程资源 */
        }
    }
    return NULL;                  /* 不会返回 */
}

/* 主函数：创建两个监听套接字并启动对应的 accept 线程 */
int main(int argc, char *argv[]) { /* 主函数入口 */
    pthread_t acc_v4_thread;      /* IPv4 accept 线程 id */
    pthread_t acc_v6_thread;      /* IPv6 accept 线程 id */
    struct accept_arg *aarg_v4;   /* IPv4 accept 参数 */
    struct accept_arg *aarg_v6;   /* IPv6 accept 参数 */
    int rc;                       /* 临时返回码 */

    (void)argc;                   /* 避免未使用警告 */
    (void)argv;                   /* 避免未使用警告 */

    /* 安装 SIGINT 信号处理器以便 Ctrl-C 可以优雅关闭监听套接字 */
    signal(SIGINT, handle_sigint); /* 注册信号处理 */

    /* 创建并绑定 IPv6 回环地址 ::1:80，设置 v6only 为 1 */
    listen_fd_v6 = make_and_bind("::1", "80", 1); /* 创建 IPv6 监听 */
    if (listen_fd_v6 < 0) {       /* 若失败则打印并继续 */
        fprintf(stderr, "Failed to bind IPv6 ::1:80\n"); /* 打印 */
    }

    /* 创建并绑定 IPv4 回环地址 127.0.0.1:80 */
    listen_fd_v4 = make_and_bind("127.0.0.1", "80", 0); /* 创建 IPv4 监听 */
    if (listen_fd_v4 < 0) {       /* 若失败则打印并继续 */
        fprintf(stderr, "Failed to bind IPv4 127.0.0.1:80\n"); /* 打印 */
    }

    /* 检查至少有一个绑定成功，否则退出 */
    if (listen_fd_v4 < 0 && listen_fd_v6 < 0) { /* 如果两个都失败 */
        fprintf(stderr, "No sockets bound. Exiting.\n"); /* 打印 */
        exit(1);                 /* 退出并返回错误状态 */
    }

    /* 为 IPv4 接受线程准备参数并启动（如果已成功绑定） */
    if (listen_fd_v4 >= 0) {      /* 若 IPv4 监听存在 */
        aarg_v4 = (struct accept_arg *)malloc(sizeof(struct accept_arg)); /* 分配 */
        if (aarg_v4 == NULL) {    /* 分配失败 */
            fprintf(stderr, "malloc failed\n"); /* 打印 */
            close(listen_fd_v4);  /* 关闭监听套接字 */
            listen_fd_v4 = -1;    /* 标记为已关闭 */
        } else {                  /* 成功分配参数 */
            aarg_v4->listen_fd = listen_fd_v4; /* 填充 */
            rc = pthread_create(&acc_v4_thread, NULL, accept_loop, (void *)aarg_v4); /* 启动线程 */
            if (rc != 0) {        /* 创建失败 */
                fprintf(stderr, "pthread_create for IPv4 accept failed\n"); /* 打印 */
                close(listen_fd_v4); /* 关闭监听 */
                listen_fd_v4 = -1; /* 标记 */
                free(aarg_v4);    /* 释放参数 */
            }
        }
    }

    /* 为 IPv6 接受线程准备参数并启动（如果已成功绑定） */
    if (listen_fd_v6 >= 0) {      /* 若 IPv6 监听存在 */
        aarg_v6 = (struct accept_arg *)malloc(sizeof(struct accept_arg)); /* 分配 */
        if (aarg_v6 == NULL) {    /* 分配失败 */
            fprintf(stderr, "malloc failed\n"); /* 打印 */
            close(listen_fd_v6);  /* 关闭监听套接字 */
            listen_fd_v6 = -1;    /* 标记 */
        } else {                  /* 成功分配参数 */
            aarg_v6->listen_fd = listen_fd_v6; /* 填充 */
            rc = pthread_create(&acc_v6_thread, NULL, accept_loop, (void *)aarg_v6); /* 启动线程 */
            if (rc != 0) {        /* 创建失败 */
                fprintf(stderr, "pthread_create for IPv6 accept failed\n"); /* 打印 */
                close(listen_fd_v6); /* 关闭监听 */
                listen_fd_v6 = -1; /* 标记 */
                free(aarg_v6);    /* 释放参数 */
            }
        }
    }

    /* 主线程等待 accept 线程结束（实际上服务器将永久运行，直到 SIGINT） */
    if (listen_fd_v4 >= 0) {      /* 若 IPv4 线程存在 */
        pthread_join(acc_v4_thread, NULL); /* 等待 IPv4 accept 线程 */
    }
    if (listen_fd_v6 >= 0) {      /* 若 IPv6 线程存在 */
        pthread_join(acc_v6_thread, NULL); /* 等待 IPv6 accept 线程 */
    }

    /* 程序通常不会走到这里，但做清理以防万一 */
    if (listen_fd_v4 >= 0) close(listen_fd_v4); /* 关闭 IPv4 监听 */
    if (listen_fd_v6 >= 0) close(listen_fd_v6); /* 关闭 IPv6 监听 */
    return 0;                     /* 正常退出 */
}
