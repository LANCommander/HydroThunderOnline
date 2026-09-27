#pragma once
/*
 * Lobby client (docs/network.md, "Lobbies"): browses LAN and internet lobbies, creates and joins
 * them, and keeps the membership alive. While in a lobby the game's link runs over the relay
 * (hle_net.c) with the unit the lobby assigned, from the next START THE GAME on.
 *
 * Everything runs on the game thread: lobby_poll() is called by the operator menu every frame and
 * by netdriver_Recieve during play.
 */
#include <winsock2.h>
#include <stdint.h>

#include "../common/htnet_proto.h"

#define LOBBY_MAX_LIST 32
/* Units the game's link supports (NET_COMM / NET_LINK). */
#define LOBBY_GAME_UNITS 16

typedef struct LobbyEntry {
    int lan;    /* found by LAN broadcast (else listed by the master) */
    int direct; /* reached by DIRECT CONNECT (a lobby hosted on a PC, by address) */
    char host[128]; /* the DNS name used to reach `relay` ("" = an address), sent as \host\ */
    char name[HTNET_LOBBY_LEN];
    int clients, max, needpass;
    uint32_t id;
    struct sockaddr_in relay; /* where the lobby lives: the LAN host or the master */
    unsigned ping;            /* ms */
} LobbyEntry;

/* LOBBY_NEEDPASS: DIRECT CONNECT found a lobby with a password: lobby_join(lobby_found(), password)
 * or lobby_leave(). */
enum { LOBBY_IDLE, LOBBY_BUSY, LOBBY_IN, LOBBY_FAILED, LOBBY_NEEDPASS };

/* A chat line; unit -1 = a notice from this PC (joins, leaves, errors). */
#define LOBBY_CHAT_LINES 32
#define LOBBY_SAY_QUEUE 4
typedef struct LobbyChatLine {
    int unit;
    char name[HTNET_NAME_LEN];
    char text[HTNET_CHAT_LEN + 32];
} LobbyChatLine;

void lobby_client_init(void);
void lobby_autostart(void); /* HYDRO_LOBBY (testing) */
/* Receive, retransmit and keep alive. */
void lobby_poll(void);

void lobby_browse(int clear); /* asks the LAN and the master (clear: forget the current list first) */
int lobby_list(const LobbyEntry **out);

/* Starts a request; lobby_state() says when it has finished. */
void lobby_create(int lan, const char *name, int max, const char *password, int delay);
void lobby_join(const LobbyEntry *e, const char *password);
void lobby_leave(void);
/* DIRECT CONNECT: "host[:port]" (port HTNET_LAN_PORT, or the SRV record's) of a PC hosting a lobby.
 * Asks it for its lobby and joins it, or stops at LOBBY_NEEDPASS. */
void lobby_connect(const char *address);
const LobbyEntry *lobby_found(void);
/* Hosting a LAN lobby: "ip:port" others can DIRECT CONNECT to (this PC's first LAN address). */
int lobby_host_address(char *out, size_t cap);

/* Chat in the current lobby: the lines, oldest first; the version changes with every new line. */
int lobby_chat(const LobbyChatLine **out);
unsigned lobby_chat_version(void);
/* Sends a line (queued, resent until the lobby has it). 0 if not in a lobby or the queue is full. */
int lobby_say(const char *text);

int lobby_state(void);
const char *lobby_message(void); /* what happened last (for the menu) */

/* In a lobby: 1, and its unit (0-7), name and whether it's a LAN lobby. */
int lobby_active(int *unit);
const char *lobby_name(void);
int lobby_is_lan(void);
int lobby_is_direct(void);
int lobby_max(void);
int lobby_input_delay(void); /* frames, fixed by the lobby's creator */
/* In a lobby: the game options its creator set (they override the operator settings in RAM at
 * every gameinit, game/operator.c); NULL otherwise. */
const HtGameOpts *lobby_game_opts(void);
/* An INPUT DELAY setting (0 = auto) as frames for a new lobby: 1 on a LAN, 3 over the internet. */
int lobby_delay_for(int lan, int setting);
/* The members' names by unit ("" = free); returns the unit mask. */
unsigned lobby_members(char names[HTNET_MAX_UNITS][HTNET_NAME_LEN]);
/* START GAME: 1 if this unit may start the lobby's game (it created the lobby, or is the lowest
 * unit left after the creator went). */
int lobby_is_owner(void);
int lobby_owner(void); /* the owner's unit, -1 if unknown */
/* The owner starts the game: tells the lobby (retransmitted until it's seen) so every member's
 * operator menu leaves as if START THE GAME was picked. 0 if this unit isn't the owner. */
int lobby_start_game(void);
/* 1 once, after another unit started the lobby's game (menus() then leaves the operator menu). */
int lobby_take_start(void);
/* PLAYER NAME changed: tell the lobby now (it also goes with every keepalive). */
void lobby_name_changed(void);

/* hle_net.c, relay transport: game packets go out through the lobby's socket. */
typedef void (*LobbyDataHandler)(uint8_t *pkt, int n, const struct sockaddr_in *from);
void lobby_set_data_handler(LobbyDataHandler h);
int lobby_send_data(const void *pkt, int n);
uint32_t lobby_token(void);

/* Every up IPv4 adapter's subnet broadcast (255.255.255.255 if none); returns the count. */
int net_lan_broadcasts(uint32_t *out, int max);
