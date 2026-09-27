/*
 * htmaster: the Hydro Thunder lobby master and relay (docs/network.md, "Lobbies").
 *
 *     htmaster [--port 27950] [--max-lobbies 256] [--delay-ms 0] [--verbose]
 *
 * Players list lobbies, create and join them here; all lobby traffic is relayed through this
 * one UDP port. Builds with the port's CMake (target htmaster) on Windows and Linux
 * (docs/network.md, "Running a master").
 */
#include "lobby.h"

#include "../common/htnet_proto.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void log_line(const char *fmt, ...)
{
    time_t t = time(NULL);
    char stamp[32];
    strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", localtime(&t));
    printf("%s ", stamp);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

int main(int argc, char **argv)
{
    int port = HTNET_MASTER_PORT, max_lobbies = 256, delay = 0, verbose = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc)
            port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--max-lobbies") && i + 1 < argc)
            max_lobbies = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--delay-ms") && i + 1 < argc)
            delay = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--verbose"))
            verbose = 1;
        else if (!strcmp(argv[i], "--version")) {
            printf("htmaster %s\n", HYDRO_VERSION);
            return 0;
        } else {
            fprintf(stderr, "usage: htmaster [--port %d] [--max-lobbies 256] [--delay-ms 0] [--verbose] [--version]\n",
                    HTNET_MASTER_PORT);
            return 2;
        }
    }
    LobbyServer *s = lobby_server_open(LOBBY_MASTER, port, 1, max_lobbies, log_line);
    if (!s) {
        fprintf(stderr, "htmaster: can't open UDP port %d\n", port);
        return 1;
    }
    lobby_server_set_delay(s, delay);
    lobby_server_set_verbose(s, verbose);
    log_line("htmaster %s on UDP port %d", HYDRO_VERSION, port);
    if (delay)
        log_line("relaying with %d ms extra latency", delay);
    for (;;)
        lobby_server_poll(s, 250);
}
