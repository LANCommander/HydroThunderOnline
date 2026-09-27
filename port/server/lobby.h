#pragma once
/*
 * Lobby server and relay (protocol: common/htnet_proto.h). One UDP socket; lobbies, members and
 * forwarding in fixed tables. Used by the standalone master (htmaster.c, LOBBY_MASTER) and, on a
 * thread, by a PC hosting a LAN lobby (host/lobby_client.c, LOBBY_LAN).
 */
#include <stdint.h>

typedef struct LobbyServer LobbyServer;

enum { LOBBY_MASTER, LOBBY_LAN };

typedef void (*LobbyLog)(const char *fmt, ...);

/* Binds `port` (or the next free of port..port+tries-1). NULL on failure. `log` may be NULL. */
LobbyServer *lobby_server_open(int mode, int port, int tries, int max_lobbies, LobbyLog log);
int lobby_server_port(const LobbyServer *s);
/* Handles everything pending, waiting up to `timeout_ms` for traffic. */
void lobby_server_poll(LobbyServer *s, int timeout_ms);
void lobby_server_close(LobbyServer *s);
/* Testing: hold every relayed game packet this long (simulated internet latency). */
void lobby_server_set_delay(LobbyServer *s, int ms);
void lobby_server_set_verbose(LobbyServer *s, int on);
