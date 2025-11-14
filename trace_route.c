#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip6.h>
#include <netinet/icmp6.h>
#include <arpa/inet.h>
#include <netdb.h>

#define MAX_HOPS 30
#define PACKET_SIZE 64
#define TIMEOUT 3
#define PORT 33434

struct packet_data {
    struct timeval tv;
    int ttl;
};

unsigned short checksum(void *b, int len);
int create_icmp6_socket(void);
int create_udp_socket(void);
void trace_route(const char *hostname);
void print_icmp6_type(int type);

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <hostname>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    trace_route(argv[1]);
    return 0;
}

unsigned short checksum(void *b, int len) {
    unsigned short *buf;
    unsigned int sum;
    unsigned short result;

    buf = (unsigned short*) b;
    sum = 0;

    for (sum = 0; len > 1; len -= 2) {
        sum += *buf++;
    }

    if (len == 1) {
        sum += *(unsigned char *)buf;
    }

    sum = (sum >> 16) + (sum & 0xFFFF);
    sum += (sum >> 16);
    result = ~sum;

    return result;
}

int create_icmp6_socket(void) {
    int sock;
    int on;

    on = 1;
    sock = socket(AF_INET6, SOCK_RAW, IPPROTO_ICMPV6);
    if (sock < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    if (setsockopt(sock, IPPROTO_IPV6, IPV6_RECVHOPLIMIT, &on, sizeof(on)) < 0) {
        perror("setsockopt IPV6_RECVHOPLIMIT");
        close(sock);
        exit(EXIT_FAILURE);
    }

    return sock;
}

int create_udp_socket(void) {
    int sock;

    sock = socket(AF_INET6, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    return sock;
}

void print_icmp6_type(int type) {
    switch (type) {
        case ICMP6_TIME_EXCEEDED:
            printf(" Time exceeded");
            break;
        case ICMP6_DST_UNREACH:
            printf(" Destination unreachable");
            break;
        case ICMP6_ECHO_REPLY:
            printf(" Echo reply");
            break;
        case ICMP6_PACKET_TOO_BIG:
            printf(" Packet too big");
            break;
        case ICMP6_PARAM_PROB:
            printf(" Parameter problem");
            break;
        default:
            printf(" Type %d", type);
            break;
    }
}

void trace_route(const char *hostname) {
    struct addrinfo hints;
    struct addrinfo *result;
    struct sockaddr_in6 *target_addr;
    int icmp_sock, udp_sock;
    int ttl;
    int i;
    struct timeval tv;
    char packet[PACKET_SIZE];
    char recv_buf[1024];
    struct sockaddr_in6 from_addr;
    socklen_t addr_len;
    struct icmp6_hdr *icmp_hdr;
    struct ip6_hdr *ip6_hdr;
    int ret;
    int hops_found;
    char addr_str[INET6_ADDRSTRLEN];
    struct msghdr msg;
    struct iovec iov;
    char cmsg_buf[1024];
    struct cmsghdr *cmsg;
    int hoplimit;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET6;
    hints.ai_socktype = SOCK_DGRAM;

    if (getaddrinfo(hostname, NULL, &hints, &result) != 0) {
        fprintf(stderr, "Cannot resolve hostname: %s\n", hostname);
        exit(EXIT_FAILURE);
    }

    target_addr = (struct sockaddr_in6 *)result->ai_addr;

    printf("traceroute to %s (%s), %d hops max, %d byte packets\n",
           hostname,
           inet_ntop(AF_INET6, &target_addr->sin6_addr, addr_str, sizeof(addr_str)),
           MAX_HOPS, PACKET_SIZE);

    icmp_sock = create_icmp6_socket();
    udp_sock = create_udp_socket();

    hops_found = 0;

    for (ttl = 1; ttl <= MAX_HOPS && !hops_found; ttl++) {
        printf("%2d ", ttl);
        fflush(stdout);

        if (setsockopt(udp_sock, IPPROTO_IPV6, IPV6_UNICAST_HOPS, &ttl, sizeof(ttl)) < 0) {
            perror("setsockopt IPV6_UNICAST_HOPS");
            continue;
        }

        for (i = 0; i < 3; i++) {
            struct timeval start, end;
            double elapsed;
            int timed_out;
            fd_set readfds;

            memset(packet, 0, sizeof(packet));

            gettimeofday(&start, NULL);

            if (sendto(udp_sock, packet, sizeof(packet), 0,
                      (struct sockaddr *)target_addr, sizeof(*target_addr)) < 0) {
                perror("sendto");
                printf("* ");
                continue;
            }

            timed_out = 0;
            FD_ZERO(&readfds);
            FD_SET(icmp_sock, &readfds);

            tv.tv_sec = TIMEOUT;
            tv.tv_usec = 0;

            ret = select(icmp_sock + 1, &readfds, NULL, NULL, &tv);

            if (ret == 0) {
                printf("* ");
                continue;
            } else if (ret < 0) {
                perror("select");
                printf("* ");
                continue;
            }

            iov.iov_base = recv_buf;
            iov.iov_len = sizeof(recv_buf);

            memset(&msg, 0, sizeof(msg));
            msg.msg_name = &from_addr;
            msg.msg_namelen = sizeof(from_addr);
            msg.msg_iov = &iov;
            msg.msg_iovlen = 1;
            msg.msg_control = cmsg_buf;
            msg.msg_controllen = sizeof(cmsg_buf);

            ret = recvmsg(icmp_sock, &msg, 0);
            if (ret < 0) {
                perror("recvmsg");
                printf("* ");
                continue;
            }

            hoplimit = -1;
            for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
                if (cmsg->cmsg_level == IPPROTO_IPV6 && cmsg->cmsg_type == IPV6_HOPLIMIT) {
                    memcpy(&hoplimit, CMSG_DATA(cmsg), sizeof(hoplimit));
                    break;
                }
            }

            gettimeofday(&end, NULL);
            elapsed = (end.tv_sec - start.tv_sec) * 1000.0 +
                     (end.tv_usec - start.tv_usec) / 1000.0;

            ip6_hdr = (struct ip6_hdr *)recv_buf;
            icmp_hdr = (struct icmp6_hdr *)(recv_buf + sizeof(struct ip6_hdr));

            inet_ntop(AF_INET6, &from_addr.sin6_addr, addr_str, sizeof(addr_str));

            printf("%s %.3f ms", addr_str, elapsed);
            print_icmp6_type(icmp_hdr->icmp6_type);
            printf(" ");

            if (icmp_hdr->icmp6_type == ICMP6_DST_UNREACH ||
                icmp_hdr->icmp6_type == ICMP6_ECHO_REPLY) {
                hops_found = 1;
            }
        }
        printf("\n");
    }

    close(icmp_sock);
    close(udp_sock);
    freeaddrinfo(result);
}
