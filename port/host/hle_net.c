/*
 * Network link: NETDRIVER (the game's UDP transport) on real Winsock.
 *
 * On the cabinet, netdriver_Init gives the NE2000 ("ether0") the fixed IP 192.168.200.(10+unit) and
 * sends every datagram from port 1303 to port 2537, with "broadcast" and "multicast" both meaning
 * 255.255.255.255. NET_COMM and NET_LINK above it (unchanged) see only NetAddr entries and raw
 * 32-bit net ids, which are those IPs; NET_LINK also carries the sender's IP in every packet
 * header. See docs/network.md for the protocol.
 *
 * Here the game keeps seeing those virtual IPs; the host maps them to real endpoints:
 *   - one non-blocking UDP socket on INADDR_ANY:(base port + unit), so up to 16 instances fit on a PC
 *   - every datagram carries a small envelope (magic, unit, sequence, per-run nonce). The receiver
 *     drops its own echoes and duplicates (a packet can arrive by broadcast, loopback and the peer
 *     list), learns unit -> sender address, and hands the bare payload to the game
 *   - broadcasts fan out to every adapter's subnet broadcast, to 127.0.0.1 and to the hydro.ini
 *     peer list, each at base port + 0..15; unicasts go to the learned address (or fan out until
 *     the unit has been heard from)
 *
 * Two different machines on the same unit id both send the same virtual IP, which NET_LINK would
 * drop as its own echo; the receiver rewrites the header's IP to a sentinel so the game's own
 * duplicate check ("CONFLICTING UNIT ID") fires, as it would on the cabinets.
 *
 * In a lobby (lobby_client.c) the unit comes from the lobby and every packet goes to the lobby's
 * relay (the master server, or the PC hosting a LAN lobby) through the lobby client's socket,
 * carrying the membership token and the destination unit; the relay forwards it to the members.
 */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <mstcpip.h>

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

#include "hook.h"
#include "host.h"
#include "lobby_client.h"
#include "settings.h"

#include "../common/htnet_proto.h"

#include "hydro_syms.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NET_UNITS HTNET_MAX_UNITS         /* unit ids 0-7 */
#define NET_MAX_PAYLOAD HTNET_MAX_PAYLOAD /* netdriver_Recieve's buffer */
#define NET_VIP_BASE 10       /* unit n is 192.168.200.(10+n) */
#define NET_VIP_DUPLICATE 250 /* another machine with my unit id */
#define NET_MAX_DEST 256
#define NET_MAX_SENDERS 32
#define NET_SEEN 64

typedef HtEnvelope Envelope;

/* The game's address entry (NET_COMM keeps one per unit, plus "all" and "multicast"). */
typedef struct NetAddr {
    uint32_t ip; /* network order */
    struct sockaddr_in *sa;
} NetAddr;

typedef void(__cdecl *RecvCallback)(void *buf, int len);

typedef struct Sender {
    uint32_t nonce; /* 0 = free */
    int unit;
    struct sockaddr_in from;
    uint16_t seen[NET_SEEN]; /* recent sequence numbers */
    uint8_t seen_valid[NET_SEEN];
    unsigned seen_pos;
    DWORD last;
} Sender;

static struct {
    int up;
    int relay; /* in a lobby: everything goes through the lobby's relay */
    SOCKET sock;
    int unit;
    uint32_t vip; /* my virtual IP */
    uint32_t nonce;
    uint16_t seq;
    int log;
    RecvCallback recv_cb;
    struct sockaddr_in slots[NET_UNITS + 2]; /* what the game's NetAddr.sa point at (like the original's pool) */
    int slots_used;
    struct sockaddr_in fanout[NET_MAX_DEST]; /* broadcast destinations */
    int fanout_count;
    Sender senders[NET_MAX_SENDERS];
    int peer[NET_UNITS]; /* unit -> senders[] index of the last sender with that id, -1 = unknown */
    int delivered;       /* relay: packets handed to the game by this netdriver_Recieve */
} g_net;

static uint32_t vip_of(int unit)
{
    return htonl(0xc0a8c800u | (uint32_t)(NET_VIP_BASE + unit)); /* 192.168.200.x */
}

static int unit_of_vip(uint32_t ip)
{
    uint32_t h = ntohl(ip);
    if ((h & 0xffffff00u) != 0xc0a8c800u)
        return -1;
    int u = (int)(h & 0xff) - NET_VIP_BASE;
    return u >= 0 && u < NET_UNITS ? u : -1;
}

static const char *addr_str(const struct sockaddr_in *a)
{
    static char buf[4][32];
    static int n;
    char *b = buf[n++ & 3];
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &a->sin_addr, ip, sizeof ip);
    snprintf(b, 32, "%s:%u", ip, ntohs(a->sin_port));
    return b;
}

/* ---------------------------------------------------------------- destinations */

static void fanout_add(uint32_t ip, int port)
{
    if (g_net.fanout_count >= NET_MAX_DEST)
        return;
    for (int i = 0; i < g_net.fanout_count; i++)
        if (g_net.fanout[i].sin_addr.s_addr == ip && g_net.fanout[i].sin_port == htons((u_short)port))
            return;
    struct sockaddr_in *a = &g_net.fanout[g_net.fanout_count++];
    memset(a, 0, sizeof *a);
    a->sin_family = AF_INET;
    a->sin_addr.s_addr = ip;
    a->sin_port = htons((u_short)port);
}

/* Every other unit's port on `ip` (a host without an explicit port may run any unit). */
static void fanout_add_units(uint32_t ip, int is_self_host)
{
    for (int k = 0; k < NET_UNITS; k++)
        if (!is_self_host || k != g_net.unit)
            fanout_add(ip, g_settings.net.base_port + k);
}

/* The subnet broadcast of every IPv4 adapter that's up (255.255.255.255 leaves by one adapter only). */
static void fanout_add_lan(void)
{
    uint32_t bcast[16];
    int n = net_lan_broadcasts(bcast, 16);
    for (int i = 0; i < n; i++) {
        struct in_addr in = {.s_addr = bcast[i]};
        char s[INET_ADDRSTRLEN];
        hy_log("net: LAN broadcast %s", inet_ntop(AF_INET, &in, s, sizeof s));
        fanout_add_units(bcast[i], 1);
    }
}

/* hydro.ini [Network] peers: "host[:port], ..." */
static void fanout_add_peers(void)
{
    char list[sizeof g_settings.net.peers];
    snprintf(list, sizeof list, "%s", g_settings.net.peers);
    char *save, *tok = strtok_s(list, ",; ", &save);
    for (; tok; tok = strtok_s(NULL, ",; ", &save)) {
        int port = 0;
        char *colon = strrchr(tok, ':');
        if (colon) {
            *colon = 0;
            port = atoi(colon + 1);
        }
        struct addrinfo hints = {0}, *res = NULL;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        if (getaddrinfo(tok, NULL, &hints, &res) || !res) {
            hy_log("net: peer %s: can't resolve", tok);
            continue;
        }
        uint32_t ip = ((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr;
        freeaddrinfo(res);
        if (port > 0 && port < 65536)
            fanout_add(ip, port);
        else
            fanout_add_units(ip, 0); /* our own nonce filters the copy sent to ourselves */
        hy_log("net: peer %s%s%s", tok, colon ? ":" : "", colon ? colon + 1 : "");
    }
}

/* ---------------------------------------------------------------- send / receive */

static int send_to(const void *buf, int len, const struct sockaddr_in *dst)
{
    return sendto(g_net.sock, buf, len, 0, (const struct sockaddr *)dst, sizeof *dst) != SOCKET_ERROR;
}

static void log_packet(const char *dir, const uint8_t *p, int len, const char *where)
{
    if (!g_net.log || len < 8)
        return;
    unsigned type = *(const uint16_t *)(p + 2);
    char extra[64] = "";
    if (type <= 1 && len >= 0x18) /* HELLO / HELLO_ACK: code and data checksums, "connected" */
        snprintf(extra, sizeof extra, " code %08x data %08x conn %d", *(const uint32_t *)(p + 0xc),
                 *(const uint32_t *)(p + 0x10), *(const int32_t *)(p + 0x14));
    else if (type == 18 && len >= 0xc) /* error: code, unit */
        snprintf(extra, sizeof extra, " error %u unit %u", *(const uint16_t *)(p + 8), *(const uint16_t *)(p + 0xa));
    hy_log("net: %s type %2u unit %u len %3d %s%s", dir, type, *(const uint16_t *)p, len, where, extra);
}

/* ip == INADDR_BROADCAST or an unknown unit: fan out. Returns 1 if anything went out. */
static int transmit(const void *payload, int len, uint32_t ip)
{
    if (!g_net.up || len < 0 || len > NET_MAX_PAYLOAD)
        return 0;
    uint8_t pkt[sizeof(Envelope) + NET_MAX_PAYLOAD];
    Envelope *e = (Envelope *)pkt;
    memset(e, 0, sizeof *e);
    e->magic = HTNET_MAGIC;
    e->version = HTNET_VERSION;
    e->unit = (uint8_t)g_net.unit;
    e->seq = g_net.seq++;
    e->nonce = g_net.nonce;
    memcpy(pkt + sizeof *e, payload, (size_t)len);
    int n = (int)sizeof *e + len;

    int unit = ip == INADDR_BROADCAST ? -1 : unit_of_vip(ip);
    e->dest = unit >= 0 ? (uint8_t)unit : HTNET_DEST_ALL;
    if (g_net.relay) {
        e->token = lobby_token();
        log_packet("tx", payload, len, unit >= 0 ? "(relay, unicast)" : "(relay)");
        return lobby_send_data(pkt, n);
    }
    if (unit >= 0 && g_net.peer[unit] >= 0) {
        const struct sockaddr_in *to = &g_net.senders[g_net.peer[unit]].from;
        log_packet("tx", payload, len, addr_str(to));
        return send_to(pkt, n, to);
    }
    log_packet("tx", payload, len, "(all)");
    int ok = 0;
    for (int i = 0; i < g_net.fanout_count; i++)
        ok |= send_to(pkt, n, &g_net.fanout[i]);
    return ok;
}

/* Sender bookkeeping; returns 0 for a duplicate. */
static int accept_packet(const Envelope *e, const struct sockaddr_in *from)
{
    Sender *s = NULL, *oldest = &g_net.senders[0];
    for (int i = 0; i < NET_MAX_SENDERS; i++) {
        Sender *c = &g_net.senders[i];
        if (c->nonce == e->nonce) {
            s = c;
            break;
        }
        if (!c->nonce || (oldest->nonce && c->last < oldest->last))
            oldest = c;
    }
    int is_new = !s;
    if (is_new) {
        s = oldest;
        for (int u = 0; u < NET_UNITS; u++)
            if (g_net.peer[u] == (int)(s - g_net.senders))
                g_net.peer[u] = -1;
        memset(s, 0, sizeof *s);
        s->nonce = e->nonce;
    }
    for (int i = 0; i < NET_SEEN; i++)
        if (s->seen_valid[i] && s->seen[i] == e->seq)
            return 0;
    s->seen[s->seen_pos] = e->seq;
    s->seen_valid[s->seen_pos] = 1;
    s->seen_pos = (s->seen_pos + 1) % NET_SEEN;
    s->last = GetTickCount();

    /* The same instance can reach us by several routes (loopback, LAN, a listed peer): answer
     * through the first one it used, for as long as it runs (a restart brings a new nonce). */
    if (is_new) {
        s->from = *from;
        s->unit = e->unit;
    }
    int idx = (int)(s - g_net.senders);
    if (e->unit < NET_UNITS && e->unit != g_net.unit && g_net.peer[e->unit] != idx) {
        g_net.peer[e->unit] = idx;
        hy_log("net: unit %u is at %s", e->unit + 1, addr_str(&s->from));
    } else if (is_new && e->unit == g_net.unit) {
        hy_log("net: another machine at %s uses my unit id %u", addr_str(from), e->unit + 1);
    }
    return 1;
}

/* ---------------------------------------------------------------- code checksum
 * NET_LINK's HELLO carries gameinit_GetCodeChecksum, and units whose values differ refuse to link.
 * main() sums the image's CODE, CONST and data-init stream (ranges at 0x2a9a8c), but here that runs
 * after the host has patched CODE with jumps to per-process thunks, so two instances never match.
 * Take the same sum over the pristine image instead: it's the value the cabinets computed. */
static uint32_t g_code_checksum;

void hle_net_capture_checksum(void)
{
    uint32_t total = 0;
    for (const uint32_t *r = (const uint32_t *)(uintptr_t)0x002a9a8cu; r[1]; r += 2) {
        const uint32_t *p = (const uint32_t *)(uintptr_t)((r[0] + 3) & ~3u);
        uint32_t n = ((r[1] & ~7u) - (uint32_t)(uintptr_t)p + 4) >> 2; /* sic: the original's rounding */
        uint32_t sum = 0;
        while (n--)
            sum += *p++;
        total += sum;
    }
    g_code_checksum = total;
}

static uint32_t __cdecl my_gameinit_GetCodeChecksum(void)
{
    return g_code_checksum;
}

/* ---------------------------------------------------------------- relay (lobby) */

/* A game packet from the lobby's relay, which has set the sender's unit. */
static void on_relay_data(uint8_t *pkt, int n, const struct sockaddr_in *from)
{
    const Envelope *e = (const Envelope *)pkt;
    if (!g_net.up || !g_net.relay || n < (int)sizeof *e || e->magic != HTNET_MAGIC || e->version != HTNET_VERSION ||
        e->nonce == g_net.nonce || e->unit >= NET_UNITS || e->unit == g_net.unit)
        return;
    if (!accept_packet(e, from))
        return;
    uint8_t *payload = pkt + sizeof *e;
    int len = n - (int)sizeof *e;
    if (len > NET_MAX_PAYLOAD)
        len = NET_MAX_PAYLOAD;
    log_packet("rx", payload, len, "(relay)");
    g_net.recv_cb(payload, len);
    g_net.delivered++;
}

/* ---------------------------------------------------------------- NETDRIVER */

static void net_close(void)
{
    if (g_net.relay)
        lobby_set_data_handler(NULL); /* the socket is the lobby client's */
    if (g_net.sock != INVALID_SOCKET)
        closesocket(g_net.sock);
    if (g_net.up)
        WSACleanup();
    memset(&g_net, 0, sizeof g_net);
    g_net.sock = INVALID_SOCKET;
}

static void set_addr(NetAddr *a, uint32_t ip)
{
    a->ip = ip;
    if (!a->sa && g_net.slots_used < NET_UNITS + 2)
        a->sa = &g_net.slots[g_net.slots_used++];
    if (a->sa) {
        memset(a->sa, 0, sizeof *a->sa);
        a->sa->sin_family = AF_INET;
        a->sa->sin_addr.s_addr = ip;
        a->sa->sin_port = htons(2537);
    }
}

static int __cdecl my_netdriver_Init(RecvCallback cb, NetAddr *local, NetAddr *mcast, NetAddr *all)
{
    net_close();
    if (!cb || !local || !mcast || !all)
        return 0;
    local->sa = mcast->sa = all->sa = NULL;
    local->ip = mcast->ip = all->ip = 0;

    int unit = HY_GLOBAL(int, OperatorSettings); /* UNIT ID in NETWORK ADJUSTMENTS, stored 0-based */
    int relay = lobby_active(&unit);             /* ...or the lobby's (operator_ModuleInit put it there too) */
    if (unit < 0 || unit >= NET_UNITS) {
        hy_log("net: unit id %d out of range", unit);
        return 0;
    }

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) {
        hy_log("net: WSAStartup failed");
        return 0;
    }
    g_net.up = 1;
    g_net.unit = unit;
    g_net.vip = vip_of(unit);
    for (int u = 0; u < NET_UNITS; u++)
        g_net.peer[u] = -1;
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    g_net.nonce = (uint32_t)(qpc.QuadPart ^ (qpc.QuadPart >> 32)) ^ (GetCurrentProcessId() << 16) | 1u;
    g_net.log = getenv("HYDRO_NET_LOG") && atoi(getenv("HYDRO_NET_LOG"));

    if (relay) {
        g_net.relay = 1;
        lobby_set_data_handler(on_relay_data);
        set_addr(local, g_net.vip);
        set_addr(mcast, INADDR_BROADCAST);
        set_addr(all, INADDR_BROADCAST);
        g_net.recv_cb = cb;
        hy_log("net: unit %d in %s lobby '%s' (relayed)", unit + 1, lobby_is_lan() ? "LAN" : "NET", lobby_name());
        return 1;
    }

    int port = g_settings.net.base_port + unit;
    g_net.sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    BOOL on = TRUE;
    u_long nonblock = 1;
    struct sockaddr_in me = {0};
    me.sin_family = AF_INET;
    me.sin_port = htons((u_short)port);
    me.sin_addr.s_addr = INADDR_ANY;
    /* No SO_REUSEADDR: a second instance on the same unit must fail here, not share the port. */
    if (g_net.sock == INVALID_SOCKET ||
        setsockopt(g_net.sock, SOL_SOCKET, SO_BROADCAST, (const char *)&on, sizeof on) ||
        ioctlsocket(g_net.sock, FIONBIO, &nonblock) || bind(g_net.sock, (struct sockaddr *)&me, sizeof me)) {
        hy_log("net: can't open UDP port %d (error %d)", port, WSAGetLastError());
        net_close();
        return 0;
    }
    /* Windows reports an ICMP port-unreachable from an earlier sendto as WSAECONNRESET on the next
     * recvfrom; peers that aren't running yet would stop the receive loop. */
    BOOL off = FALSE;
    DWORD ret;
    WSAIoctl(g_net.sock, SIO_UDP_CONNRESET, &off, sizeof off, NULL, 0, &ret, NULL, NULL);

    if (g_settings.net.lan_broadcast)
        fanout_add_lan();
    if (g_settings.net.local_instances)
        fanout_add_units(htonl(INADDR_LOOPBACK), 1);
    fanout_add_peers();

    set_addr(local, g_net.vip);
    set_addr(mcast, INADDR_BROADCAST);
    set_addr(all, INADDR_BROADCAST);
    g_net.recv_cb = cb;
    hy_log("net: unit %d on UDP port %d, %d broadcast destinations", unit + 1, port, g_net.fanout_count);
    return 1;
}

static void __cdecl my_netdriver_Shutdown(void)
{
    if (g_net.up)
        hy_log("net: shutdown");
    net_close();
}

static int __cdecl my_netdriver_GetAddressFromNetId(uint32_t ip, NetAddr *out)
{
    if (!out || (!out->sa && g_net.slots_used >= NET_UNITS + 2))
        return 0;
    set_addr(out, ip);
    return 1;
}

static int __cdecl my_netdriver_EnableMulticasting(int enable)
{
    (void)enable;
    return 1;
}

static int __cdecl my_netdriver_Send(void *buf, int len, NetAddr *dst)
{
    return dst && transmit(buf, len, dst->ip);
}

static int __cdecl my_netdriver_SendToId(void *buf, int len, uint32_t ip)
{
    return transmit(buf, len, ip);
}

/* Delivers every pending datagram to the game; returns the count, -1 on a socket error. */
static int __cdecl my_netdriver_Recieve(void)
{
    if (!g_net.up)
        return -1;
    if (g_net.relay) {
        g_net.delivered = 0;
        lobby_poll(); /* hands game packets to on_relay_data; keeps the membership alive */
        return g_net.delivered;
    }
    int count = 0;
    for (;;) {
        uint8_t pkt[sizeof(Envelope) + NET_MAX_PAYLOAD + 64];
        struct sockaddr_in from;
        int fromlen = sizeof from;
        int n = recvfrom(g_net.sock, (char *)pkt, sizeof pkt, 0, (struct sockaddr *)&from, &fromlen);
        if (n == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK)
                return count;
            if (err == WSAEMSGSIZE || err == WSAECONNRESET)
                continue;
            hy_log("net: recvfrom error %d", err);
            return -1;
        }
        const Envelope *e = (const Envelope *)pkt;
        if (n < (int)sizeof *e || e->magic != HTNET_MAGIC || e->version != HTNET_VERSION || e->nonce == g_net.nonce ||
            (e->dest != HTNET_DEST_ALL && e->dest != g_net.unit))
            continue;
        if (!accept_packet(e, &from))
            continue;
        uint8_t *payload = pkt + sizeof *e;
        int len = n - (int)sizeof *e;
        if (len > NET_MAX_PAYLOAD)
            len = NET_MAX_PAYLOAD;
        /* NET_LINK header: +0 u16 unit, +2 u16 type, +4 u32 sender IP */
        if (e->unit == g_net.unit && len >= 8)
            *(uint32_t *)(payload + 4) = htonl(0xc0a8c800u | NET_VIP_DUPLICATE);
        log_packet("rx", payload, len, addr_str(&from));
        g_net.recv_cb(payload, len);
        count++;
    }
}

/* The race's input delay in frames (game/net_link.c, "input delay"): the lobby's, or without a
 * lobby [Network] input_delay. Every unit of a race must use the same. */
int __cdecl hle_net_input_delay(void)
{
    int d = lobby_active(NULL) ? lobby_input_delay() : g_settings.net.input_delay;
    if (g_net.log || d > 1)
        hy_log("net: input delay %d frame%s", d, d == 1 ? "" : "s");
    return d;
}

void net_link_install_units(void); /* game/net_link.c */
void operator_install(void);       /* game/operator.c: lobby game options stay out of the CMOS */

void hle_net_install(void)
{
    g_net.sock = INVALID_SOCKET;
    net_link_install_units();
    operator_install();
    hook_name("netdriver_Init", my_netdriver_Init);
    hook_name("netdriver_Shutdown", my_netdriver_Shutdown);
    hook_name("netdriver_GetAddressFromNetId", my_netdriver_GetAddressFromNetId);
    hook_name("netdriver_EnableMulticasting", my_netdriver_EnableMulticasting);
    hook_name("netdriver_Send", my_netdriver_Send);
    hook_name("netdriver_SendToId", my_netdriver_SendToId);
    hook_name("netdriver_Recieve", my_netdriver_Recieve);
    hook_name("gameinit_GetCodeChecksum", my_gameinit_GetCodeChecksum);
    hy_log("net: code checksum %08x", g_code_checksum);
}
