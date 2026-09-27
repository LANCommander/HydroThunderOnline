/*
 * Lobby server and relay. See common/htnet_proto.h for the messages and docs/network.md for the
 * design. Everything runs from lobby_server_poll on the caller's thread.
 */
#include "../common/htsock.h"

#include "lobby.h"

#include "../common/htnet_proto.h"
#include "../common/sha256.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#endif

#ifdef _WIN32
#define strtok_r_compat strtok_s
#else
#define strtok_r_compat strtok_r
#endif

#define MAX_LOBBIES_LIMIT 4096 /* the token's low 12 bits index the lobby table */
#define LOBBIES_PER_IP 4
#define QUEUE_CAP 8192

typedef struct Member {
    int used;
    uint32_t token;
    uint32_t nonce; /* the create/join nonce: a repeated request gets the same answer */
    struct sockaddr_in addr;
    uint64_t last;
    char name[HTNET_NAME_LEN];
    uint32_t say_seq; /* the last chat line taken from this member */
    uint64_t say_last;
} Member;

typedef struct ChatLine {
    uint32_t id; /* 0 = none */
    int unit;
    char name[HTNET_NAME_LEN];
    char text[HTNET_CHAT_LEN];
} ChatLine;

typedef struct Lobby {
    int used;
    uint32_t id;
    char name[HTNET_LOBBY_LEN];
    int max;
    int delay;         /* input delay (frames) every member's race uses */
    char verifier[65]; /* "" = no password */
    char gopts[192];   /* game options every member races with (ht_gameopts_encode), passed on as is */
    uint32_t ver;      /* bumped on every membership change */
    int owner;         /* the unit that may start the game: the creator, then the lowest unit left */
    int start;         /* bumped by every startGame from the owner */
    struct sockaddr_in creator;
    uint32_t create_nonce;
    Member m[HTNET_MAX_UNITS];
    uint32_t chat_id;                  /* the last chat line's id (they count from 1) */
    ChatLine chat[HTNET_CHAT_KEEP];    /* by id % HTNET_CHAT_KEEP */
} Lobby;

#define SAY_MIN_MS 250 /* a member's lines are taken at most this often (the rest are resent) */

typedef struct Delayed {
    uint64_t due;
    struct sockaddr_in to;
    int len;
    uint8_t data[sizeof(HtEnvelope) + HTNET_MAX_PAYLOAD];
} Delayed;

struct LobbyServer {
    int mode;
    SOCKET sock;
    int port;
    LobbyLog log;
    int verbose;
    Lobby *lobbies;
    int max_lobbies;
    uint32_t next_id;
    char secret[20];
    uint64_t last_sweep;
    Delayed *q;
    int qhead, qcount;
    int delay_ms;
};

#define LOG(...)                                                                                       \
    do {                                                                                               \
        if (s->log)                                                                                    \
            s->log(__VA_ARGS__);                                                                       \
    } while (0)

static const char *addr_str(const struct sockaddr_in *a)
{
    static char buf[4][32];
    static int n;
    char *b = buf[n++ & 3];
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, (void *)&a->sin_addr, ip, sizeof ip);
    snprintf(b, 32, "%s:%u", ip, ntohs(a->sin_port));
    return b;
}

static int same_addr(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

static void send_raw(LobbyServer *s, const struct sockaddr_in *to, const void *buf, int len)
{
    sendto(s->sock, (const char *)buf, len, 0, (const struct sockaddr *)to, sizeof *to);
}

static void send_text(LobbyServer *s, const struct sockaddr_in *to, const char *text)
{
    uint8_t pkt[HTNET_MAX_DATAGRAM];
    send_raw(s, to, pkt, ht_oob_build(pkt, sizeof pkt, text));
}

/* ---------------------------------------------------------------- lobbies */

static int member_count(const Lobby *l)
{
    int n = 0;
    for (int u = 0; u < HTNET_MAX_UNITS; u++)
        n += l->m[u].used;
    return n;
}

static Lobby *lobby_by_id(LobbyServer *s, uint32_t id)
{
    for (int i = 0; i < s->max_lobbies; i++)
        if (s->lobbies[i].used && (s->lobbies[i].id == id || (s->mode == LOBBY_LAN && id == 0)))
            return &s->lobbies[i];
    return NULL;
}

static Member *member_by_token(LobbyServer *s, uint32_t token, Lobby **lobby)
{
    unsigned idx = token & 0xfffu;
    if (!token || idx >= (unsigned)s->max_lobbies || !s->lobbies[idx].used)
        return NULL;
    Lobby *l = &s->lobbies[idx];
    for (int u = 0; u < HTNET_MAX_UNITS; u++)
        if (l->m[u].used && l->m[u].token == token) {
            *lobby = l;
            return &l->m[u];
        }
    return NULL;
}

static void lobby_info(const Lobby *l, char *out, size_t cap)
{
    out[0] = 0;
    ht_info_set(out, cap, "hostname", l->name);
    ht_info_set(out, cap, "gamename", HTNET_GAMENAME);
    ht_info_set_int(out, cap, "protocol", HTNET_PROTOCOL);
    ht_info_set_int(out, cap, "clients", member_count(l));
    ht_info_set_int(out, cap, "sv_maxclients", l->max);
    ht_info_set_int(out, cap, "g_needpass", l->verifier[0] != 0);
    ht_info_set_int(out, cap, "delay", l->delay);
    char id[16];
    snprintf(id, sizeof id, "%u", l->id);
    ht_info_set(out, cap, "lobby", id);
}

static void members_text(const Lobby *l, char *out, size_t cap)
{
    char info[1200] = "";
    char id[16];
    snprintf(id, sizeof id, "%u", l->id);
    ht_info_set(info, sizeof info, "lobby", id);
    ht_info_set(info, sizeof info, "hostname", l->name);
    ht_info_set_int(info, sizeof info, "clients", member_count(l));
    ht_info_set_int(info, sizeof info, "sv_maxclients", l->max);
    ht_info_set_int(info, sizeof info, "ver", (int)l->ver);
    ht_info_set_int(info, sizeof info, "owner", l->owner);
    ht_info_set_int(info, sizeof info, "start", l->start);
    ht_info_set_int(info, sizeof info, "chat", (int)l->chat_id);
    for (int u = 0; u < HTNET_MAX_UNITS; u++)
        if (l->m[u].used) {
            char key[8];
            snprintf(key, sizeof key, "n%d", u);
            ht_info_set(info, sizeof info, key, l->m[u].name);
        }
    snprintf(out, cap, "members\n%s", info);
}

static void push_members(LobbyServer *s, Lobby *l)
{
    char text[1300];
    members_text(l, text, sizeof text);
    for (int u = 0; u < HTNET_MAX_UNITS; u++)
        if (l->m[u].used)
            send_text(s, &l->m[u].addr, text);
}

/* ---------------------------------------------------------------- chat */

static void chat_line_info(const ChatLine *c, char *out, size_t cap)
{
    out[0] = 0;
    ht_info_set_int(out, cap, "id", (int)c->id);
    ht_info_set_int(out, cap, "unit", c->unit);
    ht_info_set(out, cap, "name", c->name);
    ht_info_set(out, cap, "text", c->text);
}

/* say: 1 if the member's line was taken (or already was), so it can be acked */
static int chat_say(LobbyServer *s, Lobby *l, Member *m, uint32_t seq, const char *info)
{
    if (seq <= m->say_seq)
        return 1; /* a resend of a line we have */
    uint64_t now = ht_now_ms();
    if (now - m->say_last < SAY_MIN_MS)
        return 0; /* too fast: not acked, so the client resends it */
    char text[HTNET_CHAT_LEN];
    ht_info_get(info, "text", text, sizeof text);
    ht_sanitize(text);
    m->say_seq = seq;
    m->say_last = now;
    if (!text[0])
        return 1;
    ChatLine *c = &l->chat[++l->chat_id % HTNET_CHAT_KEEP];
    c->id = l->chat_id;
    c->unit = (int)(m - l->m);
    snprintf(c->name, sizeof c->name, "%s", m->name);
    snprintf(c->text, sizeof c->text, "%s", text);
    LOG("lobby %u '%s': <%s> %s", l->id, l->name, c->name, c->text);
    char line[300], msg[320];
    chat_line_info(c, line, sizeof line);
    snprintf(msg, sizeof msg, "chat push\n%s", line);
    for (int u = 0; u < HTNET_MAX_UNITS; u++)
        if (l->m[u].used)
            send_text(s, &l->m[u].addr, msg);
    return 1;
}

/* getchat: the lines after `since`, oldest first, as many as fit one datagram (the client asks
 * again while it's behind) */
static void chat_send_log(LobbyServer *s, const struct sockaddr_in *to, const Lobby *l, uint32_t since)
{
    char msg[HTNET_MAX_DATAGRAM - 8] = "chat log";
    size_t used = strlen(msg);
    uint32_t first = l->chat_id > HTNET_CHAT_KEEP ? l->chat_id - HTNET_CHAT_KEEP + 1 : 1;
    if (since + 1 > first)
        first = since + 1;
    for (uint32_t id = first; id <= l->chat_id; id++) {
        const ChatLine *c = &l->chat[id % HTNET_CHAT_KEEP];
        char line[300];
        chat_line_info(c, line, sizeof line);
        if (used + 1 + strlen(line) >= sizeof msg)
            break;
        used += (size_t)snprintf(msg + used, sizeof msg - used, "\n%s", line);
    }
    send_text(s, to, msg);
}

static void remove_member(LobbyServer *s, Lobby *l, int u, const char *why)
{
    LOG("lobby %u '%s': unit %d (%s, %s) %s", l->id, l->name, u + 1, l->m[u].name, addr_str(&l->m[u].addr), why);
    memset(&l->m[u], 0, sizeof l->m[u]);
    l->ver++;
    if (!member_count(l)) {
        LOG("lobby %u '%s' closed", l->id, l->name);
        memset(l, 0, sizeof *l);
    } else {
        if (u == l->owner)
            for (l->owner = 0; !l->m[l->owner].used; l->owner++)
                ;
        push_members(s, l);
    }
}

static uint32_t new_token(int lobby_index)
{
    uint32_t r;
    do
        r = ht_random32() & 0xfffff000u;
    while (!r);
    return r | (uint32_t)lobby_index;
}

static void salt_for(LobbyServer *s, const struct sockaddr_in *from, const char *nonce, uint32_t lobby,
                     char out[17])
{
    char what[96], hex[65];
    snprintf(what, sizeof what, "%s:%s:%u", addr_str(from), nonce, lobby);
    sha256_hex2(s->secret, what, hex);
    memcpy(out, hex, 16);
    out[16] = 0;
}

/* ---------------------------------------------------------------- commands */

static void reply_joined(LobbyServer *s, const struct sockaddr_in *to, const char *cmd, const char *nonce,
                         const Lobby *l, int u)
{
    char info[512] = "", text[600], v[16];
    snprintf(v, sizeof v, "%u", l->id);
    ht_info_set(info, sizeof info, "lobby", v);
    ht_info_set_int(info, sizeof info, "unit", u);
    snprintf(v, sizeof v, "%08x", l->m[u].token);
    ht_info_set(info, sizeof info, "token", v);
    ht_info_set(info, sizeof info, "hostname", l->name);
    ht_info_set_int(info, sizeof info, "sv_maxclients", l->max);
    ht_info_set_int(info, sizeof info, "delay", l->delay);
    if (l->gopts[0])
        ht_info_set(info, sizeof info, "gopts", l->gopts);
    snprintf(text, sizeof text, "%s %s\n%s", cmd, nonce, info);
    send_text(s, to, text);
}

static void reject(LobbyServer *s, const struct sockaddr_in *to, const char *nonce, const char *why)
{
    char text[96];
    snprintf(text, sizeof text, "joinRejected %s %s", nonce, why);
    send_text(s, to, text);
}

static void cmd_create(LobbyServer *s, const struct sockaddr_in *from, const char *nonce_s, const char *info)
{
    uint32_t nonce = (uint32_t)strtoul(nonce_s, NULL, 16);
    for (int i = 0; i < s->max_lobbies; i++) {
        Lobby *l = &s->lobbies[i];
        if (l->used && l->create_nonce == nonce && same_addr(&l->creator, from) && l->m[0].used &&
            l->m[0].nonce == nonce) {
            reply_joined(s, from, "createResponse", nonce_s, l, 0); /* a repeat */
            return;
        }
    }
    if (ht_info_get_int(info, "protocol", 0) != HTNET_PROTOCOL) {
        reject(s, from, nonce_s, "protocol");
        return;
    }
    if (s->mode == LOBBY_LAN) {
        if (ntohl(from->sin_addr.s_addr) >> 24 != 127) {
            reject(s, from, nonce_s, "closed");
            return;
        }
        for (int i = 0; i < s->max_lobbies; i++)
            if (s->lobbies[i].used) {
                Lobby *l = &s->lobbies[i];
                for (int u = 0; u < HTNET_MAX_UNITS; u++)
                    if (l->m[u].used) {
                        char text[32];
                        snprintf(text, sizeof text, "kicked %08x", l->m[u].token);
                        send_text(s, &l->m[u].addr, text);
                    }
                memset(l, 0, sizeof *l);
            }
    } else {
        int mine = 0;
        for (int i = 0; i < s->max_lobbies; i++)
            mine += s->lobbies[i].used && s->lobbies[i].creator.sin_addr.s_addr == from->sin_addr.s_addr;
        if (mine >= LOBBIES_PER_IP) {
            reject(s, from, nonce_s, "limit");
            return;
        }
    }
    int idx = -1;
    for (int i = 0; i < s->max_lobbies && idx < 0; i++)
        if (!s->lobbies[i].used)
            idx = i;
    if (idx < 0) {
        reject(s, from, nonce_s, "busy");
        return;
    }
    Lobby *l = &s->lobbies[idx];
    memset(l, 0, sizeof *l);
    l->used = 1;
    l->id = s->next_id++;
    if (!l->id)
        l->id = s->next_id++;
    ht_info_get(info, "hostname", l->name, sizeof l->name);
    if (!l->name[0])
        snprintf(l->name, sizeof l->name, "LOBBY %u", l->id % 1000);
    l->max = ht_info_get_int(info, "sv_maxclients", HTNET_MAX_UNITS);
    l->max = l->max < 2 ? 2 : l->max > HTNET_MAX_UNITS ? HTNET_MAX_UNITS : l->max;
    l->delay = ht_info_get_int(info, "delay", 1);
    l->delay = l->delay < 1 ? 1 : l->delay > 6 ? 6 : l->delay;
    ht_info_get(info, "verifier", l->verifier, sizeof l->verifier);
    ht_info_get(info, "gopts", l->gopts, sizeof l->gopts);
    ht_sanitize(l->gopts);
    l->creator = *from;
    l->create_nonce = nonce;
    Member *m = &l->m[0];
    m->used = 1;
    m->token = new_token(idx);
    m->nonce = nonce;
    m->addr = *from;
    m->last = ht_now_ms();
    ht_info_get(info, "name", m->name, sizeof m->name);
    LOG("lobby %u '%s' created by %s (%s), max %d%s", l->id, l->name, m->name, addr_str(from), l->max,
        l->verifier[0] ? ", password" : "");
    reply_joined(s, from, "createResponse", nonce_s, l, 0);
    push_members(s, l);
}

static void cmd_join(LobbyServer *s, const struct sockaddr_in *from, const char *lobby_s, const char *nonce_s,
                     const char *proof, const char *info)
{
    uint32_t nonce = (uint32_t)strtoul(nonce_s, NULL, 16);
    Lobby *l = lobby_by_id(s, (uint32_t)strtoul(lobby_s, NULL, 10));
    if (!l) {
        reject(s, from, nonce_s, "closed");
        return;
    }
    for (int u = 0; u < HTNET_MAX_UNITS; u++)
        if (l->m[u].used && l->m[u].nonce == nonce && same_addr(&l->m[u].addr, from)) {
            reply_joined(s, from, "joinResponse", nonce_s, l, u);
            return;
        }
    char salt[17];
    salt_for(s, from, nonce_s, l->id, salt);
    if (!proof) {
        char text[64];
        snprintf(text, sizeof text, "challengeResponse %s %s", nonce_s, salt);
        send_text(s, from, text);
        return;
    }
    if (ht_info_get_int(info, "protocol", 0) != HTNET_PROTOCOL) {
        reject(s, from, nonce_s, "protocol");
        return;
    }
    if (l->verifier[0]) {
        char expect[65];
        sha256_hex2(salt, l->verifier, expect);
        if (strcmp(expect, proof)) {
            LOG("lobby %u '%s': wrong password from %s", l->id, l->name, addr_str(from));
            reject(s, from, nonce_s, "password");
            return;
        }
    }
    int u = -1;
    if (member_count(l) < l->max)
        for (int k = 0; k < HTNET_MAX_UNITS && u < 0; k++)
            if (!l->m[k].used)
                u = k;
    if (u < 0) {
        reject(s, from, nonce_s, "full");
        return;
    }
    Member *m = &l->m[u];
    memset(m, 0, sizeof *m);
    m->used = 1;
    m->token = new_token((int)(l - s->lobbies));
    m->nonce = nonce;
    m->addr = *from;
    m->last = ht_now_ms();
    ht_info_get(info, "name", m->name, sizeof m->name);
    l->ver++;
    LOG("lobby %u '%s': unit %d is %s (%s), %d/%d", l->id, l->name, u + 1, m->name, addr_str(from),
        member_count(l), l->max);
    reply_joined(s, from, "joinResponse", nonce_s, l, u);
    push_members(s, l);
}

static void cmd_getservers(LobbyServer *s, const struct sockaddr_in *from, int argc, char **argv)
{
    if (argc < 3 || strcmp(argv[1], HTNET_GAMENAME) || atoi(argv[2]) != HTNET_PROTOCOL)
        return;
    int full = 0, empty = 0;
    for (int i = 3; i < argc; i++)
        full |= !strcmp(argv[i], "full"), empty |= !strcmp(argv[i], "empty");
    uint8_t pkt[HTNET_MAX_DATAGRAM];
    const char *head = "getserversResponse";
    int n = ht_oob_build(pkt, sizeof pkt, head);
    for (int i = 0; i < s->max_lobbies; i++) {
        const Lobby *l = &s->lobbies[i];
        int c = l->used ? member_count(l) : 0;
        if (!l->used || (!full && c >= l->max) || (!empty && !c))
            continue;
        if (n + 7 + 7 > (int)sizeof pkt) {
            send_raw(s, from, pkt, n);
            n = ht_oob_build(pkt, sizeof pkt, head);
        }
        pkt[n++] = '\\';
        pkt[n++] = (uint8_t)(l->id >> 24), pkt[n++] = (uint8_t)(l->id >> 16);
        pkt[n++] = (uint8_t)(l->id >> 8), pkt[n++] = (uint8_t)l->id;
        pkt[n++] = 0, pkt[n++] = 0;
    }
    memcpy(pkt + n, "\\EOT\0\0\0", 7);
    send_raw(s, from, pkt, n + 7);
}

static void cmd_getserversinfo(LobbyServer *s, const struct sockaddr_in *from, int argc, char **argv)
{
    if (argc < 3 || atoi(argv[1]) != HTNET_PROTOCOL)
        return;
    const char *challenge = argv[2];
    char lines[HTNET_MAX_DATAGRAM], head[64];
    int page = 0;
    size_t used = 0;
    lines[0] = 0;
    for (int i = 0; i <= s->max_lobbies; i++) {
        char info[512] = "";
        if (i < s->max_lobbies && s->lobbies[i].used)
            lobby_info(&s->lobbies[i], info, sizeof info);
        else if (i < s->max_lobbies)
            continue;
        int last = i == s->max_lobbies;
        if (last || used + strlen(info) + 1 + 80 > sizeof lines) {
            char text[HTNET_MAX_DATAGRAM + 64];
            snprintf(head, sizeof head, "getserversInfoResponse %s %d %d", challenge, page++, last);
            snprintf(text, sizeof text, "%s\n%s", head, lines);
            send_text(s, from, text);
            lines[0] = 0;
            used = 0;
        }
        if (!last) {
            used += (size_t)snprintf(lines + used, sizeof lines - used, "%s\n", info);
        }
    }
}

static void handle_oob(LobbyServer *s, const struct sockaddr_in *from, char *line, const char *info)
{
    char *argv[8];
    int argc = 0;
    for (char *save = NULL, *t = strtok_r_compat(line, " ", &save); t && argc < 8; t = strtok_r_compat(NULL, " ", &save))
        argv[argc++] = t;
    if (!argc)
        return;
    const char *cmd = argv[0];
    if (s->verbose)
        LOG("rx %s from %s", cmd, addr_str(from));

    if (!strcmp(cmd, "getinfo") && argc >= 2) {
        Lobby *l = lobby_by_id(s, argc >= 3 ? (uint32_t)strtoul(argv[2], NULL, 10) : 0);
        if (!l)
            return;
        char body[600], text[700];
        ht_sanitize(argv[1]);
        lobby_info(l, body, sizeof body);
        snprintf(text, sizeof text, "infoResponse\n\\challenge\\%.32s%s", argv[1], body);
        send_text(s, from, text);
    } else if (!strcmp(cmd, "getservers")) {
        cmd_getservers(s, from, argc, argv);
    } else if (!strcmp(cmd, "getserversInfo")) {
        cmd_getserversinfo(s, from, argc, argv);
    } else if (!strcmp(cmd, "createLobby") && argc >= 2) {
        cmd_create(s, from, argv[1], info);
    } else if (!strcmp(cmd, "join") && argc >= 3) {
        cmd_join(s, from, argv[1], argv[2], argc >= 4 ? argv[3] : NULL, info);
    } else if ((!strcmp(cmd, "keepalive") || !strcmp(cmd, "getmembers") || !strcmp(cmd, "leave") ||
                !strcmp(cmd, "startGame") || !strcmp(cmd, "say") || !strcmp(cmd, "getchat")) &&
               argc >= 2) {
        uint32_t token = (uint32_t)strtoul(argv[1], NULL, 16);
        Lobby *l;
        Member *m = member_by_token(s, token, &l);
        char text[1300];
        if (!m) {
            snprintf(text, sizeof text, "kicked %s", argv[1]);
            send_text(s, from, text);
            return;
        }
        if (!strcmp(cmd, "leave")) {
            send_text(s, from, "leaveAck");
            remove_member(s, l, (int)(m - l->m), "left");
            return;
        }
        if (!same_addr(&m->addr, from)) {
            LOG("lobby %u '%s': unit %d moved %s -> %s", l->id, l->name, (int)(m - l->m) + 1, addr_str(&m->addr),
                addr_str(from));
            m->addr = *from;
        }
        m->last = ht_now_ms();
        if (!strcmp(cmd, "keepalive")) {
            snprintf(text, sizeof text, "keepaliveAck %s %u %u", argc >= 3 ? argv[2] : "0", l->ver, l->chat_id);
            send_text(s, from, text);
            /* a member renamed itself (the name rides on every keepalive, so a lost one heals) */
            char name[HTNET_NAME_LEN];
            if (ht_info_get(info, "name", name, sizeof name) && name[0] && strcmp(name, m->name)) {
                LOG("lobby %u '%s': unit %d renamed %s -> %s", l->id, l->name, (int)(m - l->m) + 1, m->name, name);
                snprintf(m->name, sizeof m->name, "%s", name);
                l->ver++;
                push_members(s, l);
            }
            return;
        }
        if (!strcmp(cmd, "say")) {
            if (argc >= 3 && chat_say(s, l, m, (uint32_t)strtoul(argv[2], NULL, 10), info)) {
                snprintf(text, sizeof text, "sayAck %s", argv[2]);
                send_text(s, from, text);
            }
            return;
        }
        if (!strcmp(cmd, "getchat")) {
            chat_send_log(s, from, l, argc >= 3 ? (uint32_t)strtoul(argv[2], NULL, 10) : 0);
            return;
        }
        if (!strcmp(cmd, "startGame")) {
            /* argv[2] = the start number wanted (last seen + 1): retransmits don't start twice */
            int u = (int)(m - l->m);
            if (u != l->owner || argc < 3) {
                LOG("lobby %u '%s': unit %d isn't the owner, can't start", l->id, l->name, u + 1);
            } else if (atoi(argv[2]) == l->start + 1) {
                l->start++;
                l->ver++;
                LOG("lobby %u '%s': unit %d starts the game (%d)", l->id, l->name, u + 1, l->start);
                push_members(s, l);
                return;
            }
        }
        members_text(l, text, sizeof text);
        send_text(s, from, text);
    }
}

/* ---------------------------------------------------------------- relay */

static void relay_send(LobbyServer *s, const struct sockaddr_in *to, const uint8_t *pkt, int len)
{
    if (!s->delay_ms || !s->q) {
        send_raw(s, to, pkt, len);
        return;
    }
    if (s->qcount == QUEUE_CAP)
        return; /* dropped, like a congested link */
    Delayed *d = &s->q[(s->qhead + s->qcount++) % QUEUE_CAP];
    d->due = ht_now_ms() + (uint64_t)s->delay_ms;
    d->to = *to;
    d->len = len;
    memcpy(d->data, pkt, (size_t)len);
}

static void handle_data(LobbyServer *s, const struct sockaddr_in *from, uint8_t *pkt, int n)
{
    HtEnvelope *e = (HtEnvelope *)pkt;
    Lobby *l;
    Member *m = member_by_token(s, e->token, &l);
    if (!m) {
        if (s->verbose)
            LOG("data with unknown token %08x from %s", e->token, addr_str(from));
        return;
    }
    if (!same_addr(&m->addr, from)) {
        if (s->verbose)
            LOG("data for unit %d from %s, expected %s", (int)(m - l->m) + 1, addr_str(from), addr_str(&m->addr));
        return;
    }
    m->last = ht_now_ms();
    int u = (int)(m - l->m);
    e->unit = (uint8_t)u;
    e->token = 0;
    if (e->dest == HTNET_DEST_ALL) {
        for (int k = 0; k < HTNET_MAX_UNITS; k++)
            if (k != u && l->m[k].used)
                relay_send(s, &l->m[k].addr, pkt, n);
    } else if (e->dest < HTNET_MAX_UNITS && e->dest != u && l->m[e->dest].used) {
        relay_send(s, &l->m[e->dest].addr, pkt, n);
    }
}

static void handle_packet(LobbyServer *s, const struct sockaddr_in *from, uint8_t *pkt, int n)
{
    char line[HTNET_MAX_DATAGRAM + 1];
    const char *info;
    if (ht_oob_parse(pkt, n, line, sizeof line, &info)) {
        handle_oob(s, from, line, info);
        return;
    }
    const HtEnvelope *e = (const HtEnvelope *)pkt;
    if (n >= (int)sizeof *e && n <= (int)(sizeof *e + HTNET_MAX_PAYLOAD) && e->magic == HTNET_MAGIC &&
        e->version == HTNET_VERSION)
        handle_data(s, from, pkt, n);
}

static void sweep(LobbyServer *s, uint64_t now)
{
    for (int i = 0; i < s->max_lobbies; i++) {
        Lobby *l = &s->lobbies[i];
        for (int u = 0; l->used && u < HTNET_MAX_UNITS; u++)
            if (l->m[u].used && now - l->m[u].last > HTNET_TIMEOUT_MS)
                remove_member(s, l, u, "timed out");
    }
}

/* ---------------------------------------------------------------- API */

LobbyServer *lobby_server_open(int mode, int port, int tries, int max_lobbies, LobbyLog log)
{
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa))
        return NULL;
#endif
    LobbyServer *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->sock = INVALID_SOCKET;
    s->mode = mode;
    s->log = log;
    s->max_lobbies = mode == LOBBY_LAN ? 1 : max_lobbies < 1 ? 1 : max_lobbies > MAX_LOBBIES_LIMIT ? MAX_LOBBIES_LIMIT : max_lobbies;
    s->lobbies = calloc((size_t)s->max_lobbies, sizeof *s->lobbies);
    s->next_id = (ht_random32() % 900000u) + 100000u;
    snprintf(s->secret, sizeof s->secret, "%08x%08x", ht_random32(), ht_random32());
    s->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (!s->lobbies || s->sock == INVALID_SOCKET) {
        lobby_server_close(s);
        return NULL;
    }
    int bound = 0;
    for (int k = 0; k < (tries < 1 ? 1 : tries) && !bound; k++) {
        struct sockaddr_in me;
        memset(&me, 0, sizeof me);
        me.sin_family = AF_INET;
        me.sin_port = htons((unsigned short)(port + k));
        me.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(s->sock, (struct sockaddr *)&me, sizeof me) == 0) {
            s->port = port + k;
            bound = 1;
        }
    }
    if (!bound) {
        LOG("lobby server: can't bind UDP %d..%d (error %d)", port, port + tries - 1, ht_sock_error());
        lobby_server_close(s);
        return NULL;
    }
    ht_sock_nonblock(s->sock);
#ifdef _WIN32
    BOOL off = FALSE;
    DWORD ret;
    WSAIoctl(s->sock, SIO_UDP_CONNRESET, &off, sizeof off, NULL, 0, &ret, NULL, NULL);
#endif
    LOG("lobby server (%s) on UDP %d", mode == LOBBY_LAN ? "LAN" : "master", s->port);
    return s;
}

int lobby_server_port(const LobbyServer *s)
{
    return s ? s->port : 0;
}

void lobby_server_set_delay(LobbyServer *s, int ms)
{
    s->delay_ms = ms > 0 ? ms : 0;
    if (s->delay_ms && !s->q)
        s->q = calloc(QUEUE_CAP, sizeof *s->q);
}

void lobby_server_set_verbose(LobbyServer *s, int on)
{
    s->verbose = on;
}

void lobby_server_poll(LobbyServer *s, int timeout_ms)
{
    uint64_t now = ht_now_ms();
    if (s->qcount) {
        uint64_t due = s->q[s->qhead].due;
        int until = due > now ? (int)(due - now) : 0;
        if (until < timeout_ms)
            timeout_ms = until;
    }
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(s->sock, &rd);
    struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    select((int)s->sock + 1, &rd, NULL, NULL, &tv);

    for (;;) {
        uint8_t pkt[HTNET_MAX_DATAGRAM + 64];
        struct sockaddr_in from;
        socklen_t fromlen = sizeof from;
        int n = (int)recvfrom(s->sock, (char *)pkt, sizeof pkt, 0, (struct sockaddr *)&from, &fromlen);
        if (n == SOCKET_ERROR) {
            int err = ht_sock_error();
            if (err == HT_EWOULDBLOCK)
                break;
#ifdef _WIN32
            if (err == WSAECONNRESET || err == WSAEMSGSIZE)
                continue;
#endif
            break;
        }
        handle_packet(s, &from, pkt, n);
    }

    now = ht_now_ms();
    while (s->qcount && s->q[s->qhead].due <= now) {
        Delayed *d = &s->q[s->qhead];
        send_raw(s, &d->to, d->data, d->len);
        s->qhead = (s->qhead + 1) % QUEUE_CAP;
        s->qcount--;
    }
    if (now - s->last_sweep >= 1000) {
        s->last_sweep = now;
        sweep(s, now);
    }
}

void lobby_server_close(LobbyServer *s)
{
    if (!s)
        return;
    for (int i = 0; s->lobbies && s->sock != INVALID_SOCKET && i < s->max_lobbies; i++)
        for (int u = 0; s->lobbies[i].used && u < HTNET_MAX_UNITS; u++)
            if (s->lobbies[i].m[u].used) {
                char text[32];
                snprintf(text, sizeof text, "kicked %08x", s->lobbies[i].m[u].token);
                send_text(s, &s->lobbies[i].m[u].addr, text);
            }
    if (s->sock != INVALID_SOCKET)
        closesocket(s->sock);
    free(s->lobbies);
    free(s->q);
    free(s);
#ifdef _WIN32
    WSACleanup();
#endif
}
