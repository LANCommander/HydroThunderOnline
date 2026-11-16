#pragma once
/* The few socket differences between Winsock and BSD sockets. */
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int socklen_t;
#define ht_sock_error() WSAGetLastError()
#define HT_EWOULDBLOCK WSAEWOULDBLOCK
static inline int ht_sock_nonblock(SOCKET s)
{
    u_long on = 1;
    return ioctlsocket(s, FIONBIO, &on);
}
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define closesocket close
#define ht_sock_error() errno
#define HT_EWOULDBLOCK EWOULDBLOCK
static inline int ht_sock_nonblock(SOCKET s)
{
    return fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
}
#endif
#include <stdio.h>
#include <string.h>

/* Dotted-quad IPv4 text. (Not inet_ntop / inet_pton: Winsock has them only from Vista.) */
static inline const char *ht_ip4_str(struct in_addr a, char *buf, size_t cap)
{
    const unsigned char *b = (const unsigned char *)&a.s_addr;
    snprintf(buf, cap, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return buf;
}

/* 1 if `s` is exactly four decimal parts 0-255. */
static inline int ht_ip4_parse(const char *s, struct in_addr *out)
{
    unsigned char b[4];
    for (int i = 0; i < 4; i++) {
        unsigned v = 0;
        int digits = 0;
        for (; *s >= '0' && *s <= '9' && digits < 4; s++, digits++)
            v = v * 10 + (unsigned)(*s - '0');
        if (!digits || digits > 3 || v > 255 || *s != (i < 3 ? '.' : '\0'))
            return 0;
        s += i < 3;
        b[i] = (unsigned char)v;
    }
    memcpy(&out->s_addr, b, 4);
    return 1;
}
