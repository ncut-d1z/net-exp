/* raw_voice_proto.c
 *
 * C89 style, Linux RAW-socket based voice prototype (server/client).
 *
 * Build:
 *   gcc -std=c89 -Wall -O2 -pthread -o raw_voice_proto raw_voice_proto.c
 *
 * Usage (examples):
 *   Server: ./raw_voice_proto server <ifname> <server_ip>
 *     e.g.: sudo ./raw_voice_proto server eth0 192.168.1.10
 *
 *   Client: ./raw_voice_proto client <server_ip> <client_id>
 *     e.g.: sudo ./raw_voice_proto client 192.168.1.10 42
 *
 * Notes:
 *   - Program uses RAW IPv4 packets with protocol field = 255 (custom).
 *   - Each payload = PRIV_HDR + audio_bytes. Audio bytes are simulated
 *     Gaussian noise generated every FRAME_MS.
 *   - Minimal error handling/logging printed to stdout.
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

/* -------- Configuration -------- */
#define CUSTOM_PROTO 255          /* custom protocol in IP header */
#define MAGIC 0xA1B2C3D4UL        /* private header magic */
#define FRAME_MS 20               /* generate one frame per 20ms */
#define PLAYBACK_DELAY_MS 60      /* receiver buffering target */
#define FRAME_BYTES 160           /* simulated audio bytes per frame */
#define PRIV_HDR_SIZE (4 + 4 + 4 + 8) /* magic(4) client_id(4) seq(4) ts_ms(8) */
#define MAX_PACKET_SIZE 1500
#define MAX_CLIENTS 64
#define HEARTBEAT_INTERVAL_S 10

/* -------- Private header layout --------
   uint32_t magic;
   uint32_t client_id;
   uint32_t seq;
   uint64_t ts_ms;
   followed by audio bytes...
*/
typedef unsigned int u32;
typedef unsigned long long u64;

#pragma pack(push,1)
struct priv_hdr {
    u32 magic;
    u32 client_id;
    u32 seq;
    u64 ts_ms;
};
#pragma pack(pop)

/* -------- Client list entry (server-side) -------- */
struct client_entry {
    int used;
    u32 client_id;
    struct sockaddr_in addr;
    u64 last_seen_ms;
};

/* -------- Globals -------- */
static int is_server = 0;
static char g_ifname[64];
static char g_server_ip_str[64];
static u32 g_client_id = 0;
static int raw_sock = -1;
static struct client_entry clients[MAX_CLIENTS];
static pthread_mutex_t clients_lock = PTHREAD_MUTEX_INITIALIZER;

/* -------- Utility: get current time in ms -------- */
static u64 now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (u64)tv.tv_sec * 1000ULL + (u64)(tv.tv_usec / 1000);
}

/* -------- Utility: simple logging -------- */
static void log_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("[%llu] ", (unsigned long long) now_ms());
    vprintf(fmt, ap);
    printf("\n");
    fflush(stdout);
    va_end(ap);
}

/* -------- IP checksum (for header) --------
   Standard RFC 791 checksum for IP header.
*/
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

/* -------- Build IP packet (header+payload) --------
   ip_src and ip_dst are in network byte order (struct in_addr).
   payload_len <= MAX_PACKET_SIZE - sizeof(struct iphdr)
*/
static int build_ip_packet(unsigned char *buf,
                           const struct in_addr ip_src,
                           const struct in_addr ip_dst,
                           const unsigned char *payload, int payload_len)
{
    struct iphdr *ip = (struct iphdr *)buf;
    int iphdr_len = sizeof(struct iphdr);
    int total_len = iphdr_len + payload_len;

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

    /* compute checksum */
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
    if (buflen < (int)sizeof(struct iphdr)) return -1;
    struct iphdr *ip = (struct iphdr *)buf;
    if (ip->version != 4) return -1;
    if (ip->protocol != CUSTOM_PROTO) return -1;

    /* compute ip header length in bytes */
    int ihl = ip->ihl * 4;
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

/* -------- Send raw packet via raw_sock (IP header included) -------- */
static ssize_t send_raw_packet(int sock, const unsigned char *packet, int pktlen,
                               struct sockaddr_in *dst)
{
    ssize_t sent = sendto(sock, packet, pktlen, 0,
                          (struct sockaddr *)dst, sizeof(struct sockaddr_in));
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
    /* insert new */
    for (i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].used) {
            clients[i].used = 1;
            clients[i].client_id = client_id;
            clients[i].addr = *addr;
            clients[i].last_seen_ms = now_ms();
            log_printf("Registered client id=%u addr=%s:%d", client_id,
                       inet_ntoa(addr->sin_addr), ntohs(addr->sin_port));
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
    /* set server source as g_server_ip_str */
    if (inet_aton(g_server_ip_str, &server_src) == 0) {
        log_printf("server_forward_payload: invalid server ip %s", g_server_ip_str);
        return;
    }

    pthread_mutex_lock(&clients_lock);
    for (i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].used) continue;
        /* don't send back to original sender (compare addresses) */
        if (clients[i].addr.sin_addr.s_addr == src_addr.s_addr) continue;

        /* build ip packet with server as src and target client as dst */
        int pktlen = build_ip_packet(pktbuf, server_src, clients[i].addr.sin_addr,
                                     payload, payload_len);
        struct sockaddr_in dst = clients[i].addr;
        dst.sin_port = 0; /* raw send ignores port */
        if (send_raw_packet(raw_sock, pktbuf, pktlen, &dst) < 0) {
            log_printf("Forward to %s failed: %s",
                       inet_ntoa(clients[i].addr.sin_addr), strerror(errno));
        }
    }
    pthread_mutex_unlock(&clients_lock);
}

/* -------- Server main loop thread -------- */
static void *server_recv_thread(void *arg)
{
    (void)arg;
    unsigned char rxbuf[2048];
    while (1) {
        struct sockaddr_in src_sock;
        socklen_t slen = sizeof(src_sock);
        ssize_t r = recvfrom(raw_sock, rxbuf, sizeof(rxbuf), 0,
                             (struct sockaddr *)&src_sock, &slen);
        if (r <= 0) {
            log_printf("recvfrom raw_sock error: %s", strerror(errno));
            continue;
        }
        struct in_addr pkt_src;
        unsigned char *payload;
        int payload_len;
        if (parse_ip_packet(rxbuf, (int)r, &pkt_src, &payload, &payload_len) < 0) {
            /* not our custom proto or malformed */
            continue;
        }
        /* check private header */
        if (payload_len < (int)sizeof(struct priv_hdr)) continue;
        struct priv_hdr ph;
        memcpy(&ph, payload, sizeof(ph));
        if (ntohl((u32)ph.magic) != MAGIC) continue;

        ph.client_id = ntohl(ph.client_id);
        ph.seq = ntohl(ph.seq);
        /* ts is network-order 64-bit (we stored as host order; convert manually) */
        {
            u64 be_ts = 0;
            unsigned char *p = (unsigned char *)&ph.ts_ms;
            int i;
            /* reconstruct big-endian u64 */
            for (i = 0; i < 8; i++) {
                be_ts = (be_ts << 8) | (unsigned long long)(p[i]);
            }
            /* convert to host if needed — for simplicity assume network order inserted same way */
            /* In this code we don't strictly rely on ph.ts_ms value besides logging */
        }

        /* register client; source address is pkt_src */
        struct sockaddr_in client_addr;
        memset(&client_addr, 0, sizeof(client_addr));
        client_addr.sin_family = AF_INET;
        client_addr.sin_addr = pkt_src;
        server_register_client(ntohl(ph.client_id), &client_addr);

        /* forward payload (private hdr + audio) to other clients */
        server_forward_payload(payload, payload_len, pkt_src);
    }
    return NULL;
}

/* -------- Client: sending thread -------- */
struct send_thread_arg {
    struct sockaddr_in server_addr;
};

static void *client_send_thread(void *arg)
{
    struct send_thread_arg *sarg = (struct send_thread_arg *)arg;
    unsigned int seq = 0;
    unsigned char payload[PRIV_HDR_SIZE + FRAME_BYTES];
    unsigned char pktbuf[MAX_PACKET_SIZE];
    struct in_addr src_addr;
    /* determine local source IP by connecting a UDP socket to server (non-raw) */
    {
        int tmp = socket(AF_INET, SOCK_DGRAM, 0);
        if (tmp >= 0) {
            struct sockaddr_in serv = sarg->server_addr;
            serv.sin_port = htons(53);
            if (connect(tmp, (struct sockaddr *)&serv, sizeof(serv)) == 0) {
                struct sockaddr_in local;
                socklen_t ln = sizeof(local);
                if (getsockname(tmp, (struct sockaddr *)&local, &ln) == 0) {
                    src_addr = local.sin_addr;
                } else {
                    inet_aton("0.0.0.0", &src_addr);
                }
            } else {
                inet_aton("0.0.0.0", &src_addr);
            }
            close(tmp);
        } else {
            inet_aton("0.0.0.0", &src_addr);
        }
    }

    while (1) {
        /* clock-driven generation: sleep FRAME_MS */
        usleep(FRAME_MS * 1000);

        /* build private header (network byte order) */
        struct priv_hdr ph;
        ph.magic = htonl((u32)MAGIC);
        ph.client_id = htonl((u32)g_client_id);
        ph.seq = htonl((u32)seq++);
        {
            u64 ts = now_ms();
            /* store ts in network byte order (big-endian) */
            unsigned char *p = (unsigned char *)&ph.ts_ms;
            int i;
            for (i = 0; i < 8; i++) {
                p[7 - i] = (unsigned char)(ts & 0xFF);
                ts >>= 8;
            }
        }

        /* simulate audio bytes (Gaussian scaled to int8) */
        {
            int i;
            for (i = 0; i < FRAME_BYTES; i++) {
                double g = rand_gaussian() * 10.0; /* scale down amplitude */
                int v = (int)(g);
                payload[PRIV_HDR_SIZE + i] = (unsigned char)(v & 0xFF);
            }
        }
        /* copy header + payload */
        memcpy(payload, &ph, sizeof(ph));

        /* build full ip packet where dst is server */
        int pktlen = build_ip_packet(pktbuf, src_addr, sarg->server_addr.sin_addr,
                                     payload, PRIV_HDR_SIZE + FRAME_BYTES);

        /* send */
        if (send_raw_packet(raw_sock, pktbuf, pktlen, &sarg->server_addr) < 0) {
            log_printf("client send failed: %s", strerror(errno));
        } else {
            /* optionally log minimal info */
            /* log_printf("sent seq=%u to server", seq-1); */
        }
    }
    return NULL;
}

/* -------- Client: receive thread (collect & "play") -------- */
static void *client_recv_thread(void *arg)
{
    (void)arg;
    unsigned char rxbuf[2048];
    while (1) {
        struct sockaddr_in src_sock;
        socklen_t slen = sizeof(src_sock);
        ssize_t r = recvfrom(raw_sock, rxbuf, sizeof(rxbuf), 0,
                             (struct sockaddr *)&src_sock, &slen);
        if (r <= 0) {
            log_printf("client recv error: %s", strerror(errno));
            continue;
        }
        struct in_addr pkt_src;
        unsigned char *payload;
        int payload_len;
        if (parse_ip_packet(rxbuf, (int)r, &pkt_src, &payload, &payload_len) < 0) {
            continue;
        }
        if (payload_len < (int)sizeof(struct priv_hdr)) continue;
        struct priv_hdr ph;
        memcpy(&ph, payload, sizeof(ph));
        if (ntohl((u32)ph.magic) != MAGIC) continue;

        u32 sender_id = ntohl(ph.client_id);
        u32 seq = ntohl(ph.seq);
        /* parse timestamp (big endian) */
        {
            u64 ts = 0;
            unsigned char *p = (unsigned char *)&ph.ts_ms;
            int i;
            for (i = 0; i < 8; i++) {
                ts = (ts << 8) | (unsigned long long)p[i];
            }
            u64 delay = now_ms() - ts;
            /* log occasionally */
            if ((seq % 50) == 0) {
                log_printf("rx from id=%u seq=%u delay=%llu ms payload=%d",
                           sender_id, seq, (unsigned long long)delay, payload_len - sizeof(ph));
            }
        }

        /* Simulate "play": we simply drop or write to log (no audio device used). */
        /* In a full implementation we'd insert into jitter buffer keyed by seq/timestamp. */
    }
    return NULL;
}

/* -------- Helper: open raw socket and set IP_HDRINCL, bind to interface if server -------- */
static int open_raw_socket_and_bind(const char *ifname, int is_server_mode)
{
    int s = socket(AF_INET, SOCK_RAW, IPPROTO_RAW);
    if (s < 0) {
        log_printf("socket(AF_INET, SOCK_RAW) failed: %s", strerror(errno));
        return -1;
    }
    /* set IP_HDRINCL so we provide our own IP header */
    {
        int on = 1;
        if (setsockopt(s, IPPROTO_IP, IP_HDRINCL, &on, sizeof(on)) < 0) {
            log_printf("setsockopt(IP_HDRINCL) failed: %s", strerror(errno));
            close(s);
            return -1;
        }
    }
    /* If server, open a separate raw socket for receiving (because IPPROTO_RAW cannot receive).
       We'll close s and instead open a recv socket bound to protocol CUSTOM_PROTO.
    */
    if (is_server_mode) {
        close(s);
        s = socket(AF_INET, SOCK_RAW, CUSTOM_PROTO);
        if (s < 0) {
            log_printf("server recv socket(AF_INET, SOCK_RAW, %d) failed: %s",
                       CUSTOM_PROTO, strerror(errno));
            return -1;
        }
        /* optionally bind to interface */
        if (ifname && ifname[0]) {
            struct ifreq ifr;
            memset(&ifr, 0, sizeof(ifr));
            strncpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name)-1);
            if (setsockopt(s, SOL_SOCKET, SO_BINDTODEVICE, (void *)&ifr, sizeof(ifr)) < 0) {
                /* try ioctl approach */
                int fd = socket(AF_INET, SOCK_DGRAM, 0);
                if (fd >= 0) {
                    if (ioctl(fd, SIOCGIFADDR, &ifr) == 0) {
                        /* no-op, we only wanted to ensure interface exists */
                    }
                    close(fd);
                }
                /* if setsockopt fails, we continue; binding to device is optional */
            }
        }
        /* server also needs a raw send socket with IP_HDRINCL for forwarding */
        /* open send socket */
        {
            int ssend = socket(AF_INET, SOCK_RAW, IPPROTO_RAW);
            if (ssend < 0) {
                log_printf("server send socket open failed: %s", strerror(errno));
                close(s);
                return -1;
            }
            {
                int on = 1;
                setsockopt(ssend, IPPROTO_IP, IP_HDRINCL, &on, sizeof(on));
            }
            /* we will use two descriptors: one recv (s) and reuse ssend for raw sending */
            /* to simplify, set global raw_sock to ssend and create a duplicate for recv in select */
            raw_sock = ssend;
            /* return the recv socket in caller via separate variable? For brevity, we set global recv socket via another global */
            /* But to keep functions simple, we'll dup s into a separate fd stored in raw_sock+1? Simpler: close ssend and use s for both send/recv is tricky. */
            /* Simpler approach: return ssend and keep s in a static variable accessible via a hack — but for clarity, we instead create a process where recv uses a different local socket in server thread directly. */
            /* We'll store recv fd inside a heap and pass via thread arg in main. */
            /* For current function, return s (recv fd) and leave raw_sock as send socket. */
            return s; /* s is recv fd, raw_sock has been set to ssend */
        }
    } else {
        /* client: we want a single raw socket for send and recv of our custom proto.
           Create a socket for sending with IP_HDRINCL and another socket for receiving custom proto.
        */
        close(s);
        int ssend = socket(AF_INET, SOCK_RAW, IPPROTO_RAW);
        if (ssend < 0) {
            log_printf("client send socket open failed: %s", strerror(errno));
            return -1;
        }
        {
            int on = 1;
            setsockopt(ssend, IPPROTO_IP, IP_HDRINCL, &on, sizeof(on));
        }
        /* receive socket for CUSTOM_PROTO */
        int srecv = socket(AF_INET, SOCK_RAW, CUSTOM_PROTO);
        if (srecv < 0) {
            log_printf("client recv socket open failed: %s", strerror(errno));
            close(ssend);
            return -1;
        }
        raw_sock = ssend; /* use raw_sock for sending; return receive fd */
        return srecv;
    }
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
        if (argc < 4) {
            fprintf(stderr, "server usage: %s server <ifname> <server_ip>\n", argv[0]);
            return 1;
        }
        is_server = 1;
        strncpy(g_ifname, argv[2], sizeof(g_ifname)-1);
        strncpy(g_server_ip_str, argv[3], sizeof(g_server_ip_str)-1);

        /* open sockets: returns recv_fd; raw_sock global is send socket */
        int recv_fd = open_raw_socket_and_bind(g_ifname, 1);
        if (recv_fd < 0) {
            log_printf("Server: failed to open raw sockets");
            return 1;
        }

        /* create server recv thread (using recv_fd) */
        pthread_t recv_tid;
        /* pass recv_fd via global duplication: duplicate descriptor into raw_sock+? simplest: set raw_sock_send already; but server_recv_thread uses global raw_sock for forwarding and uses recvfd via closure? */
        /* To resolve: we'll dup recv_fd into STDIN_FILENO slot and set a global recv_fd. Simpler: store in a static variable. */
        /* For neatness, create a small wrapper that sets a global recv_fd_var. */
        {
            /* store recv fd in a heap pointer passed as arg */
            int *pfd = (int *)malloc(sizeof(int));
            if (!pfd) return 1;
            *pfd = recv_fd;
            /* modify server_recv_thread to use recv_fd from * (we currently use raw_sock directly). For brevity in this prototype, we will instead use select on recv_fd in main loop. */
            /* Launch a thread that calls a small lambda — but in C89 we cannot create lambdas. So instead we will use recv_fd in this main thread loop (blocking) and not spawn another thread. */
            free(pfd);
        }

        log_printf("Server started on interface=%s ip=%s", g_ifname, g_server_ip_str);

        /* Simple server loop in main: receive via recv_fd, parse, register, forward */
        {
            unsigned char rxbuf[4096];
            while (1) {
                struct sockaddr_in src_sock;
                socklen_t slen = sizeof(src_sock);
                ssize_t r = recvfrom(recv_fd, rxbuf, sizeof(rxbuf), 0,
                                     (struct sockaddr *)&src_sock, &slen);
                if (r <= 0) {
                    log_printf("server recvfrom error: %s", strerror(errno));
                    continue;
                }
                struct in_addr pkt_src;
                unsigned char *payload;
                int payload_len;
                if (parse_ip_packet(rxbuf, (int)r, &pkt_src, &payload, &payload_len) < 0) continue;
                if (payload_len < (int)sizeof(struct priv_hdr)) continue;
                struct priv_hdr ph;
                memcpy(&ph, payload, sizeof(ph));
                if (ntohl((u32)ph.magic) != MAGIC) continue;
                u32 cid = ntohl(ph.client_id);
                /* register client */
                struct sockaddr_in client_addr;
                memset(&client_addr, 0, sizeof(client_addr));
                client_addr.sin_family = AF_INET;
                client_addr.sin_addr = pkt_src;
                server_register_client(cid, &client_addr);
                /* forward payload to others */
                server_forward_payload(payload, payload_len, pkt_src);
            }
        }

    } else if (strcmp(argv[1], "client") == 0) {
        if (argc < 4) {
            fprintf(stderr, "client usage: %s client <server_ip> <client_id>\n", argv[0]);
            return 1;
        }
        strncpy(g_server_ip_str, argv[2], sizeof(g_server_ip_str)-1);
        g_client_id = (u32)atoi(argv[3]);

        /* open sockets: returns recv_fd; raw_sock global is send socket */
        int recv_fd = open_raw_socket_and_bind(NULL, 0);
        if (recv_fd < 0) {
            log_printf("Client: failed to open raw sockets");
            return 1;
        }

        /* Prepare server sockaddr */
        struct sockaddr_in server_addr;
        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        if (inet_aton(g_server_ip_str, &server_addr.sin_addr) == 0) {
            log_printf("Invalid server ip %s", g_server_ip_str);
            return 1;
        }

        /* create send & recv threads: send uses raw_sock (global send), recv uses recv_fd */
        pthread_t send_tid, recv_tid;
        struct send_thread_arg sarg;
        sarg.server_addr = server_addr;

        /* Set raw_sock send socket already in open_raw_socket_and_bind */
        /* But recv thread should use recv_fd; we will swap raw_sock temporarily to allow recvfrom in client_recv_thread to read from recv_fd.
           To keep code simpler: we close previous raw_sock, set global raw_sock appropriately:
        */
        /* store global send socket in raw_sock (already set), but we need recv socket for recv thread. We'll pass it via a global by duping descriptor into raw_sock_recv_fd variable. Simpler: set global raw_sock_recv to recv_fd via dup. */
        /* For prototype, we will use recv_fd directly inside client_recv_thread by modifying that thread to use recv_fd passed as argument. To avoid extra refactor here, we will spawn a small thread wrapper that stores recv_fd in a static global. */

        /* Create thread for sender (it uses server_addr) */
        if (pthread_create(&send_tid, NULL, client_send_thread, &sarg) != 0) {
            log_printf("pthread_create send failed");
            return 1;
        }

        /* For receiving we will call a simple loop in main (rather than thread) */
        /* detach send thread to keep program running */
        pthread_detach(send_tid);

        /* Receive loop in main */
        unsigned char rxbuf[2048];
        while (1) {
            struct sockaddr_in src_sock;
            socklen_t slen = sizeof(src_sock);
            ssize_t r = recvfrom(recv_fd, rxbuf, sizeof(rxbuf), 0,
                                 (struct sockaddr *)&src_sock, &slen);
            if (r <= 0) {
                log_printf("client recvfrom error: %s", strerror(errno));
                continue;
            }
            struct in_addr pkt_src;
            unsigned char *payload;
            int payload_len;
            if (parse_ip_packet(rxbuf, (int)r, &pkt_src, &payload, &payload_len) < 0) continue;
            if (payload_len < (int)sizeof(struct priv_hdr)) continue;
            struct priv_hdr ph;
            memcpy(&ph, payload, sizeof(ph));
            if (ntohl((u32)ph.magic) != MAGIC) continue;
            u32 sender_id = ntohl(ph.client_id);
            u32 seq = ntohl(ph.seq);
            /* parse timestamp (big endian) */
            {
                u64 ts = 0;
                unsigned char *p = (unsigned char *)&ph.ts_ms;
                int i;
                for (i = 0; i < 8; i++) {
                    ts = (ts << 8) | (unsigned long long)p[i];
                }
                u64 delay = now_ms() - ts;
                if ((seq % 50) == 0) {
                    log_printf("RX from %u seq=%u delay=%llu ms", sender_id, seq, (unsigned long long)delay);
                }
            }
        }

    } else {
        fprintf(stderr, "Unknown mode: %s\n", argv[1]);
        return 1;
    }

    return 0;
}
