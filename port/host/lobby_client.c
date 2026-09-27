/*
 * Lobby client: LAN / master browsing, create, join (password challenge), keepalive, members.
 * See lobby_client.h and docs/network.md ("Lobbies"). A PC that hosts a LAN lobby runs the same
 * server as htmaster (server/lobby.c, LOBBY_LAN) on a thread and joins it over 127.0.0.1.
 */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <mstcpip.h>
#include <windns.h>

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

#include "lobby_client.h"

#include "host.h"
#include "settings.h"

#include "../common/sha256.h"
#include "../server/lobby.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RETRY_MS 500
#define RETRIES 10

enum { REQ_NONE, REQ_CREATE, REQ_CHALLENGE, REQ_JOIN, REQ_PROBE };

static struct {
    int init;
    SOCKET sock;
    LobbyDataHandler data;

    LobbyEntry list[LOBBY_MAX_LIST];
    int count;
    char challenge[12];
    ULONGLONG browse_t0;

    int state;
    char msg[64];

    /* the request in flight */
    int req;
    char nonce[12];
    struct sockaddr_in relay;
    uint32_t lobby_id;
    int lan, direct;
    char relay_host[160]; /* the DNS name the relay was reached by ("" = an address), for \host\ */
    LobbyEntry found;     /* DIRECT CONNECT: the lobby that answered (LOBBY_NEEDPASS) */
    char verifier[65];
    char text[768]; /* what gets retransmitted */
    ULONGLONG next_send;
    int tries;

    /* membership */
    uint32_t token;
    int unit;
    char name[HTNET_LOBBY_LEN];
    int max;
    int delay;
    int has_opts; /* the lobby sent game options (a server from before them doesn't) */
    HtGameOpts opts;
    char names[HTNET_MAX_UNITS][HTNET_NAME_LEN];
    unsigned mask;
    int ver;
    ULONGLONG last_ack, next_keepalive;
    unsigned ka_seq;

    /* START GAME: the lobby's owner bumps the lobby's start number, every member follows it */
    int owner;         /* -1 = unknown (a server from before START GAME) */
    int start;         /* the lobby's start number as last seen, -1 = none yet */
    int start_want;    /* our startGame in flight (0 = none) */
    int start_tries;
    ULONGLONG start_next;
    int start_pending; /* another unit started the game: the operator menu should leave */

    /* chat: the lines shown (oldest first), the server's last line id we have, and our own lines
     * waiting for sayAck (one in flight, resent like requests) */
    LobbyChatLine chat[LOBBY_CHAT_LINES];
    int chat_n;
    unsigned chat_ver;
    uint32_t chat_id;
    ULONGLONG getchat_next;
    char say_q[LOBBY_SAY_QUEUE][HTNET_CHAT_LEN];
    int say_n, say_tries;
    unsigned say_seq;
    ULONGLONG say_next;
    int members_seen; /* joined/left notices start with the second members */

    /* LAN hosting */
    LobbyServer *server;
    HANDLE thread;
    volatile LONG stop;
} g;

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

static void set_msg(const char *m)
{
    snprintf(g.msg, sizeof g.msg, "%s", m);
    hy_log("lobby: %s", m);
}

static void make_addr(struct sockaddr_in *a, uint32_t ip, int port)
{
    memset(a, 0, sizeof *a);
    a->sin_family = AF_INET;
    a->sin_addr.s_addr = ip;
    a->sin_port = htons((u_short)port);
}

/* ---------------------------------------------------------------- name resolution */

/* SRV _hydrothunder._udp.<name>: the target host and port, like Minecraft's _minecraft._tcp, so one
 * address can carry several servers on their own ports. Lowest priority wins, then a pick by weight. */
static int srv_lookup(const char *name, char *target, size_t cap, int *port)
{
    char q[300];
    snprintf(q, sizeof q, HTNET_SRV_PREFIX "%s", name);
    DNS_RECORDA *list = NULL;
    if (DnsQuery_A(q, DNS_TYPE_SRV, DNS_QUERY_STANDARD, NULL, (PDNS_RECORD *)&list, NULL) != 0 || !list)
        return 0;
    const DNS_SRV_DATAA *best[16];
    int n = 0, prio = 0x10000;
    for (DNS_RECORDA *r = list; r; r = r->pNext) {
        if (r->wType != DNS_TYPE_SRV || r->Flags.S.Section != DnsSectionAnswer || !r->Data.SRV.pNameTarget)
            continue;
        const DNS_SRV_DATAA *d = &r->Data.SRV;
        if (d->wPriority < prio)
            prio = d->wPriority, n = 0;
        if (d->wPriority == prio && n < 16)
            best[n++] = d;
    }
    int ok = 0;
    if (n) {
        unsigned total = 0;
        for (int i = 0; i < n; i++)
            total += best[i]->wWeight;
        unsigned pick = total ? ht_random32() % total : 0;
        int i = 0;
        while (i < n - 1 && pick >= best[i]->wWeight)
            pick -= best[i++]->wWeight;
        /* target "." means "no such service here" (RFC 2782) */
        if (strcmp(best[i]->pNameTarget, ".") && best[i]->pNameTarget[0] && best[i]->wPort) {
            snprintf(target, cap, "%s", best[i]->pNameTarget);
            *port = best[i]->wPort;
            ok = 1;
        }
    }
    DnsRecordListFree((PDNS_RECORD)list, DnsFreeRecordList);
    return ok;
}

/* "host[:port]". A DNS name without a port tries SRV first, then the name itself on `def_port`.
 * `name` (may be NULL) gets the DNS name as typed ("" for an IP address), for \host\. Answers are
 * cached for a while, since FIND LOBBIES asks the master every 3 s from the game thread. */
static int resolve(const char *spec, int def_port, struct sockaddr_in *out, char *name, size_t name_cap)
{
    static struct {
        char spec[160];
        int def_port, ok;
        struct sockaddr_in addr;
        char name[160];
        ULONGLONG until;
    } cache[4];
    static int next;
    ULONGLONG now = GetTickCount64();
    for (int i = 0; i < 4; i++)
        if (cache[i].until > now && cache[i].def_port == def_port && !strcmp(cache[i].spec, spec)) {
            *out = cache[i].addr;
            if (name)
                snprintf(name, name_cap, "%s", cache[i].name);
            return cache[i].ok;
        }

    char host[160], target[256] = "", dns[160] = "";
    snprintf(host, sizeof host, "%s", spec);
    char *p = host;
    while (*p == ' ')
        p++;
    for (char *e = p + strlen(p); e > p && e[-1] == ' ';)
        *--e = 0;
    int port = def_port, ok = 0;
    const char *how = "";
    memset(out, 0, sizeof *out);
    if (*p) {
        char *colon = strrchr(p, ':');
        if (colon) {
            *colon = 0;
            port = atoi(colon + 1);
        }
        struct in_addr lit;
        if (inet_pton(AF_INET, p, &lit) == 1) {
            make_addr(out, lit.s_addr, port);
            ok = 1;
        } else {
            snprintf(dns, sizeof dns, "%s", p);
            const char *h = p;
            if (!colon && srv_lookup(p, target, sizeof target, &port)) {
                h = target;
                how = " (SRV)";
            }
            struct addrinfo hints = {0}, *res = NULL;
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_DGRAM;
            if (!getaddrinfo(h, NULL, &hints, &res) && res) {
                make_addr(out, ((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr, port);
                freeaddrinfo(res);
                ok = 1;
            }
        }
        ok = ok && port > 0 && port < 65536;
        if (!ok)
            hy_log("lobby: can't resolve '%s'", p);
        else if (dns[0])
            hy_log("lobby: %s is %s%s%s%s", p, addr_str(out), how, target[0] ? " via " : "", target);
    }
    int i = next++ & 3;
    snprintf(cache[i].spec, sizeof cache[i].spec, "%s", spec);
    cache[i].def_port = def_port;
    cache[i].ok = ok;
    cache[i].addr = *out;
    snprintf(cache[i].name, sizeof cache[i].name, "%s", dns);
    cache[i].until = now + (ok ? 5 * 60 * 1000 : 5000);
    if (name)
        snprintf(name, name_cap, "%s", dns);
    return ok;
}

int net_lan_broadcasts(uint32_t *out, int max)
{
    ULONG size = 16384;
    IP_ADAPTER_ADDRESSES *list = NULL;
    ULONG r = ERROR_BUFFER_OVERFLOW;
    for (int tries = 0; tries < 3 && r == ERROR_BUFFER_OVERFLOW; tries++) {
        free(list);
        list = malloc(size);
        if (!list)
            return 0;
        r = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                 NULL, list, &size);
    }
    int n = 0;
    if (r == NO_ERROR) {
        for (IP_ADAPTER_ADDRESSES *a = list; a; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
                continue;
            for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u && n < max; u = u->Next) {
                if (u->Address.lpSockaddr->sa_family != AF_INET || u->OnLinkPrefixLength > 30)
                    continue;
                uint32_t ip = ntohl(((struct sockaddr_in *)u->Address.lpSockaddr)->sin_addr.s_addr);
                uint32_t mask = u->OnLinkPrefixLength ? ~0u << (32 - u->OnLinkPrefixLength) : 0;
                out[n++] = htonl(ip | ~mask);
            }
        }
    }
    free(list);
    if (!n && max > 0)
        out[n++] = INADDR_BROADCAST;
    return n;
}

/* ---------------------------------------------------------------- socket */

static int open_socket(void)
{
    if (g.sock != INVALID_SOCKET)
        return 1;
    g.sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    BOOL on = TRUE, off = FALSE;
    u_long nonblock = 1;
    struct sockaddr_in me;
    make_addr(&me, INADDR_ANY, 0);
    DWORD ret;
    if (g.sock == INVALID_SOCKET ||
        setsockopt(g.sock, SOL_SOCKET, SO_BROADCAST, (const char *)&on, sizeof on) ||
        ioctlsocket(g.sock, FIONBIO, &nonblock) || bind(g.sock, (struct sockaddr *)&me, sizeof me)) {
        hy_log("lobby: can't open a UDP socket (error %d)", WSAGetLastError());
        if (g.sock != INVALID_SOCKET)
            closesocket(g.sock);
        g.sock = INVALID_SOCKET;
        return 0;
    }
    WSAIoctl(g.sock, SIO_UDP_CONNRESET, &off, sizeof off, NULL, 0, &ret, NULL, NULL);
    return 1;
}

static void send_text(const struct sockaddr_in *to, const char *text)
{
    uint8_t pkt[HTNET_MAX_DATAGRAM];
    int n = ht_oob_build(pkt, sizeof pkt, text);
    sendto(g.sock, (const char *)pkt, n, 0, (const struct sockaddr *)to, sizeof *to);
}

int lobby_send_data(const void *pkt, int n)
{
    if (g.state != LOBBY_IN || g.sock == INVALID_SOCKET)
        return 0;
    return sendto(g.sock, pkt, n, 0, (const struct sockaddr *)&g.relay, sizeof g.relay) != SOCKET_ERROR;
}

/* ---------------------------------------------------------------- LAN hosting */

static DWORD WINAPI server_thread(void *param)
{
    (void)param;
    while (!g.stop)
        lobby_server_poll(g.server, 50);
    return 0;
}

static int lan_server_start(void)
{
    if (g.server)
        return lobby_server_port(g.server);
    g.server = lobby_server_open(LOBBY_LAN, g_settings.net.lan_port, HTNET_LAN_PORTS, 1, hy_log);
    if (!g.server)
        return 0;
    g.stop = 0;
    g.thread = CreateThread(NULL, 0, server_thread, NULL, 0, NULL);
    return lobby_server_port(g.server);
}

static void lan_server_stop(void)
{
    if (!g.server)
        return;
    g.stop = 1;
    WaitForSingleObject(g.thread, 2000);
    CloseHandle(g.thread);
    lobby_server_close(g.server); /* tells the members */
    g.server = NULL;
    g.thread = NULL;
}

/* ---------------------------------------------------------------- membership */

static void chat_add(int unit, const char *name, const char *text)
{
    if (g.chat_n == LOBBY_CHAT_LINES)
        memmove(g.chat, g.chat + 1, sizeof g.chat - sizeof *g.chat);
    else
        g.chat_n++;
    LobbyChatLine *c = &g.chat[g.chat_n - 1];
    c->unit = unit;
    snprintf(c->name, sizeof c->name, "%s", name ? name : "");
    snprintf(c->text, sizeof c->text, "%s", text);
    g.chat_ver++;
    if (unit >= 0)
        hy_log("lobby: <%s> %s", c->name, c->text);
}

static void chat_notice(const char *fmt, ...)
{
    char text[sizeof g.chat[0].text];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    chat_add(-1, NULL, text);
}

static void drop_membership(void)
{
    g.token = 0;
    g.mask = 0;
    memset(g.names, 0, sizeof g.names);
    g.owner = g.start = -1;
    g.start_want = g.start_pending = 0;
    g.say_n = 0;
    g.members_seen = 0;
}

static void fail(const char *why)
{
    g.req = REQ_NONE;
    drop_membership();
    if (g.server)
        lan_server_stop();
    g.state = LOBBY_FAILED;
    set_msg(why);
}

static void send_request(void)
{
    send_text(&g.relay, g.text);
    g.next_send = GetTickCount64() + RETRY_MS;
    g.tries++;
}

int lobby_delay_for(int lan, int setting)
{
    return setting >= 1 && setting <= 6 ? setting : lan ? 1 : 3;
}

void lobby_create(int lan, const char *name, int max, const char *password, int delay)
{
    lobby_leave();
    if (!open_socket()) {
        fail("No network");
        return;
    }
    g.lan = lan;
    g.direct = 0;
    g.relay_host[0] = 0;
    if (lan) {
        int port = lan_server_start();
        if (!port) {
            fail("Can't host: LAN lobby ports are busy");
            return;
        }
        make_addr(&g.relay, htonl(INADDR_LOOPBACK), port);
    } else if (!resolve(g_settings.net.master, HTNET_MASTER_PORT, &g.relay, g.relay_host, sizeof g.relay_host)) {
        fail(g_settings.net.master[0] ? "Can't find the MASTER SERVER" : "Set the MASTER SERVER first");
        return;
    }
    char verifier[65] = "";
    if (password && *password)
        sha256_hex2("HydroThunder:", password, verifier);
    if (max > LOBBY_GAME_UNITS)
        max = LOBBY_GAME_UNITS;
    char info[512] = "";
    ht_info_set(info, sizeof info, "hostname", name);
    ht_info_set_int(info, sizeof info, "sv_maxclients", max);
    ht_info_set_int(info, sizeof info, "delay", lobby_delay_for(lan, delay));
    ht_info_set(info, sizeof info, "verifier", verifier);
    ht_info_set_int(info, sizeof info, "protocol", HTNET_PROTOCOL);
    ht_info_set(info, sizeof info, "name", g_settings.net.player_name);
    char opts[192];
    ht_gameopts_encode(&g_settings.net.lobby_opts, opts, sizeof opts);
    ht_info_set(info, sizeof info, "gopts", opts);
    if (g.relay_host[0])
        ht_info_set(info, sizeof info, "host", g.relay_host);
    snprintf(g.nonce, sizeof g.nonce, "%08x", ht_random32());
    snprintf(g.text, sizeof g.text, "createLobby %s\n%s", g.nonce, info);
    g.req = REQ_CREATE;
    g.tries = 0;
    g.state = LOBBY_BUSY;
    set_msg(lan ? "Creating the LAN lobby..." : "Creating the lobby on the master...");
    send_request();
}

void lobby_join(const LobbyEntry *e, const char *password)
{
    lobby_leave();
    if (!open_socket()) {
        fail("No network");
        return;
    }
    g.lan = e->lan;
    g.direct = e->direct;
    g.relay = e->relay;
    snprintf(g.relay_host, sizeof g.relay_host, "%s", e->host);
    g.lobby_id = e->id;
    g.verifier[0] = 0;
    if (password && *password)
        sha256_hex2("HydroThunder:", password, g.verifier);
    snprintf(g.nonce, sizeof g.nonce, "%08x", ht_random32());
    char hinfo[200] = "";
    if (g.relay_host[0])
        ht_info_set(hinfo, sizeof hinfo, "host", g.relay_host);
    snprintf(g.text, sizeof g.text, "join %u %s%s%s", e->id, g.nonce, hinfo[0] ? "\n" : "", hinfo);
    g.req = REQ_CHALLENGE;
    g.tries = 0;
    g.state = LOBBY_BUSY;
    char m[64];
    snprintf(m, sizeof m, "Joining %s...", e->name);
    set_msg(m);
    send_request();
}

void lobby_leave(void)
{
    if (g.token) {
        char text[32];
        snprintf(text, sizeof text, "leave %08x", g.token);
        send_text(&g.relay, text);
        send_text(&g.relay, text);
        hy_log("lobby: left '%s'", g.name);
    }
    g.req = REQ_NONE;
    drop_membership();
    lan_server_stop();
    if (g.state != LOBBY_IDLE) {
        g.state = LOBBY_IDLE;
        g.msg[0] = 0;
    }
}

static void joined(const char *info)
{
    char v[32];
    ht_info_get(info, "token", v, sizeof v);
    g.token = (uint32_t)strtoul(v, NULL, 16);
    g.unit = ht_info_get_int(info, "unit", 0);
    g.lobby_id = (uint32_t)ht_info_get_int(info, "lobby", 0);
    g.max = ht_info_get_int(info, "sv_maxclients", HTNET_MAX_UNITS);
    g.delay = ht_info_get_int(info, "delay", 1);
    ht_info_get(info, "hostname", g.name, sizeof g.name);
    char opts[192];
    ht_info_get(info, "gopts", opts, sizeof opts);
    g.has_opts = ht_gameopts_decode(opts, &g.opts);
    if (g.has_opts)
        hy_log("lobby: game options %s", opts);
    else
        hy_log("lobby: no game options from the lobby (older server?): the cabinet's own apply");
    g.req = REQ_NONE;
    g.state = LOBBY_IN;
    g.ver = -1;
    g.chat_n = 0;
    g.chat_id = 0;
    g.chat_ver++;
    g.say_n = 0;
    g.say_seq = 1;
    g.members_seen = 0;
    g.last_ack = GetTickCount64();
    g.next_keepalive = g.last_ack + HTNET_KEEPALIVE_MS;
    char m[64];
    snprintf(m, sizeof m, "In %s lobby %s as player %d", g.direct ? "DIRECT" : g.lan ? "LAN" : "NET", g.name,
             g.unit + 1);
    hy_log("lobby: input delay %d frame%s", g.delay, g.delay == 1 ? "" : "s");
    set_msg(m);
    chat_notice("You joined %s as player %d", g.name, g.unit + 1);
    hy_log("lobby: relay %s, token %08x", addr_str(&g.relay), g.token);
    snprintf(v, sizeof v, "getmembers %08x", g.token);
    send_text(&g.relay, v);
}

static char g_master_host[128]; /* the master's DNS name at the last browse */

static void add_entry(int lan, const char *info, const struct sockaddr_in *from)
{
    char game[32];
    ht_info_get(info, "gamename", game, sizeof game);
    if (strcmp(game, HTNET_GAMENAME) || ht_info_get_int(info, "protocol", 0) != HTNET_PROTOCOL)
        return;
    char idtext[16];
    ht_info_get(info, "lobby", idtext, sizeof idtext);
    uint32_t id = (uint32_t)strtoul(idtext, NULL, 10);
    LobbyEntry *e = NULL;
    for (int i = 0; i < g.count && !e; i++)
        if (g.list[i].id == id && g.list[i].lan == lan)
            e = &g.list[i];
    if (!e) {
        if (g.count >= LOBBY_MAX_LIST)
            return;
        e = &g.list[g.count++];
        memset(e, 0, sizeof *e);
        e->relay = *from;
        snprintf(e->host, sizeof e->host, "%s", lan ? "" : g_master_host);
        e->ping = (unsigned)(GetTickCount64() - g.browse_t0);
    }
    e->lan = lan;
    e->id = id;
    ht_info_get(info, "hostname", e->name, sizeof e->name);
    e->clients = ht_info_get_int(info, "clients", 0);
    e->max = ht_info_get_int(info, "sv_maxclients", 0);
    e->needpass = ht_info_get_int(info, "g_needpass", 0);
}

/* The lobby's last chat id is `id`: fetch what we're missing (at most every RETRY_MS). */
static void chat_catch_up(uint32_t id)
{
    ULONGLONG now = GetTickCount64();
    if (id <= g.chat_id || now < g.getchat_next)
        return;
    char text[48];
    snprintf(text, sizeof text, "getchat %08x %u", g.token, g.chat_id);
    send_text(&g.relay, text);
    g.getchat_next = now + RETRY_MS;
}

/* chat push|log: one line per infostring. A push must be the next id (else the gap is fetched);
 * a log may start after lines the lobby no longer keeps. */
static void chat_receive(int log, const char *info)
{
    char buf[HTNET_MAX_DATAGRAM + 1];
    snprintf(buf, sizeof buf, "%s", info);
    for (char *save = NULL, *l = strtok_s(buf, "\n", &save); l; l = strtok_s(NULL, "\n", &save)) {
        uint32_t id = (uint32_t)ht_info_get_int(l, "id", 0);
        if (!id || id <= g.chat_id)
            continue;
        if (!log && id != g.chat_id + 1) {
            chat_catch_up(id);
            return;
        }
        char name[HTNET_NAME_LEN], text[HTNET_CHAT_LEN];
        ht_info_get(l, "name", name, sizeof name);
        ht_info_get(l, "text", text, sizeof text);
        chat_add(ht_info_get_int(l, "unit", 0), name, text);
        g.chat_id = id;
    }
    if (log)
        g.getchat_next = 0; /* still behind? the next keepaliveAck asks again at once */
}

/* members: notices for who came, went or renamed */
static void members_notices(const char (*names)[HTNET_NAME_LEN], unsigned mask)
{
    for (int u = 0; u < HTNET_MAX_UNITS; u++) {
        int was = (g.mask >> u) & 1, is = (mask >> u) & 1;
        if (!g.members_seen || u == g.unit)
            continue;
        if (is && !was)
            chat_notice("%s joined as player %d", names[u], u + 1);
        else if (was && !is)
            chat_notice("%s left", g.names[u]);
        else if (is && strcmp(names[u], g.names[u]))
            chat_notice("%s is now %s", g.names[u], names[u]);
    }
    g.members_seen = 1;
}

/* DIRECT CONNECT: the PC at the address answered with its lobby. */
static void probe_answer(const char *info)
{
    char game[32], idtext[16];
    ht_info_get(info, "gamename", game, sizeof game);
    if (strcmp(game, HTNET_GAMENAME))
        return;
    if (ht_info_get_int(info, "protocol", 0) != HTNET_PROTOCOL) {
        fail("That lobby runs a different version");
        return;
    }
    LobbyEntry *e = &g.found;
    memset(e, 0, sizeof *e);
    e->lan = 1;
    e->direct = 1;
    e->relay = g.relay;
    snprintf(e->host, sizeof e->host, "%s", g.relay_host);
    ht_info_get(info, "lobby", idtext, sizeof idtext);
    e->id = (uint32_t)strtoul(idtext, NULL, 10);
    ht_info_get(info, "hostname", e->name, sizeof e->name);
    e->clients = ht_info_get_int(info, "clients", 0);
    e->max = ht_info_get_int(info, "sv_maxclients", 0);
    e->needpass = ht_info_get_int(info, "g_needpass", 0);
    g.req = REQ_NONE;
    hy_log("lobby: %s has lobby '%s' (%d/%d%s)", addr_str(&e->relay), e->name, e->clients, e->max,
           e->needpass ? ", password" : "");
    if (e->max && e->clients >= e->max) {
        fail("That lobby is full");
    } else if (e->needpass) {
        g.state = LOBBY_NEEDPASS;
        set_msg("That lobby needs a password");
    } else {
        LobbyEntry copy = *e;
        lobby_join(&copy, "");
    }
}

static void handle_oob(char *line, const char *info, const struct sockaddr_in *from)
{
    char *argv[6];
    int argc = 0;
    for (char *save = NULL, *t = strtok_s(line, " ", &save); t && argc < 6; t = strtok_s(NULL, " ", &save))
        argv[argc++] = t;
    if (!argc)
        return;
    const char *cmd = argv[0];
    int from_relay = from->sin_addr.s_addr == g.relay.sin_addr.s_addr && from->sin_port == g.relay.sin_port;

    if (!strcmp(cmd, "infoResponse")) {
        char c[16];
        ht_info_get(info, "challenge", c, sizeof c);
        if (!strcmp(c, g.challenge))
            add_entry(1, info, from);
        else if (g.req == REQ_PROBE && from_relay && !strcmp(c, g.nonce))
            probe_answer(info);
    } else if (!strcmp(cmd, "getserversInfoResponse") && argc >= 2 && !strcmp(argv[1], g.challenge)) {
        char buf[HTNET_MAX_DATAGRAM + 1];
        snprintf(buf, sizeof buf, "%s", info);
        for (char *save = NULL, *l = strtok_s(buf, "\n", &save); l; l = strtok_s(NULL, "\n", &save))
            add_entry(0, l, from);
    } else if (!from_relay) {
        return;
    } else if (!strcmp(cmd, "challengeResponse") && argc >= 3 && g.req == REQ_CHALLENGE &&
               !strcmp(argv[1], g.nonce)) {
        char proof[65], jinfo[256] = "";
        sha256_hex2(argv[2], g.verifier, proof);
        ht_info_set(jinfo, sizeof jinfo, "name", g_settings.net.player_name);
        ht_info_set_int(jinfo, sizeof jinfo, "protocol", HTNET_PROTOCOL);
        if (g.relay_host[0])
            ht_info_set(jinfo, sizeof jinfo, "host", g.relay_host);
        snprintf(g.text, sizeof g.text, "join %u %s %s\n%s", g.lobby_id, g.nonce, proof, jinfo);
        g.req = REQ_JOIN;
        g.tries = 0;
        send_request();
    } else if ((!strcmp(cmd, "createResponse") || !strcmp(cmd, "joinResponse")) && argc >= 2 &&
               g.req != REQ_NONE && !strcmp(argv[1], g.nonce)) {
        joined(info);
    } else if (!strcmp(cmd, "joinRejected") && argc >= 3 && g.req != REQ_NONE && !strcmp(argv[1], g.nonce)) {
        const char *why = argv[2];
        fail(!strcmp(why, "full")       ? "That lobby is full"
             : !strcmp(why, "password") ? "Wrong password"
             : !strcmp(why, "protocol") ? "That lobby runs a different version"
             : !strcmp(why, "limit")    ? "Too many lobbies from this address"
             : !strcmp(why, "busy")     ? "The master has no room for lobbies"
                                        : "That lobby is closed");
    } else if (!strcmp(cmd, "keepaliveAck") && argc >= 3 && g.state == LOBBY_IN) {
        g.last_ack = GetTickCount64();
        if (atoi(argv[2]) != g.ver) {
            char text[32];
            snprintf(text, sizeof text, "getmembers %08x", g.token);
            send_text(&g.relay, text);
        }
        if (argc >= 4)
            chat_catch_up((uint32_t)strtoul(argv[3], NULL, 10));
    } else if (!strcmp(cmd, "chat") && argc >= 2 && g.state == LOBBY_IN) {
        chat_receive(!strcmp(argv[1], "log"), info);
    } else if (!strcmp(cmd, "sayAck") && argc >= 2 && g.state == LOBBY_IN && g.say_n &&
               (unsigned)strtoul(argv[1], NULL, 10) == g.say_seq) {
        memmove(g.say_q, g.say_q + 1, sizeof g.say_q - sizeof g.say_q[0]);
        g.say_n--;
        g.say_seq++;
        g.say_tries = 0;
        g.say_next = 0;
    } else if (!strcmp(cmd, "members") && g.state == LOBBY_IN) {
        g.last_ack = GetTickCount64();
        g.ver = ht_info_get_int(info, "ver", 0);
        g.owner = ht_info_get_int(info, "owner", -1);
        int start = ht_info_get_int(info, "start", -1);
        if (g.start_want && start >= g.start_want) {
            g.start_want = 0; /* our own startGame arrived */
            hy_log("lobby: the lobby has our START GAME (%d)", start);
        } else if (g.start >= 0 && start > g.start) {
            g.start_pending = 1;
            hy_log("lobby: the host started the game (%d)", start);
        }
        if (start > g.start)
            g.start = start; /* the first members after joining only records it */
        char names[HTNET_MAX_UNITS][HTNET_NAME_LEN] = {0};
        unsigned mask = 0;
        for (int u = 0; u < HTNET_MAX_UNITS; u++) {
            char key[8];
            snprintf(key, sizeof key, "n%d", u);
            if (ht_info_get(info, key, names[u], sizeof names[u]))
                mask |= 1u << u;
        }
        members_notices(names, mask);
        g.mask = mask;
        memcpy(g.names, names, sizeof names);
        chat_catch_up((uint32_t)ht_info_get_int(info, "chat", 0));
        hy_log("lobby: %d/%d players", ht_info_get_int(info, "clients", 0), g.max);
    } else if (!strcmp(cmd, "kicked") && argc >= 2 && g.state == LOBBY_IN &&
               (uint32_t)strtoul(argv[1], NULL, 16) == g.token) {
        fail("The lobby was closed");
    }
}

void lobby_poll(void)
{
    if (!g.init || g.sock == INVALID_SOCKET)
        return;
    for (;;) {
        uint8_t pkt[HTNET_MAX_DATAGRAM + 64];
        struct sockaddr_in from;
        int fromlen = sizeof from;
        int n = recvfrom(g.sock, (char *)pkt, sizeof pkt, 0, (struct sockaddr *)&from, &fromlen);
        if (n == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAEMSGSIZE || err == WSAECONNRESET)
                continue;
            break;
        }
        char line[HTNET_MAX_DATAGRAM + 1];
        const char *info;
        if (ht_oob_parse(pkt, n, line, sizeof line, &info)) {
            handle_oob(line, info, &from);
        } else if (g.state == LOBBY_IN && g.data && from.sin_addr.s_addr == g.relay.sin_addr.s_addr &&
                   from.sin_port == g.relay.sin_port) {
            g.data(pkt, n, &from);
        }
    }

    ULONGLONG now = GetTickCount64();
    if (g.req != REQ_NONE && now >= g.next_send) {
        if (g.tries >= RETRIES)
            fail(g.req == REQ_PROBE ? "No lobby answered at that address"
                 : g.direct         ? "No answer from that lobby"
                 : g.lan            ? "No answer from the LAN lobby"
                                    : "No answer from the master server");
        else
            send_request();
    }
    if (g.state == LOBBY_IN && g.start_want && now >= g.start_next) {
        if (g.start_tries++ >= RETRIES) {
            hy_log("lobby: no answer to START GAME");
            g.start_want = 0;
        } else {
            char text[48];
            snprintf(text, sizeof text, "startGame %08x %d", g.token, g.start_want);
            send_text(&g.relay, text);
            g.start_next = now + RETRY_MS;
        }
    }
    if (g.state == LOBBY_IN && g.say_n && now >= g.say_next) {
        if (g.say_tries++ >= RETRIES) {
            chat_notice("Not sent (no answer from the lobby): %s", g.say_q[0]);
            memmove(g.say_q, g.say_q + 1, sizeof g.say_q - sizeof g.say_q[0]);
            g.say_n--;
            g.say_seq++;
            g.say_tries = 0;
        } else {
            char info[128] = "", text[160];
            ht_info_set(info, sizeof info, "text", g.say_q[0]);
            snprintf(text, sizeof text, "say %08x %u\n%s", g.token, g.say_seq, info);
            send_text(&g.relay, text);
            g.say_next = now + RETRY_MS;
        }
    }
    if (g.state == LOBBY_IN) {
        if (now - g.last_ack > HTNET_TIMEOUT_MS) {
            fail("Lost contact with the lobby");
        } else if (now >= g.next_keepalive) {
            /* the name rides along, so a rename reaches the lobby even if one keepalive is lost */
            char info[64] = "", text[128];
            ht_info_set(info, sizeof info, "name", g_settings.net.player_name);
            snprintf(text, sizeof text, "keepalive %08x %u\n%s", g.token, ++g.ka_seq, info);
            send_text(&g.relay, text);
            g.next_keepalive = now + HTNET_KEEPALIVE_MS;
        }
    }
}

void lobby_connect(const char *address)
{
    lobby_leave();
    if (!open_socket()) {
        fail("No network");
        return;
    }
    g.lan = g.direct = 1;
    if (!resolve(address, HTNET_LAN_PORT, &g.relay, g.relay_host, sizeof g.relay_host)) {
        fail("Can't find that address");
        return;
    }
    char info[180] = "";
    if (g.relay_host[0])
        ht_info_set(info, sizeof info, "host", g.relay_host);
    snprintf(g.nonce, sizeof g.nonce, "%08x", ht_random32());
    snprintf(g.text, sizeof g.text, "getinfo %s%s%s", g.nonce, info[0] ? "\n" : "", info);
    g.req = REQ_PROBE;
    g.tries = 0;
    g.state = LOBBY_BUSY;
    char m[64];
    snprintf(m, sizeof m, "Contacting %s...", addr_str(&g.relay));
    set_msg(m);
    send_request();
}

const LobbyEntry *lobby_found(void)
{
    return &g.found;
}

int lobby_host_address(char *out, size_t cap)
{
    if (!g.server || g.state != LOBBY_IN)
        return 0;
    ULONG size = 16384;
    IP_ADAPTER_ADDRESSES *list = NULL;
    ULONG r = ERROR_BUFFER_OVERFLOW;
    for (int tries = 0; tries < 3 && r == ERROR_BUFFER_OVERFLOW; tries++) {
        free(list);
        list = malloc(size);
        if (!list)
            return 0;
        r = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                 NULL, list, &size);
    }
    int found = 0;
    for (IP_ADAPTER_ADDRESSES *a = r == NO_ERROR ? list : NULL; a && !found; a = a->Next) {
        /* the adapter with a default gateway is the one others reach */
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK || !a->FirstGatewayAddress)
            continue;
        for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u && !found; u = u->Next)
            if (u->Address.lpSockaddr->sa_family == AF_INET) {
                struct sockaddr_in in = *(struct sockaddr_in *)u->Address.lpSockaddr;
                in.sin_port = htons((u_short)lobby_server_port(g.server));
                snprintf(out, cap, "%s", addr_str(&in));
                found = 1;
            }
    }
    free(list);
    return found;
}

int lobby_chat(const LobbyChatLine **out)
{
    *out = g.chat;
    return g.chat_n;
}

unsigned lobby_chat_version(void)
{
    return g.chat_ver;
}

int lobby_say(const char *text)
{
    if (g.state != LOBBY_IN || g.say_n == LOBBY_SAY_QUEUE)
        return 0;
    char *q = g.say_q[g.say_n];
    snprintf(q, HTNET_CHAT_LEN, "%s", text);
    ht_sanitize(q);
    if (!q[0])
        return 0;
    if (g.say_n++ == 0) {
        g.say_tries = 0;
        g.say_next = 0; /* sent by the next poll */
    }
    return 1;
}

int lobby_owner(void)
{
    return g.state == LOBBY_IN ? g.owner : -1;
}

int lobby_is_owner(void)
{
    return g.state == LOBBY_IN && g.owner == g.unit && g.start >= 0;
}

int lobby_start_game(void)
{
    if (!lobby_is_owner())
        return 0;
    g.start_want = ++g.start; /* the echo of our own start doesn't start us again */
    g.start_tries = 0;
    g.start_next = 0; /* sent by the next poll, then retransmitted until the members show it */
    g.start_pending = 0;
    hy_log("lobby: START GAME (%d)", g.start_want);
    lobby_poll();
    return 1;
}

int lobby_take_start(void)
{
    int r = g.start_pending;
    g.start_pending = 0;
    return r;
}

void lobby_name_changed(void)
{
    if (g.state == LOBBY_IN)
        g.next_keepalive = 0; /* send the new name with the next poll */
}

void lobby_browse(int clear)
{
    if (!open_socket())
        return;
    if (clear || !g.challenge[0]) {
        g.count = 0;
        snprintf(g.challenge, sizeof g.challenge, "%08x", ht_random32());
    }
    g.browse_t0 = GetTickCount64();
    char text[40];
    snprintf(text, sizeof text, "getinfo %s", g.challenge);
    uint32_t bcast[16];
    int nb = g_settings.net.lan_broadcast ? net_lan_broadcasts(bcast, 15) : 0;
    bcast[nb++] = htonl(INADDR_LOOPBACK);
    for (int i = 0; i < nb; i++)
        for (int k = 0; k < HTNET_LAN_PORTS; k++) {
            struct sockaddr_in to;
            make_addr(&to, bcast[i], g_settings.net.lan_port + k);
            send_text(&to, text);
        }
    struct sockaddr_in master;
    if (resolve(g_settings.net.master, HTNET_MASTER_PORT, &master, g_master_host, sizeof g_master_host)) {
        char q[256], info[180] = "";
        if (g_master_host[0])
            ht_info_set(info, sizeof info, "host", g_master_host);
        snprintf(q, sizeof q, "getserversInfo %d %s%s%s", HTNET_PROTOCOL, g.challenge, info[0] ? "\n" : "", info);
        send_text(&master, q);
    }
}

int lobby_list(const LobbyEntry **out)
{
    *out = g.list;
    return g.count;
}

int lobby_state(void)
{
    return g.state;
}

const char *lobby_message(void)
{
    return g.msg;
}

int lobby_active(int *unit)
{
    if (g.state != LOBBY_IN)
        return 0;
    if (unit)
        *unit = g.unit;
    return 1;
}

const char *lobby_name(void)
{
    return g.name;
}

int lobby_is_lan(void)
{
    return g.lan;
}

int lobby_is_direct(void)
{
    return g.direct;
}

int lobby_max(void)
{
    return g.max;
}

int lobby_input_delay(void)
{
    return g.delay < 1 ? 1 : g.delay;
}

const HtGameOpts *lobby_game_opts(void)
{
    return g.state == LOBBY_IN && g.has_opts ? &g.opts : NULL;
}

unsigned lobby_members(char names[HTNET_MAX_UNITS][HTNET_NAME_LEN])
{
    if (names)
        memcpy(names, g.names, sizeof g.names);
    return g.state == LOBBY_IN ? g.mask : 0;
}

void lobby_set_data_handler(LobbyDataHandler h)
{
    g.data = h;
}

uint32_t lobby_token(void)
{
    return g.token;
}

void lobby_client_init(void)
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa))
        return;
    g.sock = INVALID_SOCKET;
    g.owner = g.start = -1;
    g.init = 1;
}

/* HYDRO_LOBBY (testing, like HYDRO_NET_UNIT): enter a lobby before the game starts.
 *   host:lan|net:<name>:<max>[:<password>]
 *   join:lan|net:<name>[:<password>]      (waits up to 10 s for it to show up)
 *   direct:<host[:port]>                  (DIRECT CONNECT, no password)
 * HYDRO_LOBBY_SAY=<text> then says a chat line. */
void lobby_autostart(void)
{
    const char *env = getenv("HYDRO_LOBBY");
    if (!env || !*env)
        return;
    ULONGLONG end = GetTickCount64() + 10000;
    if (!_strnicmp(env, "direct:", 7)) {
        lobby_connect(env + 7);
        goto wait;
    }
    char buf[160], *f[5] = {0};
    snprintf(buf, sizeof buf, "%s", env);
    int n = 0;
    for (char *p = buf; p && n < 5; n++) {
        f[n] = p;
        p = strchr(p, ':');
        if (p)
            *p++ = 0;
    }
    if (n < 3) {
        hy_log("lobby: HYDRO_LOBBY=%s: expected host:lan|net:name:max[:password] or join:lan|net:name[:password]", env);
        return;
    }
    int lan = !_stricmp(f[1], "lan");
    if (!_stricmp(f[0], "host")) {
        lobby_create(lan, f[2], f[3] ? atoi(f[3]) : 4, f[4] ? f[4] : "", g_settings.net.lobby_delay);
    } else {
        const LobbyEntry *found = NULL;
        ULONGLONG next = 0;
        while (!found && GetTickCount64() < end) {
            if (GetTickCount64() >= next) {
                lobby_browse(1);
                next = GetTickCount64() + 1000;
            }
            Sleep(20);
            lobby_poll();
            const LobbyEntry *list;
            int count = lobby_list(&list);
            for (int i = 0; i < count && !found; i++)
                if (list[i].lan == lan && !_stricmp(list[i].name, f[2]))
                    found = &list[i];
        }
        if (!found) {
            hy_log("lobby: HYDRO_LOBBY: lobby '%s' not found", f[2]);
            return;
        }
        LobbyEntry e = *found;
        lobby_join(&e, f[3] ? f[3] : "");
    }
wait:
    while (lobby_state() == LOBBY_BUSY && GetTickCount64() < end) {
        Sleep(20);
        lobby_poll();
    }
    hy_log("lobby: HYDRO_LOBBY: %s", lobby_message());
    const char *say = getenv("HYDRO_LOBBY_SAY");
    if (say && *say && lobby_say(say)) {
        for (end = GetTickCount64() + 2000; GetTickCount64() < end && g.say_n;) {
            lobby_poll();
            Sleep(20);
        }
    }
}
