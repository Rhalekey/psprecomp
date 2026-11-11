/* psprecomp -- ad hoc play, over the local network.
 *
 * A PSP plays ad hoc over its own wireless: consoles in reach see each other, gather in a group
 * named by the game, and pass data through the matching library. Here the same calls run over UDP
 * among the computers of one local network, or among copies of a game on one computer:
 *
 *   - sceNet and sceNetAdhoc only start and stop.
 *   - sceNetAdhocctl: each copy tells the network who it is and which group it is in, a few times
 *     a second (a beacon, to a multicast group and to every network's broadcast address). Scanning
 *     lists the groups others are in, the peer list those in one's own group.
 *   - Netconf's ad hoc dialog (in utility.c) connects to, creates or joins its group at once.
 *   - sceNetAdhocMatching: hellos, requests, accepts, data and goodbyes, sent straight to the
 *     peer's address learned from its beacon, data sent again until its receipt comes back.
 *
 * Each copy is a console of its own, with an address made up at start. A game's handlers are
 * called at a frame's wait of the thread that uses the network, never from the network's own
 * thread, which only queues what arrives. Not at any wait: a handler allocates memory, and a wait
 * inside the game's allocator, with its lock held, had the allocator entered again and the game
 * jump to nothing once the last data arrived.
 */

#include "psprecomp/hle.h"
#include "psprecomp/dispatch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
typedef SOCKET net_socket;
#define BAD_SOCKET INVALID_SOCKET
#define close_socket closesocket
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int net_socket;
#define BAD_SOCKET (-1)
#define close_socket close
#endif

/* Where copies find each other. */
#define BEACON_PORT 30472
#define BEACON_GROUP "239.255.48.72"
#define BEACON_US 400000ull
#define PEER_GONE_US 4000000ull     /* a copy unheard for this long has left */
#define SCAN_US 1200000ull

#define MAGIC "PSPA"
#define WIRE_VERSION 1

/* What travels: a head, then what the type carries. */
enum {
    T_BEACON = 1,
    T_HELLO = 10, T_REQUEST, T_ACCEPT, T_COMPLETE, T_DENY, T_CANCEL, T_BYE, T_DATA, T_DATA_ACK, T_PING
};
#define HEAD_SIZE 56            /* magic 4, version, type, port 2, from 6, to 6, product 16, group 8, seq 4, length 4 */
#define MOST_PAYLOAD 65000

/* Adhocctl's states and events, as the firmware numbers them. */
enum { CTL_DISCONNECTED = 0, CTL_CONNECTED = 1, CTL_SCANNING = 2 };
enum { CTL_EVENT_ERROR = 0, CTL_EVENT_CONNECT = 1, CTL_EVENT_DISCONNECT = 2, CTL_EVENT_SCAN = 3 };

/* Matching's modes, events and errors. */
enum { MODE_HOST = 1, MODE_CLIENT = 2, MODE_PTP = 3 };
enum {
    EV_HELLO = 1, EV_REQUEST, EV_LEAVE, EV_DENY, EV_CANCEL, EV_ACCEPT, EV_ESTABLISHED, EV_TIMEOUT, EV_ERROR, EV_BYE,
    EV_DATA, EV_DATA_ACK, EV_DATA_TIMEOUT
};
#define ERR_INVALID_MODE 0x80410801u
#define ERR_INVALID_MAXNUM 0x80410803u
#define ERR_INVALID_ARG 0x80410806u
#define ERR_INVALID_ID 0x80410807u
#define ERR_NO_SPACE 0x80410809u
#define ERR_IS_RUNNING 0x8041080Au
#define ERR_NOT_RUNNING 0x8041080Bu
#define ERR_UNKNOWN_TARGET 0x8041080Cu
#define ERR_TARGET_NOT_READY 0x8041080Du
#define ERR_EXCEED_MAXNUM 0x8041080Eu
#define ERR_REQUEST_IN_PROGRESS 0x8041080Fu
#define ERR_ALREADY_ESTABLISHED 0x80410810u
#define ERR_NOT_INITIALIZED 0x80410813u
#define ERR_INVALID_DATALEN 0x80410816u
#define ERR_NOT_ESTABLISHED 0x80410817u
#define ERR_DATA_BUSY 0x80410818u
#define ERR_CTL_NOT_INITIALIZED 0x80410B08u

/* ---- the clock and the log ----------------------------------------------- */

static uint64_t now_us(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (uint64_t)(t.QuadPart / freq.QuadPart) * 1000000ull + (uint64_t)(t.QuadPart % freq.QuadPart) * 1000000ull / (uint64_t)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
#endif
}

static const char *mac_text(const uint8_t *m) {
    static char s[4][18];
    static int k;
    k = (k + 1) & 3;
    snprintf(s[k], sizeof s[k], "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    return s[k];
}

/* ---- who this copy is ------------------------------------------------------ */

static uint8_t g_mac[6];
static char g_nickname[128] = "";

static void make_mac(void) {
    if (g_mac[0] || g_mac[1]) return;
    uint64_t seed = now_us() ^ ((uint64_t)time(NULL) << 20);
#ifdef _WIN32
    seed ^= (uint64_t)GetCurrentProcessId() << 32;
#else
    seed ^= (uint64_t)getpid() << 32;
#endif
    for (int i = 0; i < 6; i++) {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        g_mac[i] = (uint8_t)(seed >> 33);
    }
    g_mac[0] = (uint8_t)((g_mac[0] & 0xFC) | 0x02);    /* locally administered, one station */
}

void psp_netadhoc_set_nickname(const char *name) {
    snprintf(g_nickname, sizeof g_nickname, "%s", name ? name : "");
}

const char *psp_netadhoc_nickname(void) {
    if (!g_nickname[0]) {
#ifdef _WIN32
        char name[MAX_COMPUTERNAME_LENGTH + 1];
        DWORD n = sizeof name;
        if (GetComputerNameA(name, &n) && n) snprintf(g_nickname, sizeof g_nickname, "%s", name);
#else
        char name[64];
        if (!gethostname(name, sizeof name)) snprintf(g_nickname, sizeof g_nickname, "%s", name);
#endif
        if (!g_nickname[0]) snprintf(g_nickname, sizeof g_nickname, "Player");
    }
    return g_nickname;
}

/* ---- the network ----------------------------------------------------------- */

typedef struct packet {
    struct packet *next;
    struct sockaddr_in from;
    int size;
    uint8_t data[];
} packet;

static net_socket g_send = BAD_SOCKET, g_listen = BAD_SOCKET;
static uint16_t g_data_port;
static volatile int g_running;
static packet *g_queue_head, *g_queue_tail;
static int g_queue_count;
#ifdef _WIN32
static CRITICAL_SECTION g_queue_lock;
static HANDLE g_thread;
#define LOCK() EnterCriticalSection(&g_queue_lock)
#define UNLOCK() LeaveCriticalSection(&g_queue_lock)
#else
static pthread_mutex_t g_queue_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_thread;
#define LOCK() pthread_mutex_lock(&g_queue_lock)
#define UNLOCK() pthread_mutex_unlock(&g_queue_lock)
#endif

/* This computer's IPv4 networks: where beacons go out, as broadcast and multicast. */
#define MOST_NETS 16
static struct { uint32_t addr, broadcast; } g_net[MOST_NETS];
static int g_nnets;

static void find_networks(void) {
    g_nnets = 0;
#ifdef _WIN32
    ULONG size = 16384;
    IP_ADAPTER_ADDRESSES *list = NULL;
    for (int tries = 0; tries < 3; tries++) {
        list = (IP_ADAPTER_ADDRESSES *)malloc(size);
        if (!list) return;
        const ULONG r = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, NULL, list, &size);
        if (r == NO_ERROR) break;
        free(list);
        list = NULL;
        if (r != ERROR_BUFFER_OVERFLOW) return;
    }
    for (IP_ADAPTER_ADDRESSES *a = list; a && g_nnets < MOST_NETS; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u && g_nnets < MOST_NETS; u = u->Next) {
            if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
            const uint32_t ip = ntohl(((struct sockaddr_in *)u->Address.lpSockaddr)->sin_addr.s_addr);
            const int prefix = u->OnLinkPrefixLength <= 32 ? u->OnLinkPrefixLength : 24;
            const uint32_t mask = prefix ? 0xFFFFFFFFu << (32 - prefix) : 0;
            g_net[g_nnets].addr = ip;
            g_net[g_nnets].broadcast = (ip & mask) | ~mask;
            g_nnets++;
        }
    }
    free(list);
#else
    struct ifaddrs *list = NULL;
    if (getifaddrs(&list)) return;
    for (struct ifaddrs *a = list; a && g_nnets < MOST_NETS; a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET || !(a->ifa_flags & IFF_UP)) continue;
        const uint32_t ip = ntohl(((struct sockaddr_in *)a->ifa_addr)->sin_addr.s_addr);
        const uint32_t mask = a->ifa_netmask ? ntohl(((struct sockaddr_in *)a->ifa_netmask)->sin_addr.s_addr) : 0xFFFFFF00u;
        g_net[g_nnets].addr = ip;
        g_net[g_nnets].broadcast = (ip & mask) | ~mask;
        g_nnets++;
    }
    freeifaddrs(list);
#endif
}

static void take_packet(net_socket s) {
    uint8_t buf[HEAD_SIZE + MOST_PAYLOAD];
    struct sockaddr_in from;
#ifdef _WIN32
    int flen = sizeof from;
#else
    socklen_t flen = sizeof from;
#endif
    const int n = (int)recvfrom(s, (char *)buf, (int)sizeof buf, 0, (struct sockaddr *)&from, &flen);
    if (n < HEAD_SIZE || memcmp(buf, MAGIC, 4) || buf[4] != WIRE_VERSION) return;
    if (!memcmp(buf + 8, g_mac, 6)) return;         /* its own, back by multicast */
    packet *p = (packet *)malloc(sizeof *p + (size_t)n);
    if (!p) return;
    p->next = NULL;
    p->from = from;
    p->size = n;
    memcpy(p->data, buf, (size_t)n);
    LOCK();
    if (g_queue_count > 512) {
        UNLOCK();
        free(p);
        return;
    }
    if (g_queue_tail) g_queue_tail->next = p;
    else g_queue_head = p;
    g_queue_tail = p;
    g_queue_count++;
    UNLOCK();
}

#ifdef _WIN32
static DWORD WINAPI receive_thread(LPVOID arg)
#else
static void *receive_thread(void *arg)
#endif
{
    (void)arg;
    while (g_running) {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(g_send, &set);
        FD_SET(g_listen, &set);
        struct timeval tv = {0, 200000};
        const int most = (int)(g_send > g_listen ? g_send : g_listen);
        if (select(most + 1, &set, NULL, NULL, &tv) <= 0) continue;
        if (FD_ISSET(g_send, &set)) take_packet(g_send);
        if (FD_ISSET(g_listen, &set)) take_packet(g_listen);
    }
    return 0;
}

static packet *next_packet(void) {
    LOCK();
    packet *p = g_queue_head;
    if (p) {
        g_queue_head = p->next;
        if (!g_queue_head) g_queue_tail = NULL;
        g_queue_count--;
    }
    UNLOCK();
    return p;
}

static void join_beacon_group(void) {
    struct ip_mreq m;
    memset(&m, 0, sizeof m);
    m.imr_multiaddr.s_addr = inet_addr(BEACON_GROUP);
    m.imr_interface.s_addr = htonl(INADDR_ANY);
    setsockopt(g_listen, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char *)&m, sizeof m);
    for (int i = 0; i < g_nnets; i++) {
        m.imr_interface.s_addr = htonl(g_net[i].addr);
        setsockopt(g_listen, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char *)&m, sizeof m);
    }
}

static int network_start(void) {
    if (g_running) return 1;
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) return 0;
    InitializeCriticalSection(&g_queue_lock);
#endif
    make_mac();
    find_networks();
    /* One socket to send from and be answered on, at a port of its own; one where beacons arrive,
     * at a port every copy shares. */
    g_send = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    g_listen = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_send == BAD_SOCKET || g_listen == BAD_SOCKET) {
        fprintf(stderr, "adhoc: no network sockets\n");
        return 0;
    }
    int on = 1;
    setsockopt(g_send, SOL_SOCKET, SO_BROADCAST, (const char *)&on, sizeof on);
    setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, sizeof on);
#ifdef SO_REUSEPORT
    setsockopt(g_listen, SOL_SOCKET, SO_REUSEPORT, (const char *)&on, sizeof on);
#endif
    const unsigned char loop = 1, ttl = 1;
    setsockopt(g_send, IPPROTO_IP, IP_MULTICAST_LOOP, (const char *)&loop, sizeof loop);
    setsockopt(g_send, IPPROTO_IP, IP_MULTICAST_TTL, (const char *)&ttl, sizeof ttl);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = 0;
    if (bind(g_send, (struct sockaddr *)&a, sizeof a)) {
        fprintf(stderr, "adhoc: could not open a port\n");
        return 0;
    }
#ifdef _WIN32
    int alen = sizeof a;
#else
    socklen_t alen = sizeof a;
#endif
    getsockname(g_send, (struct sockaddr *)&a, &alen);
    g_data_port = ntohs(a.sin_port);
    a.sin_port = htons(BEACON_PORT);
    if (bind(g_listen, (struct sockaddr *)&a, sizeof a)) fprintf(stderr, "adhoc: port %d is taken; others may not see this copy\n", BEACON_PORT);
    join_beacon_group();
    g_running = 1;
#ifdef _WIN32
    g_thread = CreateThread(NULL, 0, receive_thread, NULL, 0, NULL);
#else
    pthread_create(&g_thread, NULL, receive_thread, NULL);
#endif
    fprintf(stderr, "adhoc: on the local network as %s (\"%s\"), port %u, %d networks\n", mac_text(g_mac), psp_netadhoc_nickname(), g_data_port, g_nnets);
    return 1;
}

static void send_to(const struct sockaddr_in *to, const uint8_t *buf, int n) {
    sendto(g_send, (const char *)buf, n, 0, (const struct sockaddr *)to, sizeof *to);
}

/* ---- adhocctl -------------------------------------------------------------- */

/* Another copy, as its beacons tell it. */
#define MOST_PEERS 32
typedef struct {
    int used;
    uint8_t mac[6];
    char nickname[128];
    char group[8];
    struct sockaddr_in addr;
    uint64_t seen;
} lan_peer;

static lan_peer g_peer[MOST_PEERS];
static uint32_t g_net_thread = 0xFFFFFFFFu;    /* the thread that set the handlers */
static uint64_t g_last_delivery;
static int g_ctl_init, g_ctl_state;
static char g_product[16];
static char g_group[8];
static uint64_t g_scan_until, g_next_beacon;
#define MOST_HANDLERS 4
static struct { uint32_t fn, arg; } g_ctl_handler[MOST_HANDLERS];

/* The game's handlers to call, in order, where the guest next waits. */
typedef struct pending {
    struct pending *next;
    uint32_t fn;
    int kind;                       /* 0 adhocctl: (event, error, arg); 1 matching: (id, event, mac, optlen, opt) */
    uint32_t a0, a1, a2;
    uint8_t mac[6];
    int optlen;
    uint8_t opt[];
} pending;
static pending *g_pend_head, *g_pend_tail;

static void queue_call(pending *p) {
    p->next = NULL;
    if (g_pend_tail) g_pend_tail->next = p;
    else g_pend_head = p;
    g_pend_tail = p;
}

static void ctl_event(int event, int error) {
    for (int i = 0; i < MOST_HANDLERS; i++) {
        if (!g_ctl_handler[i].fn) continue;
        pending *p = (pending *)calloc(1, sizeof *p);
        if (!p) return;
        p->fn = g_ctl_handler[i].fn;
        p->kind = 0;
        p->a0 = (uint32_t)event;
        p->a1 = (uint32_t)error;
        p->a2 = g_ctl_handler[i].arg;
        queue_call(p);
    }
}

static void put_head(uint8_t *b, int type, uint16_t port, const uint8_t *to, uint32_t seq, int length) {
    memset(b, 0, HEAD_SIZE);
    memcpy(b, MAGIC, 4);
    b[4] = WIRE_VERSION;
    b[5] = (uint8_t)type;
    b[6] = (uint8_t)port;
    b[7] = (uint8_t)(port >> 8);
    memcpy(b + 8, g_mac, 6);
    if (to) memcpy(b + 14, to, 6);
    else memset(b + 14, 0xFF, 6);
    memcpy(b + 20, g_product, 16);
    if (g_ctl_state == CTL_CONNECTED) memcpy(b + 36, g_group, 8);
    for (int i = 0; i < 4; i++) b[44 + i] = (uint8_t)(seq >> (8 * i));
    for (int i = 0; i < 4; i++) b[48 + i] = (uint8_t)((uint32_t)length >> (8 * i));
}

static uint32_t get32(const uint8_t *b) { return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24; }

static void send_beacon(void) {
    uint8_t b[HEAD_SIZE + 132];
    memset(b, 0, sizeof b);
    put_head(b, T_BEACON, 0, NULL, 0, 132);
    memcpy(b + HEAD_SIZE, psp_netadhoc_nickname(), strlen(psp_netadhoc_nickname()) < 127 ? strlen(psp_netadhoc_nickname()) : 127);
    b[HEAD_SIZE + 128] = (uint8_t)g_ctl_state;
    struct sockaddr_in to;
    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_port = htons(BEACON_PORT);
    to.sin_addr.s_addr = inet_addr(BEACON_GROUP);
    for (int i = 0; i < g_nnets; i++) {
        struct in_addr via;
        via.s_addr = htonl(g_net[i].addr);
        setsockopt(g_send, IPPROTO_IP, IP_MULTICAST_IF, (const char *)&via, sizeof via);
        send_to(&to, b, (int)sizeof b);
    }
    for (int i = 0; i < g_nnets; i++) {
        if ((g_net[i].addr >> 24) == 127) continue;
        to.sin_addr.s_addr = htonl(g_net[i].broadcast);
        send_to(&to, b, (int)sizeof b);
    }
    to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    send_to(&to, b, (int)sizeof b);
}

static lan_peer *find_peer(const uint8_t *mac) {
    for (int i = 0; i < MOST_PEERS; i++)
        if (g_peer[i].used && !memcmp(g_peer[i].mac, mac, 6)) return &g_peer[i];
    return NULL;
}

static int same_group(const lan_peer *p) {
    return g_ctl_state == CTL_CONNECTED && p->group[0] && !memcmp(p->group, g_group, 8);
}

static void take_beacon(const packet *pk) {
    const uint8_t *b = pk->data;
    if (memcmp(b + 20, g_product, 16)) return;      /* another game */
    lan_peer *p = find_peer(b + 8);
    if (!p) {
        for (int i = 0; i < MOST_PEERS && !p; i++)
            if (!g_peer[i].used) p = &g_peer[i];
        if (!p) return;
        memset(p, 0, sizeof *p);
        p->used = 1;
        memcpy(p->mac, b + 8, 6);
    }
    const int had_group = p->group[0] != 0;
    char was[8];
    memcpy(was, p->group, 8);
    p->addr = pk->from;
    p->seen = now_us();
    if (pk->size >= HEAD_SIZE + 128) {
        memcpy(p->nickname, b + HEAD_SIZE, 127);
        p->nickname[127] = 0;
    }
    memcpy(p->group, b + 36, 8);
    if ((!had_group && p->group[0]) || (had_group && memcmp(was, p->group, 8)))
        fprintf(stderr, "adhoc: %s (\"%s\") at %s:%u is in group \"%.8s\"\n", mac_text(p->mac), p->nickname, inet_ntoa(p->addr.sin_addr),
                ntohs(p->addr.sin_port), p->group);
}

static void ctl_connect(const char *group) {
    memset(g_group, 0, sizeof g_group);
    memcpy(g_group, group, strnlen(group, 8));
    g_ctl_state = CTL_CONNECTED;
    g_next_beacon = 0;
    fprintf(stderr, "adhoc: in group \"%.8s\"\n", g_group);
    ctl_event(CTL_EVENT_CONNECT, 0);
}

/* Netconf's ad hoc dialog (utility.c): 2 connects, 4 creates, 5 joins; all go into the group. */
int psp_netadhoc_netconf(uint32_t action, uint32_t adhoc_param) {
    if (action != 2 && action != 4 && action != 5) {
        fprintf(stderr, "adhoc: netconf action %u is not ad hoc; nothing done\n", action);
        return 0;
    }
    if (!g_ctl_init) {
        fprintf(stderr, "adhoc: netconf before adhocctl started\n");
        return 1;
    }
    char group[9] = {0};
    for (int i = 0; adhoc_param && i < 8; i++) group[i] = (char)psp_read8(adhoc_param + (uint32_t)i);
    ctl_connect(group);
    return 0;
}

static void hle_ok(void) { psp_ret(0); }

static void hle_EtherNtostr(void) {
    const uint32_t mac = psp_arg(0), out = psp_arg(1);
    uint8_t m[6];
    for (int i = 0; i < 6; i++) m[i] = psp_read8(mac + (uint32_t)i);
    const char *s = mac_text(m);
    for (int i = 0; i < 18; i++) psp_write8(out + (uint32_t)i, (uint8_t)s[i]);
    psp_ret(0);
}

static void hle_WlanGetEtherAddr(void) {
    make_mac();
    for (int i = 0; i < 6; i++) psp_write8(psp_arg(0) + (uint32_t)i, g_mac[i]);
    psp_ret(0);
}

/* (stack size, priority, product): the product, its type and its code, keeps games apart. */
static void hle_ctl_Init(void) {
    const uint32_t product = psp_arg(2);
    memset(g_product, 0, sizeof g_product);
    if (product) {
        g_product[0] = (char)psp_read8(product);
        for (int i = 0; i < 9; i++) g_product[4 + i] = (char)psp_read8(product + 4 + (uint32_t)i);
    }
    if (!network_start()) {
        psp_ret(0x80410B00u);
        return;
    }
    g_ctl_init = 1;
    g_ctl_state = CTL_DISCONNECTED;
    g_next_beacon = 0;
    fprintf(stderr, "adhoc: started for %.9s\n", g_product + 4);
    psp_ret(0);
}

static void hle_ctl_Term(void) {
    g_ctl_init = 0;
    g_ctl_state = CTL_DISCONNECTED;
    memset(g_group, 0, sizeof g_group);
    memset(g_ctl_handler, 0, sizeof g_ctl_handler);
    psp_ret(0);
}

static void hle_ctl_AddHandler(void) {
    for (int i = 0; i < MOST_HANDLERS; i++)
        if (!g_ctl_handler[i].fn) {
            g_ctl_handler[i].fn = psp_arg(0);
            g_ctl_handler[i].arg = psp_arg(1);
            g_net_thread = psp_threadman_current();
            psp_ret((uint32_t)i);
            return;
        }
    psp_ret(0x80410B03u);
}

static void hle_ctl_DelHandler(void) {
    const uint32_t id = psp_arg(0);
    if (id < MOST_HANDLERS) g_ctl_handler[id].fn = 0;
    psp_ret(0);
}

static void hle_ctl_Disconnect(void) {
    if (g_ctl_state == CTL_CONNECTED) {
        fprintf(stderr, "adhoc: left group \"%.8s\"\n", g_group);
        g_ctl_state = CTL_DISCONNECTED;
        memset(g_group, 0, sizeof g_group);
        g_next_beacon = 0;
        ctl_event(CTL_EVENT_DISCONNECT, 0);
    }
    psp_ret(0);
}

static void hle_ctl_GetState(void) {
    psp_write32(psp_arg(0), (uint32_t)g_ctl_state);
    psp_ret(g_ctl_init ? 0 : ERR_CTL_NOT_INITIALIZED);
}

static void hle_ctl_Scan(void) {
    if (!g_ctl_init) {
        psp_ret(ERR_CTL_NOT_INITIALIZED);
        return;
    }
    g_ctl_state = CTL_SCANNING;
    g_scan_until = now_us() + SCAN_US;
    psp_ret(0);
}

/* (int *length, SceNetAdhocctlScanInfo *buf): the groups others are in, 28 bytes each: next,
 * channel, group name, bssid, mode. With no buffer, the room they need. */
static void hle_ctl_GetScanInfo(void) {
    const uint32_t lenp = psp_arg(0), buf = psp_arg(1);
    char groups[MOST_PEERS][8];
    const uint8_t *bssid[MOST_PEERS];
    int n = 0;
    for (int i = 0; i < MOST_PEERS; i++) {
        if (!g_peer[i].used || !g_peer[i].group[0]) continue;
        int seen = 0;
        for (int k = 0; k < n; k++)
            if (!memcmp(groups[k], g_peer[i].group, 8)) seen = 1;
        if (seen) continue;
        memcpy(groups[n], g_peer[i].group, 8);
        bssid[n] = g_peer[i].mac;
        n++;
    }
    if (!buf) {
        psp_write32(lenp, (uint32_t)n * 28);
        psp_ret(0);
        return;
    }
    const int room = (int)(psp_read32(lenp) / 28);
    if (n > room) n = room;
    for (int k = 0; k < n; k++) {
        const uint32_t at = buf + (uint32_t)k * 28;
        psp_write32(at, k + 1 < n ? at + 28 : 0);
        psp_write32(at + 4, 1);
        for (int i = 0; i < 8; i++) psp_write8(at + 8 + (uint32_t)i, (uint8_t)groups[k][i]);
        for (int i = 0; i < 6; i++) psp_write8(at + 16 + (uint32_t)i, bssid[k][i]);
        psp_write16(at + 22, 0);
        psp_write32(at + 24, 1);
    }
    psp_write32(lenp, (uint32_t)n * 28);
    psp_ret(0);
}

/* (int *length, SceNetAdhocctlPeerInfo *buf): those in one's group, 152 bytes each: next,
 * nickname, address, and when last heard. */
static void hle_ctl_GetPeerList(void) {
    const uint32_t lenp = psp_arg(0), buf = psp_arg(1);
    int n = 0;
    for (int i = 0; i < MOST_PEERS; i++)
        if (g_peer[i].used && same_group(&g_peer[i])) n++;
    if (!buf) {
        psp_write32(lenp, (uint32_t)n * 152);
        psp_ret(0);
        return;
    }
    const int room = (int)(psp_read32(lenp) / 152);
    int k = 0;
    for (int i = 0; i < MOST_PEERS && k < room; i++) {
        if (!g_peer[i].used || !same_group(&g_peer[i])) continue;
        const uint32_t at = buf + (uint32_t)k * 152;
        for (uint32_t z = 0; z < 152; z += 4) psp_write32(at + z, 0);
        for (int c = 0; c < 127 && g_peer[i].nickname[c]; c++) psp_write8(at + 4 + (uint32_t)c, (uint8_t)g_peer[i].nickname[c]);
        for (int c = 0; c < 6; c++) psp_write8(at + 132 + (uint32_t)c, g_peer[i].mac[c]);
        psp_write32(at + 144, (uint32_t)g_peer[i].seen);
        psp_write32(at + 148, (uint32_t)(g_peer[i].seen >> 32));
        k++;
    }
    for (int j = 0; j < k; j++) psp_write32(buf + (uint32_t)j * 152, j + 1 < k ? buf + (uint32_t)(j + 1) * 152 : 0);
    psp_write32(lenp, (uint32_t)k * 152);
    psp_ret(0);
}

/* (SceNetAdhocctlParameter *): channel, group name, bssid, nickname. */
static void hle_ctl_GetParameter(void) {
    const uint32_t at = psp_arg(0);
    psp_write32(at, 1);
    for (int i = 0; i < 8; i++) psp_write8(at + 4 + (uint32_t)i, (uint8_t)g_group[i]);
    for (int i = 0; i < 6; i++) psp_write8(at + 12 + (uint32_t)i, g_mac[i]);
    const char *nick = psp_netadhoc_nickname();
    for (int i = 0; i < 128; i++) psp_write8(at + 18 + (uint32_t)i, (uint8_t)(i < 127 && i < (int)strlen(nick) ? nick[i] : 0));
    psp_ret(0);
}

/* ---- matching -------------------------------------------------------------- */

enum { P_KNOWN = 1, P_IN_REQUEST, P_OUT_REQUEST, P_ACCEPTED, P_ESTABLISHED };

typedef struct {
    int used;
    uint8_t mac[6];
    int state;
    uint64_t seen;
    /* What is on its way to it, until its receipt comes. */
    uint32_t out_seq;
    uint8_t *out;
    int out_len, resends;
    uint64_t resend_at;
    uint32_t in_seq;
} match_peer;

#define MOST_CONTEXTS 8
#define CONTEXT_PEERS 16
typedef struct {
    int used, running;
    int mode, maxnum;
    uint16_t port;
    uint32_t rxbuflen;
    uint64_t hello_us, keepalive_us, rexmt_us;
    int init_count;
    uint32_t handler;
    uint8_t *hello;
    int hello_len;
    uint64_t next_hello, next_ping;
    match_peer peer[CONTEXT_PEERS];
} match_context;

static match_context g_ctx[MOST_CONTEXTS];
static int g_matching_init;

static match_context *context(uint32_t id) {
    return id >= 1 && id <= MOST_CONTEXTS && g_ctx[id - 1].used ? &g_ctx[id - 1] : NULL;
}

static match_peer *match_find(match_context *c, const uint8_t *mac) {
    for (int i = 0; i < CONTEXT_PEERS; i++)
        if (c->peer[i].used && !memcmp(c->peer[i].mac, mac, 6)) return &c->peer[i];
    return NULL;
}

static match_peer *match_add(match_context *c, const uint8_t *mac) {
    match_peer *p = match_find(c, mac);
    if (p) return p;
    for (int i = 0; i < CONTEXT_PEERS; i++)
        if (!c->peer[i].used) {
            p = &c->peer[i];
            memset(p, 0, sizeof *p);
            p->used = 1;
            memcpy(p->mac, mac, 6);
            p->state = P_KNOWN;
            p->seen = now_us();
            p->in_seq = 0xFFFFFFFFu;
            return p;
        }
    return NULL;
}

static void match_drop(match_peer *p) {
    free(p->out);
    memset(p, 0, sizeof *p);
}

static int established(const match_context *c) {
    int n = 0;
    for (int i = 0; i < CONTEXT_PEERS; i++)
        if (c->peer[i].used && (c->peer[i].state == P_ESTABLISHED || c->peer[i].state == P_ACCEPTED)) n++;
    return n;
}

static void match_event(match_context *c, int event, const uint8_t *mac, const uint8_t *opt, int optlen) {
    if (!c->handler) return;
    if (optlen < 0 || !opt) optlen = 0;
    pending *p = (pending *)calloc(1, sizeof *p + (size_t)optlen);
    if (!p) return;
    p->fn = c->handler;
    p->kind = 1;
    p->a0 = (uint32_t)(c - g_ctx) + 1;
    p->a1 = (uint32_t)event;
    memcpy(p->mac, mac, 6);
    p->optlen = optlen;
    if (optlen) memcpy(p->opt, opt, (size_t)optlen);
    queue_call(p);
}

/* To one peer of the group, by the address its beacons came from. */
static void match_send(match_context *c, int type, const uint8_t *to, uint32_t seq, const uint8_t *data, int len) {
    const lan_peer *lp = find_peer(to);
    if (!lp) return;
    if (len > MOST_PAYLOAD) len = MOST_PAYLOAD;
    uint8_t *b = (uint8_t *)malloc((size_t)(HEAD_SIZE + len));
    if (!b) return;
    put_head(b, type, c->port, to, seq, len);
    if (len) memcpy(b + HEAD_SIZE, data, (size_t)len);
    send_to(&lp->addr, b, HEAD_SIZE + len);
    free(b);
}

static void take_matching(const packet *pk) {
    const uint8_t *b = pk->data;
    const int type = b[5];
    const uint16_t port = (uint16_t)(b[6] | b[7] << 8);
    const uint8_t *from = b + 8;
    const uint32_t seq = get32(b + 44);
    int len = (int)get32(b + 48);
    if (len > pk->size - HEAD_SIZE) len = pk->size - HEAD_SIZE;
    const uint8_t *opt = b + HEAD_SIZE;
    if (memcmp(b + 20, g_product, 16)) return;
    lan_peer *lp = find_peer(from);
    if (lp) {
        lp->seen = now_us();
        if (lp->addr.sin_port != pk->from.sin_port || lp->addr.sin_addr.s_addr != pk->from.sin_addr.s_addr) lp->addr = pk->from;
    }
    for (int k = 0; k < MOST_CONTEXTS; k++) {
        match_context *c = &g_ctx[k];
        if (!c->used || !c->running || c->port != port) continue;
        match_peer *p = match_find(c, from);
        if (p) p->seen = now_us();
        switch (type) {
        case T_HELLO:
            /* Hosts and equals say hello; clients and equals listen. */
            if (c->mode == MODE_HOST) break;
            if (!p) {
                p = match_add(c, from);
                if (p) match_event(c, EV_HELLO, from, opt, len);
            }
            break;
        case T_REQUEST:
            if (c->mode == MODE_CLIENT) break;
            if (!p) p = match_add(c, from);
            if (!p) break;
            if (p->state == P_ESTABLISHED || p->state == P_ACCEPTED || p->state == P_IN_REQUEST) break;
            if (c->maxnum > 1 && established(c) >= c->maxnum - 1) {
                match_send(c, T_DENY, from, 0, NULL, 0);
                break;
            }
            p->state = P_IN_REQUEST;
            match_event(c, EV_REQUEST, from, opt, len);
            break;
        case T_ACCEPT:
            if (!p || p->state != P_OUT_REQUEST) break;
            match_event(c, EV_ACCEPT, from, opt, len);
            p->state = P_ESTABLISHED;
            match_send(c, T_COMPLETE, from, 0, NULL, 0);
            match_event(c, EV_ESTABLISHED, from, NULL, 0);
            break;
        case T_COMPLETE:
            if (!p || p->state != P_ACCEPTED) break;
            p->state = P_ESTABLISHED;
            match_event(c, EV_ESTABLISHED, from, NULL, 0);
            break;
        case T_DENY:
            if (!p || p->state != P_OUT_REQUEST) break;
            p->state = P_KNOWN;
            match_event(c, EV_DENY, from, opt, len);
            break;
        case T_CANCEL:
            if (!p) break;
            if (p->state == P_IN_REQUEST) match_event(c, EV_CANCEL, from, opt, len);
            else if (p->state == P_ESTABLISHED || p->state == P_ACCEPTED || p->state == P_OUT_REQUEST) match_event(c, EV_LEAVE, from, opt, len);
            free(p->out);
            p->out = NULL;
            p->state = P_KNOWN;
            break;
        case T_BYE:
            if (!p) break;
            if (p->state != P_KNOWN) match_event(c, EV_BYE, from, NULL, 0);
            match_drop(p);
            break;
        case T_DATA:
            if (!p || p->state != P_ESTABLISHED) break;
            match_send(c, T_DATA_ACK, from, seq, NULL, 0);
            if (seq != p->in_seq) {
                p->in_seq = seq;
                match_event(c, EV_DATA, from, opt, len);
            }
            break;
        case T_DATA_ACK:
            if (!p || !p->out || seq != p->out_seq) break;
            free(p->out);
            p->out = NULL;
            match_event(c, EV_DATA_ACK, from, NULL, 0);
            break;
        default:
            break;
        }
    }
}

/* Timers: hellos, keepalives, data sent again, and those gone quiet. */
static void match_timers(uint64_t now) {
    for (int k = 0; k < MOST_CONTEXTS; k++) {
        match_context *c = &g_ctx[k];
        if (!c->used || !c->running) continue;
        if ((c->mode == MODE_HOST || c->mode == MODE_PTP) && now >= c->next_hello) {
            c->next_hello = now + c->hello_us;
            for (int i = 0; i < MOST_PEERS; i++)
                if (g_peer[i].used && same_group(&g_peer[i])) match_send(c, T_HELLO, g_peer[i].mac, 0, c->hello, c->hello_len);
        }
        const int ping = now >= c->next_ping;
        if (ping) c->next_ping = now + c->keepalive_us;
        uint64_t quiet = c->keepalive_us * (uint64_t)(c->init_count > 0 ? c->init_count : 10);
        if (quiet > 15000000ull) quiet = 15000000ull;
        if (quiet < 3000000ull) quiet = 3000000ull;
        for (int i = 0; i < CONTEXT_PEERS; i++) {
            match_peer *p = &c->peer[i];
            if (!p->used) continue;
            if (p->state == P_KNOWN) {
                if (now > p->seen + PEER_GONE_US) match_drop(p);
                continue;
            }
            if (now > p->seen + quiet) {
                fprintf(stderr, "adhoc: %s went quiet\n", mac_text(p->mac));
                match_event(c, EV_TIMEOUT, p->mac, NULL, 0);
                match_drop(p);
                continue;
            }
            if (ping) match_send(c, T_PING, p->mac, 0, NULL, 0);
            if (p->out && now >= p->resend_at) {
                if (++p->resends > (c->init_count > 0 ? c->init_count : 10)) {
                    free(p->out);
                    p->out = NULL;
                    match_event(c, EV_DATA_TIMEOUT, p->mac, NULL, 0);
                } else {
                    match_send(c, T_DATA, p->mac, p->out_seq, p->out, p->out_len);
                    p->resend_at = now + c->rexmt_us;
                }
            }
        }
    }
}

static void hle_m_Init(void) {
    g_matching_init = 1;
    psp_ret(0);
}

static void hle_m_Term(void) {
    for (int k = 0; k < MOST_CONTEXTS; k++) {
        for (int i = 0; i < CONTEXT_PEERS; i++)
            if (g_ctx[k].peer[i].used) match_drop(&g_ctx[k].peer[i]);
        free(g_ctx[k].hello);
    }
    memset(g_ctx, 0, sizeof g_ctx);
    g_matching_init = 0;
    psp_ret(0);
}

/* (mode, maxnum, port, rxbuflen, hello_int, keepalive_int, init_count, rexmt_int, handler), the
 * last on the stack. */
static void hle_m_Create(void) {
    const int mode = (int)psp_arg(0), maxnum = (int)psp_arg(1);
    if (!g_matching_init) {
        psp_ret(ERR_NOT_INITIALIZED);
        return;
    }
    if (mode < MODE_HOST || mode > MODE_PTP) {
        psp_ret(ERR_INVALID_MODE);
        return;
    }
    if (mode != MODE_CLIENT && (maxnum < 2 || maxnum > CONTEXT_PEERS)) {
        psp_ret(ERR_INVALID_MAXNUM);
        return;
    }
    for (int k = 0; k < MOST_CONTEXTS; k++) {
        match_context *c = &g_ctx[k];
        if (c->used) continue;
        memset(c, 0, sizeof *c);
        c->used = 1;
        c->mode = mode;
        c->maxnum = maxnum;
        c->port = (uint16_t)psp_arg(2);
        c->rxbuflen = psp_arg(3);
        c->hello_us = psp_arg(4) ? psp_arg(4) : 1000000u;
        c->keepalive_us = psp_arg(5) ? psp_arg(5) : 1000000u;
        c->init_count = (int)psp_arg(6);
        c->rexmt_us = psp_arg(7) ? psp_arg(7) : 500000u;
        c->handler = psp_read32(psp_cpu.r[PSP_REG_SP]);
        g_net_thread = psp_threadman_current();
        fprintf(stderr, "adhoc: matching %d made, mode %d, up to %d, port %u\n", k + 1, mode, maxnum, c->port);
        psp_ret((uint32_t)k + 1);
        return;
    }
    psp_ret(ERR_NO_SPACE);
}

/* (id, event thread's priority, its stack, input thread's priority, its stack, hello's length, hello). */
static void hle_m_Start(void) {
    match_context *c = context(psp_arg(0));
    if (!c) {
        psp_ret(ERR_INVALID_ID);
        return;
    }
    if (c->running) {
        psp_ret(ERR_IS_RUNNING);
        return;
    }
    const int len = (int)psp_arg(5);
    const uint32_t data = psp_arg(6);
    free(c->hello);
    c->hello = NULL;
    c->hello_len = 0;
    if (len > 0 && data) {
        c->hello = (uint8_t *)malloc((size_t)len);
        if (c->hello) {
            for (int i = 0; i < len; i++) c->hello[i] = psp_read8(data + (uint32_t)i);
            c->hello_len = len;
        }
    }
    c->running = 1;
    c->next_hello = 0;
    c->next_ping = now_us() + c->keepalive_us;
    psp_ret(0);
}

static void hle_m_Stop(void) {
    match_context *c = context(psp_arg(0));
    if (!c) {
        psp_ret(ERR_INVALID_ID);
        return;
    }
    for (int i = 0; i < CONTEXT_PEERS; i++)
        if (c->peer[i].used) {
            if (c->peer[i].state != P_KNOWN) match_send(c, T_BYE, c->peer[i].mac, 0, NULL, 0);
            match_drop(&c->peer[i]);
        }
    c->running = 0;
    psp_ret(0);
}

static void hle_m_Delete(void) {
    match_context *c = context(psp_arg(0));
    if (!c) {
        psp_ret(ERR_INVALID_ID);
        return;
    }
    for (int i = 0; i < CONTEXT_PEERS; i++)
        if (c->peer[i].used) match_drop(&c->peer[i]);
    free(c->hello);
    memset(c, 0, sizeof *c);
    psp_ret(0);
}

static void read_mac(uint32_t at, uint8_t *mac) {
    for (int i = 0; i < 6; i++) mac[i] = psp_read8(at + (uint32_t)i);
}

static uint8_t *read_bytes(uint32_t at, int len) {
    if (len <= 0 || !at) return NULL;
    uint8_t *b = (uint8_t *)malloc((size_t)len);
    if (b)
        for (int i = 0; i < len; i++) b[i] = psp_read8(at + (uint32_t)i);
    return b;
}

/* (id, mac, optlen, opt): a client asks a host; a host accepts one who asked. */
static void hle_m_SelectTarget(void) {
    match_context *c = context(psp_arg(0));
    if (!c) {
        psp_ret(ERR_INVALID_ID);
        return;
    }
    if (!c->running) {
        psp_ret(ERR_NOT_RUNNING);
        return;
    }
    uint8_t mac[6];
    read_mac(psp_arg(1), mac);
    match_peer *p = match_find(c, mac);
    if (!p) {
        psp_ret(ERR_UNKNOWN_TARGET);
        return;
    }
    const int len = (int)psp_arg(2);
    uint8_t *opt = read_bytes(psp_arg(3), len);
    uint32_t r = 0;
    if (p->state == P_IN_REQUEST) {
        if (c->maxnum > 1 && established(c) >= c->maxnum - 1) r = ERR_EXCEED_MAXNUM;
        else {
            p->state = P_ACCEPTED;
            match_send(c, T_ACCEPT, mac, 0, opt, opt ? len : 0);
        }
    } else if (p->state == P_KNOWN && c->mode != MODE_HOST) {
        p->state = P_OUT_REQUEST;
        match_send(c, T_REQUEST, mac, 0, opt, opt ? len : 0);
    } else if (p->state == P_OUT_REQUEST) {
        r = ERR_REQUEST_IN_PROGRESS;
    } else if (p->state == P_ESTABLISHED || p->state == P_ACCEPTED) {
        r = ERR_ALREADY_ESTABLISHED;
    } else {
        r = ERR_TARGET_NOT_READY;
    }
    free(opt);
    fprintf(stderr, "adhoc: matching %u chose %s: %08X\n", psp_arg(0), mac_text(mac), r);
    psp_ret(r);
}

/* (id, mac, optlen, opt): turn down, take back a request, or leave. */
static void hle_m_CancelTargetWithOpt(void) {
    match_context *c = context(psp_arg(0));
    if (!c) {
        psp_ret(ERR_INVALID_ID);
        return;
    }
    uint8_t mac[6];
    read_mac(psp_arg(1), mac);
    match_peer *p = match_find(c, mac);
    if (!p) {
        psp_ret(ERR_UNKNOWN_TARGET);
        return;
    }
    const int len = (int)psp_arg(2);
    uint8_t *opt = read_bytes(psp_arg(3), len);
    if (p->state == P_IN_REQUEST) match_send(c, T_DENY, mac, 0, opt, opt ? len : 0);
    else if (p->state != P_KNOWN) match_send(c, T_CANCEL, mac, 0, opt, opt ? len : 0);
    free(opt);
    free(p->out);
    p->out = NULL;
    p->state = P_KNOWN;
    fprintf(stderr, "adhoc: matching %u let go of %s\n", psp_arg(0), mac_text(mac));
    psp_ret(0);
}

/* (id, mac, length, data): to a peer one is matched with, sent again until it says it has it. */
static void hle_m_SendData(void) {
    match_context *c = context(psp_arg(0));
    if (!c) {
        psp_ret(ERR_INVALID_ID);
        return;
    }
    uint8_t mac[6];
    read_mac(psp_arg(1), mac);
    const int len = (int)psp_arg(2);
    match_peer *p = match_find(c, mac);
    if (!p) {
        psp_ret(ERR_UNKNOWN_TARGET);
        return;
    }
    if (p->state != P_ESTABLISHED) {
        psp_ret(ERR_NOT_ESTABLISHED);
        return;
    }
    if (len <= 0 || len > MOST_PAYLOAD) {
        psp_ret(ERR_INVALID_DATALEN);
        return;
    }
    if (p->out) {
        psp_ret(ERR_DATA_BUSY);
        return;
    }
    p->out = read_bytes(psp_arg(3), len);
    if (!p->out) {
        psp_ret(ERR_INVALID_ARG);
        return;
    }
    p->out_len = len;
    p->out_seq++;
    p->resends = 0;
    p->resend_at = now_us() + c->rexmt_us;
    match_send(c, T_DATA, mac, p->out_seq, p->out, len);
    fprintf(stderr, "adhoc: matching %u sends %d bytes to %s\n", psp_arg(0), len, mac_text(mac));
    psp_ret(0);
}

/* ---- where the game's handlers run ------------------------------------------ */

static uint32_t g_guest_buf, g_guest_room;
static int g_delivering;

static uint32_t guest_room(int size) {
    if ((uint32_t)size + 16 <= g_guest_room) return g_guest_buf;
    if (g_guest_buf) psp_sysmem_release(g_guest_buf);
    g_guest_room = ((uint32_t)size + 16 + 0x3FFF) & ~0x3FFFu;
    g_guest_buf = psp_sysmem_alloc(g_guest_room, 1);
    if (!g_guest_buf) g_guest_room = 0;
    return g_guest_buf;
}

void psp_netadhoc_deliver(void) {
    if (!g_ctl_init && !g_pend_head) return;
    if (g_delivering) return;
    const uint64_t now = now_us();
    /* On the network's thread; on another only if that one has not waited for a frame in a while. */
    if (psp_threadman_current() != g_net_thread && now < g_last_delivery + 500000ull) return;
    g_last_delivery = now;
    g_delivering = 1;
    if (g_ctl_init && now >= g_next_beacon) {
        g_next_beacon = now + BEACON_US;
        send_beacon();
    }
    for (packet *pk; (pk = next_packet()) != NULL; free(pk)) {
        if (pk->data[5] == T_BEACON) take_beacon(pk);
        else take_matching(pk);
    }
    for (int i = 0; i < MOST_PEERS; i++)
        if (g_peer[i].used && now > g_peer[i].seen + PEER_GONE_US) {
            fprintf(stderr, "adhoc: %s (\"%s\") is gone\n", mac_text(g_peer[i].mac), g_peer[i].nickname);
            g_peer[i].used = 0;
        }
    if (g_ctl_state == CTL_SCANNING && now >= g_scan_until) {
        g_ctl_state = CTL_DISCONNECTED;
        ctl_event(CTL_EVENT_SCAN, 0);
    }
    match_timers(now);
    /* The handlers, each with the registers it interrupted kept. */
    while (g_pend_head) {
        pending *p = g_pend_head;
        g_pend_head = p->next;
        if (!g_pend_head) g_pend_tail = NULL;
        if (psp_lookup(p->fn)) {
            psp_cpu_state saved = psp_cpu;
            if (p->kind == 0) {
                psp_cpu.r[PSP_REG_A0] = p->a0;
                psp_cpu.r[PSP_REG_A1] = p->a1;
                psp_cpu.r[PSP_REG_A2] = p->a2;
            } else {
                const uint32_t at = guest_room(p->optlen);
                if (at) {
                    for (int i = 0; i < 6; i++) psp_write8(at + (uint32_t)i, p->mac[i]);
                    for (int i = 0; i < p->optlen; i++) psp_write8(at + 16 + (uint32_t)i, p->opt[i]);
                }
                psp_cpu.r[PSP_REG_A0] = p->a0;
                psp_cpu.r[PSP_REG_A1] = p->a1;
                psp_cpu.r[PSP_REG_A2] = at;
                psp_cpu.r[PSP_REG_A3] = (uint32_t)p->optlen;
                psp_cpu.r[PSP_REG_T0] = p->optlen && at ? at + 16 : 0;
                fprintf(stderr, "adhoc: matching %u event %u from %s (%d bytes)\n", p->a0, p->a1, mac_text(p->mac), p->optlen);
            }
            psp_cpu.r[PSP_REG_RA] = 0;
            psp_call_from_host(p->fn);
            psp_cpu = saved;
        } else {
            fprintf(stderr, "adhoc: handler 0x%08X is not a recompiled function\n", p->fn);
        }
        free(p);
    }
    g_delivering = 0;
}

void psp_netadhoc_register(void) {
    psp_hle_register(0x39AF39A6, "sceNet", "sceNetInit", hle_ok);
    psp_hle_register(0x281928A9, "sceNet", "sceNetTerm", hle_ok);
    psp_hle_register(0x89360950, "sceNet", "sceNetEtherNtostr", hle_EtherNtostr);
    psp_hle_register(0x0C622081, "sceWlanDrv", "sceWlanGetEtherAddr", hle_WlanGetEtherAddr);
    psp_hle_register(0xE1D621D7, "sceNetAdhoc", "sceNetAdhocInit", hle_ok);
    psp_hle_register(0xA62C6F57, "sceNetAdhoc", "sceNetAdhocTerm", hle_ok);
    psp_hle_register(0xE26F226E, "sceNetAdhocctl", "sceNetAdhocctlInit", hle_ctl_Init);
    psp_hle_register(0x9D689E13, "sceNetAdhocctl", "sceNetAdhocctlTerm", hle_ctl_Term);
    psp_hle_register(0x20B317A0, "sceNetAdhocctl", "sceNetAdhocctlAddHandler", hle_ctl_AddHandler);
    psp_hle_register(0x6402490B, "sceNetAdhocctl", "sceNetAdhocctlDelHandler", hle_ctl_DelHandler);
    psp_hle_register(0x34401D65, "sceNetAdhocctl", "sceNetAdhocctlDisconnect", hle_ctl_Disconnect);
    psp_hle_register(0x75ECD386, "sceNetAdhocctl", "sceNetAdhocctlGetState", hle_ctl_GetState);
    psp_hle_register(0x08FFF7A0, "sceNetAdhocctl", "sceNetAdhocctlScan", hle_ctl_Scan);
    psp_hle_register(0x81AEE1BE, "sceNetAdhocctl", "sceNetAdhocctlGetScanInfo", hle_ctl_GetScanInfo);
    psp_hle_register(0xE162CB14, "sceNetAdhocctl", "sceNetAdhocctlGetPeerList", hle_ctl_GetPeerList);
    psp_hle_register(0xDED9D28E, "sceNetAdhocctl", "sceNetAdhocctlGetParameter", hle_ctl_GetParameter);
    psp_hle_register(0x2A2A1E07, "sceNetAdhocMatching", "sceNetAdhocMatchingInit", hle_m_Init);
    psp_hle_register(0x7945ECDA, "sceNetAdhocMatching", "sceNetAdhocMatchingTerm", hle_m_Term);
    psp_hle_register(0xCA5EDA6F, "sceNetAdhocMatching", "sceNetAdhocMatchingCreate", hle_m_Create);
    psp_hle_register(0x93EF3843, "sceNetAdhocMatching", "sceNetAdhocMatchingStart", hle_m_Start);
    psp_hle_register(0x32B156B3, "sceNetAdhocMatching", "sceNetAdhocMatchingStop", hle_m_Stop);
    psp_hle_register(0xF16EAF4F, "sceNetAdhocMatching", "sceNetAdhocMatchingDelete", hle_m_Delete);
    psp_hle_register(0x5E3D4B79, "sceNetAdhocMatching", "sceNetAdhocMatchingSelectTarget", hle_m_SelectTarget);
    psp_hle_register(0x8F58BEDF, "sceNetAdhocMatching", "sceNetAdhocMatchingCancelTargetWithOpt", hle_m_CancelTargetWithOpt);
    psp_hle_register(0xF79472D7, "sceNetAdhocMatching", "sceNetAdhocMatchingSendData", hle_m_SendData);
}
