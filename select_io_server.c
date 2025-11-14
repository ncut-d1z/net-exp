
#include <stdio.h>      /* printf */
#include <stdlib.h>     /* exit */
#include <string.h>     /* memset */
#include <unistd.h>     /* close */
#include <sys/types.h>  /* socket types */
#include <sys/socket.h> /* socket */
#include <netinet/in.h> /* sockaddr_in */
#include <arpa/inet.h>  /* inet_addr */

#define TCP_PORT 80   /* TCP server port */
#define UDP_PORT 53   /* UDP server port */
#define BUF_SIZE 1024 /* buffer size */

int main()
{
    int tcp_fd;                 /* TCP listening socket */
    int udp_fd;                 /* UDP socket */
    int maxfd;                  /* max fd for select */
    fd_set rfds;                /* read fd set */
    struct sockaddr_in addr;    /* generic address */
    struct sockaddr_in cliaddr; /* client address */
    socklen_t cliaddr_len;      /* length of client address */
    int conn_fd;                /* TCP connection fd */
    int ret;                    /* return value */
    char buf[BUF_SIZE];         /* buffer */

    tcp_fd = socket(AF_INET, SOCK_STREAM, 0); /* create TCP socket */
    if (tcp_fd < 0)
    {
        perror("socket tcp");
        exit(1);
    }

    memset(&addr, 0, sizeof(addr));                /* clear address */
    addr.sin_family = AF_INET;                     /* IPv4 */
    addr.sin_addr.s_addr = inet_addr("127.0.0.1"); /* loopback */
    addr.sin_port = htons(TCP_PORT);               /* port 80 */

    if (bind(tcp_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("bind tcp");
        exit(1);
    }

    if (listen(tcp_fd, 5) < 0)
    {
        perror("listen");
        exit(1);
    }

    udp_fd = socket(AF_INET, SOCK_DGRAM, 0); /* create UDP socket */
    if (udp_fd < 0)
    {
        perror("socket udp");
        exit(1);
    }

    memset(&addr, 0, sizeof(addr));                /* reinit for UDP */
    addr.sin_family = AF_INET;                     /* IPv4 */
    addr.sin_addr.s_addr = inet_addr("127.0.0.1"); /* loopback */
    addr.sin_port = htons(UDP_PORT);               /* port 53 */

    if (bind(udp_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("bind udp");
        exit(1);
    }

    printf("Server started: TCP on 127.0.0.1:80, UDP on 127.0.0.1:53\n");

    for (;;)
    {                          /* infinite loop */
        FD_ZERO(&rfds);        /* clear fd set */
        FD_SET(tcp_fd, &rfds); /* watch TCP listen */
        FD_SET(udp_fd, &rfds); /* watch UDP */

        maxfd = tcp_fd > udp_fd ? tcp_fd : udp_fd; /* find max for select */

        ret = select(maxfd + 1, &rfds, NULL, NULL, NULL); /* blocking select */
        if (ret < 0)
        {
            perror("select");
            continue;
        }

        if (FD_ISSET(tcp_fd, &rfds))
        {                                                                        /* new TCP connection */
            cliaddr_len = sizeof(cliaddr);                                       /* length */
            conn_fd = accept(tcp_fd, (struct sockaddr *)&cliaddr, &cliaddr_len); /* accept */
            if (conn_fd < 0)
            {
                perror("accept");
                continue;
            }

            memset(buf, 0, BUF_SIZE);               /* clear buffer */
            ret = read(conn_fd, buf, BUF_SIZE - 1); /* read */
            if (ret > 0)
            {
                printf("TCP received: %s\n", buf); /* print */
                write(conn_fd, buf, ret);          /* echo back */
            }
            close(conn_fd); /* close connection */
        }

        if (FD_ISSET(udp_fd, &rfds))
        {                                                                                            /* UDP message */
            cliaddr_len = sizeof(cliaddr);                                                           /* length */
            memset(buf, 0, BUF_SIZE);                                                                /* clear */
            ret = recvfrom(udp_fd, buf, BUF_SIZE - 1, 0, (struct sockaddr *)&cliaddr, &cliaddr_len); /* receive */
            if (ret > 0)
            {
                printf("UDP received: %s\n", buf);                                     /* print */
                sendto(udp_fd, buf, ret, 0, (struct sockaddr *)&cliaddr, cliaddr_len); /* echo */
            }
        }
    }
    return 0; /* never reached */
}
