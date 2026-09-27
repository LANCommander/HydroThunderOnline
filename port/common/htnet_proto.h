#pragma once
/*
 * Lobby / relay protocol, shared by hydro.exe (host/hle_net.c, host/lobby_client.c) and the
 * standalone master server (server/htmaster.c). See docs/network.md, "Lobbies".
 *
 * Control messages follow Quake 3 / dpmaster: an out-of-band datagram is 0xffffffff, a text command
 * line, then (for most replies) "\n" and an infostring "\key\value\key\value".
 *
 *   getinfo <challenge> [lobby]            -> infoResponse\n\challenge\..\hostname\..\gamename\HydroThunder
 *                                             \protocol\N\clients\n\sv_maxclients\m\g_needpass\0|1\lobby\id
 *   getservers HydroThunder <proto> [full] [empty]
 *                                          -> getserversResponse, then "\" + u32 lobby id (big endian)
 *                                             + u16 0 per lobby, then "\EOT\0\0\0" (Q3's 6-byte records;
 *                                             lobbies live on the relay, so the record names a lobby,
 *                                             not an address)
 *   getserversInfo <proto> <challenge>     -> getserversInfoResponse <challenge> <page> <last>\n, then
 *                                             one infostring per line (pages of <= 1400 bytes)
 *   createLobby <nonce>\n\hostname\..\sv_maxclients\2-16\delay\1-6\verifier\<hex or empty>
 *                         \protocol\N\name\<player>\gopts\<game options>
 *                                          -> createResponse <nonce>\n\lobby\id\unit\0\token\<hex>
 *                                             \hostname\..\sv_maxclients\..\delay\..\gopts\..
 *                                             or joinRejected <nonce> <reason>
 *   join <lobby> <nonce>                   -> challengeResponse <nonce> <salt>
 *   join <lobby> <nonce> <proof>\n\name\..\protocol\N
 *                                          -> joinResponse <nonce>\n\lobby\..\unit\..\token\..\hostname\..
 *                                             \sv_maxclients\..\delay\..\gopts\..
 *                                             (gopts: ht_gameopts_encode; the server passes it on as is)
 *                                             or joinRejected <nonce> <full|password|protocol|closed>
 *   keepalive <token> <seq>[\n\name\..]    -> keepaliveAck <seq> <members version> <chat id>, or kicked <token>
 *                                             (a changed name renames the member and pushes members)
 *   getmembers <token>                     -> members\n\lobby\..\hostname\..\clients\..\sv_maxclients\..
 *                                             \ver\..\owner\<unit>\start\<n>\chat\<id>\n<unit>\<name>... (n0-n15;
 *                                             also pushed on every change)
 *   startGame <token> <n>                  -> (from the owner, n = start + 1) start = n, members pushed to
 *                                             everyone; otherwise members to the sender. Every member's
 *                                             operator menu leaves when it sees start go up (START GAME)
 *   leave <token>                          -> leaveAck
 *   say <token> <seq>\n\text\..            -> sayAck <seq> (resent until acked; a seq the member already
 *                                             sent isn't added twice). The line goes to every member as
 *                                             chat push\n\id\<n>\unit\..\name\..\text\..
 *   getchat <token> <since>                -> chat log\n, then one line per chat line with id > since,
 *                                             oldest first (the lobby keeps the last HTNET_CHAT_KEEP).
 *                                             keepaliveAck's third word and members' \chat\ are the
 *                                             last chat id, so a lost push is fetched again
 *
 * Clients add \host\<the DNS name they used> to createLobby, join, getinfo and getserversInfo, so a
 * proxy in front of several servers on one address can route by name (like Minecraft's handshake).
 * A name typed without a port is looked up as SRV _hydrothunder._udp.<name> first (lobby_client.c).
 *
 * Passwords: verifier = sha256hex("HydroThunder:" + password), empty for none; proof =
 * sha256hex(salt + verifier). The password itself never leaves the PC.
 *
 * Game traffic is an HtEnvelope followed by the game's NET_LINK packet (<= 0x200 bytes). On a relay
 * the sender fills in its token and the destination unit; the relay checks the token, rewrites
 * `unit` to the sender's lobby unit, clears the token and forwards to `dest` (or to every other
 * member). Classic direct links (no lobby) send token 0.
 */
#include <stddef.h>
#include <stdint.h>

#define HTNET_GAMENAME "HydroThunder"
#define HTNET_PROTOCOL 1 /* lobby protocol; also gates the game's link format (16 units) */
#define HTNET_MAX_UNITS 16
#define HTNET_MASTER_PORT 27950
#define HTNET_LAN_PORT 27960 /* LAN lobby hosts listen on 27960..27963 */
#define HTNET_LAN_PORTS 4
#define HTNET_MAGIC 0x544e5448u /* "HTNT" */
#define HTNET_VERSION 2
#define HTNET_DEST_ALL 0xff
#define HTNET_MAX_PAYLOAD 0x200
#define HTNET_MAX_DATAGRAM 1400
#define HTNET_KEEPALIVE_MS 3000
#define HTNET_TIMEOUT_MS 15000
#define HTNET_NAME_LEN 16  /* player name, including the NUL */
#define HTNET_LOBBY_LEN 24 /* lobby name, including the NUL */
#define HTNET_CHAT_LEN 64  /* chat line, including the NUL */
#define HTNET_CHAT_KEEP 16 /* chat lines a lobby keeps for getchat */
#define HTNET_SRV_PREFIX "_hydrothunder._udp."

#pragma pack(push, 1)
typedef struct HtEnvelope {
    uint32_t magic;
    uint8_t version;
    uint8_t unit; /* sender's unit (the relay overwrites it with the lobby's) */
    uint16_t seq;
    uint32_t nonce; /* per-run random: echo and duplicate filtering */
    uint32_t token; /* relay membership; 0 on direct links and on relayed copies */
    uint8_t dest;   /* unit, or HTNET_DEST_ALL */
    uint8_t pad[3];
} HtEnvelope;
#pragma pack(pop)

/* Out-of-band: 1 and the command text (NUL-terminated in `buf`), 0 if `pkt` isn't one. `*info` is
 * the infostring after the first "\n" (or "" when there is none). */
int ht_oob_parse(const uint8_t *pkt, int n, char *buf, size_t cap, const char **info);
/* Builds 0xffffffff + text into `out`; returns the length. */
int ht_oob_build(uint8_t *out, size_t cap, const char *text);

/* Infostrings: "\key\value...". */
int ht_info_get(const char *info, const char *key, char *out, size_t n);
int ht_info_get_int(const char *info, const char *key, int def);
void ht_info_set(char *info, size_t cap, const char *key, const char *value);
void ht_info_set_int(char *info, size_t cap, const char *key, int value);
/* Printable ASCII only, without the separators \ " ; and newlines. */
void ht_sanitize(char *s);

/* Game options a lobby's creator picks for every member (docs/network.md, "Game options"): the
 * operator adjustments each unit's race runs with while it's in the lobby, in place of its CMOS
 * ones. Tracks are the game's numbering (0 GRAVEYARD .. 12). Every field is an int, in the wire
 * order. */
#define HTNET_TRACKS 13
typedef struct HtGameOpts {
    int freeplay, allboats, alltracks;                  /* OFF/ON */
    int t_track, t_boat, t_hiscore, t_continue;         /* selection times, seconds (10-60) */
    int limiter, limit_pct;                             /* FREE RACE LIMITER, LIMIT FREE RACES TO (5-50) */
    int track_diff[HTNET_TRACKS], ai_diff[HTNET_TRACKS]; /* 1-100 */
} HtGameOpts;
#define HTNET_GAMEOPTS_INTS ((int)(sizeof(HtGameOpts) / sizeof(int)))

/* The factory defaults (the game's operator settings defaults). */
void ht_gameopts_defaults(HtGameOpts *o);
/* Clamps every field to its range. */
void ht_gameopts_clamp(HtGameOpts *o);
/* "1,<35 comma-separated ints>" (1 = format). */
void ht_gameopts_encode(const HtGameOpts *o, char *out, size_t cap);
/* 1 and the clamped options, 0 if `s` isn't a known format. */
int ht_gameopts_decode(const char *s, HtGameOpts *o);

uint64_t ht_now_ms(void);
uint32_t ht_random32(void); /* never 0 */
