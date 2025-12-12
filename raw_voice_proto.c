/* raw_voice_proto_fixed.c
 *
 * C89 style (fixed) version of raw_voice_proto.c
 *
 * Build:
 *   gcc -std=c89 -Wall -Wextra -pedantic -O2 -pthread -lm -o raw_voice_proto_fixed raw_voice_proto_fixed.c
 *
 * Run:
 *   sudo ./raw_voice_proto_fixed server <ifname> <server_ip>
 *   sudo ./raw_voice_proto_fixed client <server_ip> <client_id>
 *
 * Notes:
 * - Fixed: replaced inet_aton -> inet_pton, usleep -> nanosleep, marked unused params.
 * - Behavior: supports both server and client modes.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>   /* struct iphdr */
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <pthread.h>
#include <sys/time.h>
#include <stdarg.h>
#include <math.h>

/* -------- Configuration -------- */
#define CUSTOM_PROTO 255          /* custom protocol in IP header */
#define MAGIC 0xA1B2C3D4UL        /* private header magic */
#define FRAME_MS 20               /* generate one frame per 20ms */
#define PLAYBACK_DELAY_MS 60      /* receiver buffering target */
#define FRAME_BYTES 160           /* simulated audio bytes per frame */
#define MAX_PACKET_SIZE 1500
#define MAX_CLIENTS 64
#define HEARTBEAT_INTERVAL_S 10

/* -------- Types (C89-friendly) -------- */
typedef unsigned int u32;
typedef unsigned long u_long;

/* -------- Private header layout (C89-friendly) --------
   uint32_t magic;
   uint32_t client_id;
   uint32_t seq;
   uint32_t ts_sec;   // seconds part
   uint32_t ts_usec;  // microseconds part
   followed by audio bytes...
*/
#pragma pack(push,1)
struct priv_hdr {
    u32 magic;
    u32 client_id;
    u32 seq;
    u32 ts_sec;
    u32 ts_usec;
};
#pragma pack(pop)

#define PRIV_HDR_SIZE (4 + 4 + 4 + 4 + 4) /* 20 */

/* -------- Client list entry (server-side) -------- */
struct client_entry {
    int used;
    u32 client_id;
    struct sockaddr_in addr;
    u_long last_seen_ms;
};

/* -------- Globals -------- */
static int is_server = 0;
static char g_ifname[64];
static char g_server_ip_str[64];
static u32 g_client_id = 0;
static int raw_send_sock = -1; /* used for sending raw IP packets */
static pthread_mutex_t clients_lock = PTHREAD_MUTEX_INITIALIZER;
static struct client_entry clients[MAX_CLIENTS];

/* -------- Utility: get current time in ms (returns unsigned long) -------- */
static unsigned long now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (unsigned long)tv.tv_sec * 1000UL + (unsigned long)(tv.tv_usec / 1000);
}

/* -------- Utility: simple logging (C89-safe) -------- */
static void log_printf(const char *fmt, ...)
{
    va_list ap;
    unsigned long tms;
    tms = now_ms();
    printf("[%lu] ", tms);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

/* -------- IP checksum (for header) -------- */
static unsigned short ip_checksum(unsigned short *buf, int nwords)
{
    unsigned long sum = 0;
    int i;
    for (i = 0; i < nwords; i++) {
        sum += (unsigned long)buf[i];
    }
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return (unsigned short)(~sum);
}

/* -------- Build IP packet (header+payload) -------- */
static int build_ip_packet(unsigned char *buf,
                           const struct in_addr ip_src,
                           const struct in_addr ip_dst,
                           const unsigned char *payload, int payload_len)
{
    struct iphdr *ip;
    int iphdr_len;
    int total_len;
    ip = (struct iphdr *)buf;
    iphdr_len = sizeof(struct iphdr);
    total_len = iphdr_len + payload_len;

    memset(buf, 0, iphdr_len + payload_len);

    ip->ihl = iphdr_len / 4;
    ip->version = 4;
    ip->tos = 0;
    ip->tot_len = htons((unsigned short)total_len);
    ip->id = htons((unsigned short)(rand() & 0xFFFF));
    ip->frag_off = 0;
    ip->ttl = 64;
    ip->protocol = CUSTOM_PROTO;
    ip->check = 0;
    ip->saddr = ip_src.s_addr;
    ip->daddr = ip_dst.s_addr;

    /* copy payload */
    memcpy(buf + iphdr_len, payload, payload_len);

    /* compute checksum (in 16-bit words) */
    ip->check = ip_checksum((unsigned short *)ip, ip->ihl);

    return total_len;
}

/* -------- Parse incoming IP packet: validate protocol & extract payload --------
   Returns payload_len on success and sets *payload_ptr to start of priv header.
   Returns -1 on error.
*/
static int parse_ip_packet(unsigned char *buf, int buflen,
                           struct in_addr *src_addr,
                           unsigned char **payload_ptr, int *payload_len)
{
    struct iphdr *ip;
    int ihl;
    if (buflen < (int)sizeof(struct iphdr)) return -1;
    ip = (struct iphdr *)buf;
    if (ip->version != 4) return -1;
    if (ip->protocol != CUSTOM_PROTO) return -1;

    ihl = ip->ihl * 4;
    if (buflen < ihl) return -1;

    *src_addr = *(struct in_addr *)&ip->saddr;
    *payload_ptr = buf + ihl;
    *payload_len = buflen - ihl;
    return *payload_len;
}

/* -------- Gaussian noise generator (Box-Muller) -------- */
static int gaussian_pair_ready = 0;
static double gaussian_next = 0.0;

static double rand_gaussian(void)
{
    double u1, u2, s;
    if (gaussian_pair_ready) {
        gaussian_pair_ready = 0;
        return gaussian_next;
    }
    do {
        u1 = ((double)rand() + 1.0) / ((double)RAND_MAX + 2.0);
        u2 = ((double)rand() + 1.0) / ((double)RAND_MAX + 2.0);
        s = -2.0 * log(u1);
    } while (s == 0.0);
    gaussian_next = sqrt(s) * sin(2.0 * 3.14159265358979323846 * u2);
    gaussian_pair_ready = 1;
    return sqrt(s) * cos(2.0 * 3.14159265358979323846 * u2);
}

/* -------- Send raw packet via raw_send_sock (IP header included) -------- */
static ssize_t send_raw_packet(int sock, const unsigned char *packet, int pktlen,
                               struct sockaddr_in *dst)
{
    ssize_t sent;
    sent = sendto(sock, packet, pktlen, 0, (struct sockaddr *)dst, sizeof(struct sockaddr_in));
    return sent;
}

/* -------- Server: maintain client list -------- */
static void server_register_client(u32 client_id, struct sockaddr_in *addr)
{
    int i;
    pthread_mutex_lock(&clients_lock);
    for (i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].used && clients[i].client_id == client_id) {
            clients[i].addr = *addr;
            clients[i].last_seen_ms = now_ms();
            pthread_mutex_unlock(&clients_lock);
            return;
        }
    }
    for (i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].used) {
            clients[i].used = 1;
            clients[i].client_id = client_id;
            clients[i].addr = *addr;
            clients[i].last_seen_ms = now_ms();
            log_printf("Registered client id=%u addr=%s", client_id, inet_ntoa(addr->sin_addr));
            break;
        }
    }
    pthread_mutex_unlock(&clients_lock);
}

/* -------- Server: forward payload to all other clients -------- */
static void server_forward_payload(unsigned char *payload, int payload_len,
                                   const struct in_addr src_addr)
{
    unsigned char pktbuf[MAX_PACKET_SIZE];
    int i;
    struct in_addr server_src;
    if (inet_pton(AF_INET, g_server_ip_str, &server_src) != 1) {
        log_printf("server_forward_payload: invalid server ip %s", g_server_ip_str);
        return;
    }

    pthread_mutex_lock(&clients_lock);
    for (i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].used) continue;
        if (clients[i].addr.sin_addr.s_addr == src_addr.s_addr) continue;

        {
            int pktlen;
            struct sockaddr_in dst;
            pktlen = build_ip_packet(pktbuf, server_src, clients[i].addr.sin_addr,
                                     payload, payload_len);
            dst = clients[i].addr;
            dst.sin_port = 0;
            if (send_raw_packet(raw_send_sock, pktbuf, pktlen, &dst) < 0) {
                log_printf("Forward to %s failed: %s", inet_ntoa(clients[i].addr.sin_addr), strerror(errno));
            }
        }
    }
    pthread_mutex_unlock(&clients_lock);
}

/* -------- Client: sending thread arg -------- */
struct send_thread_arg {
    struct sockaddr_in server_addr;
    int recv_sock; /* not used by sender but kept for compatibility */
};

/* -------- Client: sending thread (uses nanosleep instead of usleep) -------- */
static void *client_send_thread(void *arg)
{
    struct send_thread_arg *sarg;
    unsigned int seq;
    unsigned char payload[PRIV_HDR_SIZE + FRAME_BYTES];
    unsigned char pktbuf[MAX_PACKET_SIZE];
    struct in_addr src_addr;
    int tmp;
    sarg = (struct send_thread_arg *)arg;
    seq = 0;

    /* determine local source IP by connecting a UDP socket to server (non-raw) */
    tmp = socket(AF_INET, SOCK_DGRAM, 0);
    if (tmp >= 0) {
        struct sockaddr_in serv;
        struct sockaddr_in local;
        socklen_t ln;
        memset(&serv, 0, sizeof(serv));
        serv.sin_family = AF_INET;
        serv.sin_addr = sarg->server_addr.sin_addr;
        serv.sin_port = htons(53);
        if (connect(tmp, (struct sockaddr *)&serv, sizeof(serv)) == 0) {
            ln = sizeof(local);
            if (getsockname(tmp, (struct sockaddr *)&local, &ln) == 0) {
                src_addr = local.sin_addr;
            } else {
                inet_pton(AF_INET, "0.0.0.0", &src_addr);
            }
        } else {
            inet_pton(AF_INET, "0.0.0.0", &src_addr);
        }
        close(tmp);
    } else {
        inet_pton(AF_INET, "0.0.0.0", &src_addr);
    }

    for (;;) {
        /* sleep FRAME_MS using nanosleep */
        {
            struct timespec rq;
            rq.tv_sec = FRAME_MS / 1000;
            rq.tv_nsec = (FRAME_MS % 1000) * 1000000L;
            nanosleep(&rq, NULL);
        }

        /* build private header */
        {
            struct priv_hdr ph;
            struct timeval tv;
            int i;
            gettimeofday(&tv, NULL);
            ph.magic = htonl((u32)MAGIC);
            ph.client_id = htonl((u32)g_client_id);
            ph.seq = htonl((u32)seq++);
            ph.ts_sec = htonl((u32)tv.tv_sec);
            ph.ts_usec = htonl((u32)tv.tv_usec);
            /* copy header */
            memcpy(payload, &ph, sizeof(ph));
            /* simulate audio bytes */
            for (i = 0; i < FRAME_BYTES; i++) {
                double g = rand_gaussian() * 10.0;
                int v = (int)(g);
                payload[PRIV_HDR_SIZE + i] = (unsigned char)(v & 0xFF);
            }
        }

        {
            int pktlen;
            pktlen = build_ip_packet(pktbuf, src_addr, sarg->server_addr.sin_addr,
                                     payload, PRIV_HDR_SIZE + FRAME_BYTES);
            if (send_raw_packet(raw_send_sock, pktbuf, pktlen, &sarg->server_addr) < 0) {
                log_printf("client send failed: %s", strerror(errno));
            }
        }
    }
    /* not reached */
    return NULL;
}

/* -------- Open raw sockets helper (returns recv_fd for recv; sets raw_send_sock for sending) --------
   For server: returns a recv socket bound to protocol CUSTOM_PROTO, sets raw_send_sock to IPPROTO_RAW send socket.
   For client: same behavior.
*/
static int open_raw_socket_and_bind(const char *ifname, int is_server_mode)
{
    /* Mark unused parameters to avoid warnings when not used */
    (void)ifname;
    (void)is_server_mode;

    int ssend;
    int srecv;
    int on;
    ssend = socket(AF_INET, SOCK_RAW, IPPROTO_RAW);
    if (ssend < 0) {
        log_printf("socket(AF_INET, SOCK_RAW, IPPROTO_RAW) failed: %s", strerror(errno));
        return -1;
    }
    on = 1;
    if (setsockopt(ssend, IPPROTO_IP, IP_HDRINCL, &on, sizeof(on)) < 0) {
        log_printf("setsockopt(IP_HDRINCL) failed: %s", strerror(errno));
        close(ssend);
        return -1;
    }

    srecv = socket(AF_INET, SOCK_RAW, CUSTOM_PROTO);
    if (srecv < 0) {
        log_printf("recv socket(AF_INET, SOCK_RAW, %d) failed: %s", CUSTOM_PROTO, strerror(errno));
        close(ssend);
        return -1;
    }

    /* Note: we intentionally skip SO_BINDTODEVICE / ioctl interface binding here
       to keep the code portable and avoid platform-dependent defines. */

    raw_send_sock = ssend;
    return srecv;
}

/* -------- Main -------- */
int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage:\n  %s server <ifname> <server_ip>\n  %s client <server_ip> <client_id>\n", argv[0], argv[0]);
        return 1;
    }
    srand((unsigned int)(time(NULL) ^ getpid()));

    if (strcmp(argv[1], "server") == 0) {
        int recv_fd;
        unsigned char rxbuf[4096];
        struct sockaddr_in src_sock;
        socklen_t slen;
        int r;
        struct in_addr pkt_src;
        unsigned char *payload;
        int payload_len;
        struct priv_hdr ph;
        int cid;

        if (argc < 4) {
            fprintf(stderr, "server usage: %s server <ifname> <server_ip>\n", argv[0]);
            return 1;
        }
        is_server = 1;
        strncpy(g_ifname, argv[2], sizeof(g_ifname)-1);
        g_ifname[sizeof(g_ifname)-1] = '\0';
        strncpy(g_server_ip_str, argv[3], sizeof(g_server_ip_str)-1);
        g_server_ip_str[sizeof(g_server_ip_str)-1] = '\0';

        recv_fd = open_raw_socket_and_bind(g_ifname, 1);
        if (recv_fd < 0) {
            log_printf("Server: failed to open raw sockets");
            return 1;
        }

        log_printf("Server started on interface=%s ip=%s", g_ifname, g_server_ip_str);

        for (;;) {
            slen = sizeof(src_sock);
            r = recvfrom(recv_fd, rxbuf, sizeof(rxbuf), 0, (struct sockaddr *)&src_sock, &slen);
            if (r <= 0) {
                log_printf("server recvfrom error: %s", strerror(errno));
                continue;
            }
            if (parse_ip_packet(rxbuf, r, &pkt_src, &payload, &payload_len) < 0) continue;
            if (payload_len < (int)sizeof(struct priv_hdr)) continue;
            memcpy(&ph, payload, sizeof(ph));
            if (ntohl((u32)ph.magic) != MAGIC) continue;
            cid = ntohl(ph.client_id);

            /* register client */
            {
                struct sockaddr_in client_addr;
                memset(&client_addr, 0, sizeof(client_addr));
                client_addr.sin_family = AF_INET;
                client_addr.sin_addr = pkt_src;
                server_register_client((u32)cid, &client_addr);
            }

            /* forward payload (private hdr + audio) to other clients */
            server_forward_payload(payload, payload_len, pkt_src);
        }

    } else if (strcmp(argv[1], "client") == 0) {
        int recv_fd;
        struct sockaddr_in server_addr;
        pthread_t send_tid;
        struct send_thread_arg sarg;
        unsigned char rxbuf[2048];
        struct sockaddr_in src_sock;
        socklen_t slen;
        int r;
        struct in_addr pkt_src;
        unsigned char *payload;
        int payload_len;
        struct priv_hdr ph;
        u32 sender_id;
        u32 seq;
        unsigned long ts_ms;
        u32 secs;
        u32 usec;

        if (argc < 4) {
            fprintf(stderr, "client usage: %s client <server_ip> <client_id>\n", argv[0]);
            return 1;
        }
        strncpy(g_server_ip_str, argv[2], sizeof(g_server_ip_str)-1);
        g_server_ip_str[sizeof(g_server_ip_str)-1] = '\0';
        g_client_id = (u32)atoi(argv[3]);

        recv_fd = open_raw_socket_and_bind(NULL, 0);
        if (recv_fd < 0) {
            log_printf("Client: failed to open raw sockets");
            return 1;
        }

        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        if (inet_pton(AF_INET, g_server_ip_str, &server_addr.sin_addr) != 1) {
            log_printf("Invalid server ip %s", g_server_ip_str);
            return 1;
        }

        sarg.server_addr = server_addr;
        sarg.recv_sock = recv_fd;

        if (pthread_create(&send_tid, NULL, client_send_thread, &sarg) != 0) {
            log_printf("pthread_create send failed");
            return 1;
        }
        pthread_detach(send_tid);

        for (;;) {
            slen = sizeof(src_sock);
            r = recvfrom(recv_fd, rxbuf, sizeof(rxbuf), 0, (struct sockaddr *)&src_sock, &slen);
            if (r <= 0) {
                log_printf("client recvfrom error: %s", strerror(errno));
                continue;
            }
            if (parse_ip_packet(rxbuf, r, &pkt_src, &payload, &payload_len) < 0) continue;
            if (payload_len < (int)sizeof(struct priv_hdr)) continue;
            memcpy(&ph, payload, sizeof(ph));
            if (ntohl((u32)ph.magic) != MAGIC) continue;

            sender_id = ntohl(ph.client_id);
            seq = ntohl(ph.seq);
            secs = ntohl(ph.ts_sec);
            usec = ntohl(ph.ts_usec);
            ts_ms = (unsigned long)secs * 1000UL + (unsigned long)(usec / 1000);
            if ((seq % 50) == 0) {
                unsigned long delay = now_ms() - ts_ms;
                log_printf("RX from %u seq=%u delay=%lu ms", (unsigned int)sender_id, (unsigned int)seq, delay);
            }
            /* In prototype: "play" is a no-op (drop) or log */
        }
    } else {
        fprintf(stderr, "Unknown mode: %s\n", argv[1]);
        return 1;
    }

    return 0;
}
