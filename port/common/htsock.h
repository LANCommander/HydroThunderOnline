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
