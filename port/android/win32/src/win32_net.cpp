// Android Win32 layer: Winsock over BSD sockets.
#include "win32_internal.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <cerrno>
#include <csignal>

using namespace w32;

namespace {
int WsaFromErrno(int e)
{
    switch (e)
    {
    case 0: return 0;
    case EINTR: return WSAEINTR;
    case EBADF: return WSAEBADF;
    case EACCES: return WSAEACCES;
    case EFAULT: return WSAEFAULT;
    case EINVAL: return WSAEINVAL;
    case EMFILE: return WSAEMFILE;
#if EAGAIN != EWOULDBLOCK
    case EAGAIN:
#endif
    case EWOULDBLOCK: return WSAEWOULDBLOCK;
    case EINPROGRESS: return WSAEWOULDBLOCK;   // Windows reports a pending connect as WOULDBLOCK
    case EALREADY: return WSAEALREADY;
    case ENOTSOCK: return WSAENOTSOCK;
    case EDESTADDRREQ: return WSAEDESTADDRREQ;
    case EMSGSIZE: return WSAEMSGSIZE;
    case EPROTOTYPE: return WSAEPROTOTYPE;
    case ENOPROTOOPT: return WSAENOPROTOOPT;
    case EPROTONOSUPPORT: return WSAEPROTONOSUPPORT;
    case EOPNOTSUPP: return WSAEOPNOTSUPP;
    case EAFNOSUPPORT: return WSAEAFNOSUPPORT;
    case EADDRINUSE: return WSAEADDRINUSE;
    case EADDRNOTAVAIL: return WSAEADDRNOTAVAIL;
    case ENETDOWN: return WSAENETDOWN;
    case ENETUNREACH: return WSAENETUNREACH;
    case ENETRESET: return WSAENETRESET;
    case ECONNABORTED: return WSAECONNABORTED;
    case ECONNRESET: return WSAECONNRESET;
    case ENOBUFS: return WSAENOBUFS;
    case EISCONN: return WSAEISCONN;
    case ENOTCONN: return WSAENOTCONN;
    case ESHUTDOWN: return WSAESHUTDOWN;
    case ETIMEDOUT: return WSAETIMEDOUT;
    case ECONNREFUSED: return WSAECONNREFUSED;
    case EHOSTDOWN: return WSAEHOSTDOWN;
    case EHOSTUNREACH: return WSAEHOSTUNREACH;
    default: return WSAEINVAL;
    }
}
thread_local int t_wsaOverride = -1;
} // namespace

extern "C" int WSAStartup(WORD version, LPWSADATA data)
{
    if (data)
    {
        std::memset(data, 0, sizeof(*data));
        data->wVersion = version; data->wHighVersion = MAKEWORD(2, 2);
        std::strcpy(data->szDescription, "WinSock 2.0 (BSD sockets)");
    }
    signal(SIGPIPE, SIG_IGN);
    return 0;
}
extern "C" int WSACleanup(void) { return 0; }
extern "C" int WSAGetLastError(void)
{
    if (t_wsaOverride >= 0) { const int v = t_wsaOverride; t_wsaOverride = -1; return v; }
    return WsaFromErrno(errno);
}
extern "C" void WSASetLastError(int error) { t_wsaOverride = error; }
extern "C" int closesocket(SOCKET s) { return close(s); }
extern "C" int ioctlsocket(SOCKET s, long cmd, unsigned long* arg)
{
    if (cmd == long(FIONBIO))
    {
        const int flags = fcntl(s, F_GETFL, 0);
        if (flags < 0) return SOCKET_ERROR;
        return fcntl(s, F_SETFL, (arg && *arg) ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK)) < 0 ? SOCKET_ERROR : 0;
    }
    if (cmd == long(0x4004667F) /* FIONREAD */)
    {
        int n = 0;
        if (ioctl(s, FIONREAD, &n) != 0) return SOCKET_ERROR;
        if (arg) *arg = (unsigned long)n;
        return 0;
    }
    errno = EINVAL;
    return SOCKET_ERROR;
}
extern "C" int WSAIoctl(SOCKET s, DWORD code, LPVOID in, DWORD inSize, LPVOID, DWORD, LPDWORD returned, LPWSAOVERLAPPED, void*)
{
    if (returned) *returned = 0;
    // SIO_UDP_CONNRESET / SIO_UDP_NETRESET: Linux UDP sockets do not report ICMP errors on
    // unconnected sockets anyway. Keep-alive values map to TCP_KEEP*.
    if (code == SIO_UDP_CONNRESET || code == SIO_UDP_NETRESET) return 0;
    if (code == SIO_KEEPALIVE_VALS && in && inSize >= sizeof(tcp_keepalive))
    {
        const auto* k = static_cast<const tcp_keepalive*>(in);
        int on = k->onoff ? 1 : 0;
        setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
        int idle = int(k->keepalivetime / 1000), intvl = int(k->keepaliveinterval / 1000);
        if (idle > 0) setsockopt(s, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
        if (intvl > 0) setsockopt(s, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
        return 0;
    }
    errno = EOPNOTSUPP;
    return SOCKET_ERROR;
}
extern "C" int WSAPoll(WSAPOLLFD* fds, ULONG count, INT timeout) { return poll(fds, nfds_t(count), timeout); }
extern "C" WSAEVENT WSACreateEvent(void) { return CreateEventW(nullptr, TRUE, FALSE, nullptr); }
extern "C" BOOL WSACloseEvent(WSAEVENT e) { return CloseHandle(e); }
extern "C" BOOL WSASetEvent(WSAEVENT e) { return SetEvent(e); }
extern "C" BOOL WSAResetEvent(WSAEVENT e) { return ResetEvent(e); }
extern "C" int WSAEventSelect(SOCKET, WSAEVENT, long) { errno = EOPNOTSUPP; return SOCKET_ERROR; }
extern "C" DWORD WSAWaitForMultipleEvents(DWORD count, const WSAEVENT* events, BOOL all, DWORD ms, BOOL) { return WaitForMultipleObjects(count, events, all, ms); }
extern "C" int InetPtonW(int family, const wchar_t* src, void* dst) { return inet_pton(family, Narrow(src).c_str(), dst); }
extern "C" const wchar_t* InetNtopW(int family, const void* src, wchar_t* dst, size_t size)
{
    char b[64];
    if (!inet_ntop(family, src, b, sizeof(b))) return nullptr;
    const std::wstring w = Widen(b);
    if (w.size() >= size) return nullptr;
    std::wmemcpy(dst, w.c_str(), w.size() + 1);
    return dst;
}
extern "C" int GetAddrInfoW(const wchar_t*, const wchar_t*, const ADDRINFOW*, PADDRINFOW* result) { if (result) *result = nullptr; return EAI_FAIL; }
extern "C" void FreeAddrInfoW(PADDRINFOW) {}
