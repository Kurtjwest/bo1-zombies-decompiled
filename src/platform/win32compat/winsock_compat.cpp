// Winsock entry points on BSD sockets (see include/winsock2.h).
#include <winsock2.h>

#include <errno.h>
#include <signal.h>
#include <string.h>

namespace
{
thread_local int t_wsaError = 0;
int g_startupCount = 0;

int ErrnoToWsa(int e)
{
    switch ( e )
    {
    case 0: return 0;
    case EINTR: return WSAEINTR;
    case EBADF: return WSAEBADF;
    case EACCES: return WSAEACCES;
    case EFAULT: return WSAEFAULT;
    case EINVAL: return WSAEINVAL;
    case EMFILE: return WSAEMFILE;
    case EWOULDBLOCK: return WSAEWOULDBLOCK;
    case EINPROGRESS: return WSAEWOULDBLOCK; // a non-blocking connect() in progress reports WSAEWOULDBLOCK on Windows
    case EALREADY: return WSAEALREADY;
    case ENOTSOCK: return WSAENOTSOCK;
    case EDESTADDRREQ: return WSAEDESTADDRREQ;
    case EMSGSIZE: return WSAEMSGSIZE;
    case EPROTOTYPE: return WSAEPROTOTYPE;
    case ENOPROTOOPT: return WSAENOPROTOOPT;
    case EPROTONOSUPPORT: return WSAEPROTONOSUPPORT;
    case ESOCKTNOSUPPORT: return WSAESOCKTNOSUPPORT;
    case EOPNOTSUPP: return WSAEOPNOTSUPP;
    case EPFNOSUPPORT: return WSAEPFNOSUPPORT;
    case EAFNOSUPPORT: return WSAEAFNOSUPPORT;
    case EADDRINUSE: return WSAEADDRINUSE;
    case EADDRNOTAVAIL: return WSAEADDRNOTAVAIL;
    case ENETDOWN: return WSAENETDOWN;
    case ENETUNREACH: return WSAENETUNREACH;
    case ENETRESET: return WSAENETRESET;
    case ECONNABORTED: return WSAECONNABORTED;
    case ECONNRESET: return WSAECONNRESET;
    case EPIPE: return WSAECONNRESET;
    case ENOBUFS: return WSAENOBUFS;
    case EISCONN: return WSAEISCONN;
    case ENOTCONN: return WSAENOTCONN;
    case ESHUTDOWN: return WSAESHUTDOWN;
    case ETOOMANYREFS: return WSAETOOMANYREFS;
    case ETIMEDOUT: return WSAETIMEDOUT;
    case ECONNREFUSED: return WSAECONNREFUSED;
    case ELOOP: return WSAELOOP;
    case ENAMETOOLONG: return WSAENAMETOOLONG;
    case EHOSTDOWN: return WSAEHOSTDOWN;
    case EHOSTUNREACH: return WSAEHOSTUNREACH;
    case ENOTEMPTY: return WSAENOTEMPTY;
#ifdef EPROCLIM
    case EPROCLIM: return WSAEPROCLIM;
#endif
    case EUSERS: return WSAEUSERS;
    case EDQUOT: return WSAEDQUOT;
    case ESTALE: return WSAESTALE;
    case EREMOTE: return WSAEREMOTE;
    default: return WSAEINVAL;
    }
}
} // namespace

extern "C" {

int WSAStartup(WORD wVersionRequested, LPWSADATA lpWSAData)
{
    if ( lpWSAData )
    {
        memset(lpWSAData, 0, sizeof(*lpWSAData));
        lpWSAData->wVersion = wVersionRequested;
        lpWSAData->wHighVersion = 0x0202;
        strcpy(lpWSAData->szDescription, "BSD sockets");
        strcpy(lpWSAData->szSystemStatus, "Running");
        lpWSAData->iMaxSockets = 0x7FFF;
        lpWSAData->iMaxUdpDg = 0xFFFF;
    }
    if ( g_startupCount++ == 0 )
        signal(SIGPIPE, SIG_IGN); // a send() on a reset TCP connection must return an error, not kill the process
    return 0;
}

int WSACleanup(void)
{
    if ( g_startupCount > 0 )
        --g_startupCount;
    return 0;
}

int WSAGetLastError(void)
{
    // Winsock keeps its own error slot; on BSD the failing call left errno, so translate it on demand. A value set
    // with WSASetLastError wins until the next socket call fails.
    if ( t_wsaError )
    {
        int e = t_wsaError;
        t_wsaError = 0;
        return e;
    }
    return ErrnoToWsa(errno);
}

void WSASetLastError(int iError)
{
    t_wsaError = iError;
    if ( !iError )
        errno = 0;
}

int closesocket(SOCKET s)
{
    return close(s);
}

int ioctlsocket(SOCKET s, long cmd, u_long *argp)
{
    const unsigned long ucmd = (unsigned long)(unsigned int)cmd;
    if ( ucmd == 0x8004667EUL || ucmd == (unsigned long)FIONBIO )
    {
        int flags = fcntl(s, F_GETFL, 0);
        if ( flags < 0 )
            return SOCKET_ERROR;
        // Windows' u_long is 32 bits and callers often pass the address of an int: read only the low 4 bytes
        unsigned int on = 0;
        if ( argp )
            memcpy(&on, argp, sizeof(on));
        flags = on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
        return fcntl(s, F_SETFL, flags) < 0 ? SOCKET_ERROR : 0;
    }
    if ( ucmd == 0x4004667FUL || ucmd == (unsigned long)FIONREAD )
    {
        int n = 0;
        if ( ioctl(s, FIONREAD, &n) < 0 )
            return SOCKET_ERROR;
        if ( argp )
        {
            const unsigned int un = (unsigned int)n;
            memcpy(argp, &un, sizeof(un));
        }
        return 0;
    }
    errno = EINVAL;
    return SOCKET_ERROR;
}

} // extern "C"
