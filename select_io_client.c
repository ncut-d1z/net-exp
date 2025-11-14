#include <stdio.h>      /* printf */
#include <stdlib.h>     /* exit */
#include <string.h>     /* memset */
#include <unistd.h>     /* close */
#include <sys/types.h>  /* socket */
#include <sys/socket.h> /* socket */
#include <netinet/in.h> /* sockaddr_in */
#include <arpa/inet.h>  /* inet_addr */

#define TCP_PORT 80   /* TCP port */
#define UDP_PORT 53   /* UDP port */
#define BUF_SIZE 1024 /* buffer size */

int main()
{
    int tcp_fd;              /* TCP socket */
    int udp_fd;              /* UDP socket */
    struct sockaddr_in addr; /* server addr */
    char buf[BUF_SIZE];      /* buffer */
    int ret;                 /* return value */

    tcp_fd = socket(AF_INET, SOCK_STREAM, 0); /* create TCP socket */
    if (tcp_fd < 0)
    {
        perror("socket tcp");
        exit(1);
    }

    memset(&addr, 0, sizeof(addr));                /* clear */
    addr.sin_family = AF_INET;                     /* IPv4 */
    addr.sin_addr.s_addr = inet_addr("127.0.0.1"); /* server addr */
    addr.sin_port = htons(TCP_PORT);               /* port */

    if (connect(tcp_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("connect");
        exit(1);
    }

    strcpy(buf, "Hello via TCP");    /* message */
    write(tcp_fd, buf, strlen(buf)); /* send */

    memset(buf, 0, BUF_SIZE);              /* clear */
    ret = read(tcp_fd, buf, BUF_SIZE - 1); /* recv */
    if (ret > 0)
        printf("TCP echo: %s\n", buf); /* print */

    close(tcp_fd); /* close tcp */

    udp_fd = socket(AF_INET, SOCK_DGRAM, 0); /* create udp */
    if (udp_fd < 0)
    {
        perror("socket udp");
        exit(1);
    }

    memset(&addr, 0, sizeof(addr));                /* clear */
    addr.sin_family = AF_INET;                     /* IPv4 */
    addr.sin_addr.s_addr = inet_addr("127.0.0.1"); /* loopback */
    addr.sin_port = htons(UDP_PORT);               /* port */

    strcpy(buf, "Hello via UDP");                                                /* data */
    sendto(udp_fd, buf, strlen(buf), 0, (struct sockaddr *)&addr, sizeof(addr)); /* send */

    memset(buf, 0, BUF_SIZE);                                 /* clear */
    ret = recvfrom(udp_fd, buf, BUF_SIZE - 1, 0, NULL, NULL); /* receive */
    if (ret > 0)
        printf("UDP echo: %s\n", buf); /* print */

    close(udp_fd); /* close */

    return 0; /* end */
}
