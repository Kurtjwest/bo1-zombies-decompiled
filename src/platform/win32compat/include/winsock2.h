// Winsock on BSD sockets for the macOS port. Socket handles are file descriptors; WSAGetLastError() reports
// Winsock error numbers (WSAEWOULDBLOCK, ...) translated from errno, which the engine compares against.
#pragma once
#include <windows.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>

typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
typedef struct sockaddr SOCKADDR, *PSOCKADDR, *LPSOCKADDR;
typedef struct sockaddr_in SOCKADDR_IN, *PSOCKADDR_IN, *LPSOCKADDR_IN;
typedef struct sockaddr_in6 SOCKADDR_IN6, *PSOCKADDR_IN6;
typedef struct sockaddr_storage SOCKADDR_STORAGE, *PSOCKADDR_STORAGE;
typedef struct in_addr IN_ADDR, *PIN_ADDR, *LPIN_ADDR;
typedef struct hostent HOSTENT, *PHOSTENT, *LPHOSTENT;
typedef struct timeval TIMEVAL, *PTIMEVAL, *LPTIMEVAL;
typedef struct linger LINGER;
typedef unsigned short ADDRESS_FAMILY;
typedef struct addrinfo ADDRINFOA, *PADDRINFOA;

#define WSADESCRIPTION_LEN 256
#define WSASYS_STATUS_LEN 128
typedef struct WSAData {
    WORD wVersion;
    WORD wHighVersion;
    char szDescription[WSADESCRIPTION_LEN + 1];
    char szSystemStatus[WSASYS_STATUS_LEN + 1];
    unsigned short iMaxSockets;
    unsigned short iMaxUdpDg;
    char *lpVendorInfo;
} WSADATA, *LPWSADATA;

#define WSABASEERR 10000
#define WSAEINTR (WSABASEERR + 4)
#define WSAEBADF (WSABASEERR + 9)
#define WSAEACCES (WSABASEERR + 13)
#define WSAEFAULT (WSABASEERR + 14)
#define WSAEINVAL (WSABASEERR + 22)
#define WSAEMFILE (WSABASEERR + 24)
#define WSAEWOULDBLOCK (WSABASEERR + 35)
#define WSAEINPROGRESS (WSABASEERR + 36)
#define WSAEALREADY (WSABASEERR + 37)
#define WSAENOTSOCK (WSABASEERR + 38)
#define WSAEDESTADDRREQ (WSABASEERR + 39)
#define WSAEMSGSIZE (WSABASEERR + 40)
#define WSAEPROTOTYPE (WSABASEERR + 41)
#define WSAENOPROTOOPT (WSABASEERR + 42)
#define WSAEPROTONOSUPPORT (WSABASEERR + 43)
#define WSAESOCKTNOSUPPORT (WSABASEERR + 44)
#define WSAEOPNOTSUPP (WSABASEERR + 45)
#define WSAEPFNOSUPPORT (WSABASEERR + 46)
#define WSAEAFNOSUPPORT (WSABASEERR + 47)
#define WSAEADDRINUSE (WSABASEERR + 48)
#define WSAEADDRNOTAVAIL (WSABASEERR + 49)
#define WSAENETDOWN (WSABASEERR + 50)
#define WSAENETUNREACH (WSABASEERR + 51)
#define WSAENETRESET (WSABASEERR + 52)
#define WSAECONNABORTED (WSABASEERR + 53)
#define WSAECONNRESET (WSABASEERR + 54)
#define WSAENOBUFS (WSABASEERR + 55)
#define WSAEISCONN (WSABASEERR + 56)
#define WSAENOTCONN (WSABASEERR + 57)
#define WSAESHUTDOWN (WSABASEERR + 58)
#define WSAETOOMANYREFS (WSABASEERR + 59)
#define WSAETIMEDOUT (WSABASEERR + 60)
#define WSAECONNREFUSED (WSABASEERR + 61)
#define WSAELOOP (WSABASEERR + 62)
#define WSAENAMETOOLONG (WSABASEERR + 63)
#define WSAEHOSTDOWN (WSABASEERR + 64)
#define WSAEHOSTUNREACH (WSABASEERR + 65)
#define WSAENOTEMPTY (WSABASEERR + 66)
#define WSAEPROCLIM (WSABASEERR + 67)
#define WSAEUSERS (WSABASEERR + 68)
#define WSAEDQUOT (WSABASEERR + 69)
#define WSAESTALE (WSABASEERR + 70)
#define WSAEREMOTE (WSABASEERR + 71)
#define WSASYSNOTREADY (WSABASEERR + 91)
#define WSAVERNOTSUPPORTED (WSABASEERR + 92)
#define WSANOTINITIALISED (WSABASEERR + 93)
#define WSAEDISCON (WSABASEERR + 101)
#define WSAHOST_NOT_FOUND (WSABASEERR + 1001)
#define WSATRY_AGAIN (WSABASEERR + 1002)
#define WSANO_RECOVERY (WSABASEERR + 1003)
#define WSANO_DATA (WSABASEERR + 1004)

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#define SD_RECEIVE 0
#define SD_SEND 1
#define SD_BOTH 2
#define WIN_FIONBIO ((long)0x8004667E)
#define WIN_FIONREAD ((long)0x4004667F)

#ifdef __cplusplus
extern "C" {
#endif
int WSAStartup(WORD wVersionRequested, LPWSADATA lpWSAData);
int WSACleanup(void);
int WSAGetLastError(void);
void WSASetLastError(int iError);
int closesocket(SOCKET s);
// Accepts both the BSD request codes and the Winsock numeric ones (0x8004667E FIONBIO, 0x4004667F FIONREAD).
int ioctlsocket(SOCKET s, long cmd, u_long *argp);
// What FD_ISSET expands to in the Windows SDK; decompiled code calls it directly.
static inline int __WSAFDIsSet(SOCKET fd, fd_set *set) { return FD_ISSET(fd, set) ? 1 : 0; }
#ifdef __cplusplus
}

// Winsock signatures take 'int *' address lengths where BSD takes socklen_t *; overloads keep the engine's calls.
static inline int recvfrom(SOCKET s, char *buf, int len, int flags, struct sockaddr *from, int *fromlen)
{
    socklen_t l = fromlen ? (socklen_t)*fromlen : 0;
    ssize_t r = ::recvfrom(s, (void *)buf, (size_t)len, flags, from, fromlen ? &l : nullptr);
    if (fromlen)
        *fromlen = (int)l;
    return (int)r;
}
static inline SOCKET accept(SOCKET s, struct sockaddr *addr, int *addrlen)
{
    socklen_t l = addrlen ? (socklen_t)*addrlen : 0;
    SOCKET r = ::accept(s, addr, addrlen ? &l : nullptr);
    if (addrlen)
        *addrlen = (int)l;
    return r;
}
static inline int getsockname(SOCKET s, struct sockaddr *name, int *namelen)
{
    socklen_t l = namelen ? (socklen_t)*namelen : 0;
    int r = ::getsockname(s, name, namelen ? &l : nullptr);
    if (namelen)
        *namelen = (int)l;
    return r;
}
static inline int getpeername(SOCKET s, struct sockaddr *name, int *namelen)
{
    socklen_t l = namelen ? (socklen_t)*namelen : 0;
    int r = ::getpeername(s, name, namelen ? &l : nullptr);
    if (namelen)
        *namelen = (int)l;
    return r;
}
static inline int getsockopt(SOCKET s, int level, int optname, char *optval, int *optlen)
{
    socklen_t l = optlen ? (socklen_t)*optlen : 0;
    int r = ::getsockopt(s, level, optname, (void *)optval, optlen ? &l : nullptr);
    if (optlen)
        *optlen = (int)l;
    return r;
}
static inline int send(SOCKET s, const char *buf, int len, int flags) { return (int)::send(s, (const void *)buf, (size_t)len, flags | MSG_NOSIGNAL); }
static inline int recv(SOCKET s, char *buf, int len, int flags) { return (int)::recv(s, (void *)buf, (size_t)len, flags); }
static inline int sendto(SOCKET s, const char *buf, int len, int flags, const struct sockaddr *to, int tolen)
{
    return (int)::sendto(s, (const void *)buf, (size_t)len, flags, to, (socklen_t)tolen);
}
#endif
