// Winsock over BSD sockets.
#pragma once
#include <windows.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>

typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
typedef struct sockaddr SOCKADDR, *PSOCKADDR, *LPSOCKADDR;
typedef struct sockaddr_in SOCKADDR_IN, *PSOCKADDR_IN;
typedef struct sockaddr_in6 SOCKADDR_IN6, *PSOCKADDR_IN6;
typedef struct sockaddr_storage SOCKADDR_STORAGE, *PSOCKADDR_STORAGE;
typedef struct in_addr IN_ADDR;
typedef struct in6_addr IN6_ADDR;
typedef struct addrinfo ADDRINFOA, *PADDRINFOA;
typedef struct hostent HOSTENT;
typedef struct pollfd WSAPOLLFD;
typedef struct timeval TIMEVAL;
typedef struct linger LINGER;
typedef int socklen_t_shim;
typedef unsigned short ADDRESS_FAMILY;
typedef struct WSAData { WORD wVersion, wHighVersion; char szDescription[257]; char szSystemStatus[129]; unsigned short iMaxSockets, iMaxUdpDg; char* lpVendorInfo; } WSADATA, *LPWSADATA;
typedef struct _WSABUF { ULONG len; CHAR* buf; } WSABUF, *LPWSABUF;
typedef struct _WSAOVERLAPPED { DWORD Internal, InternalHigh, Offset, OffsetHigh; HANDLE hEvent; } WSAOVERLAPPED, *LPWSAOVERLAPPED;
typedef HANDLE WSAEVENT;
#define WSA_INVALID_EVENT ((WSAEVENT)nullptr)
#define SD_RECEIVE SHUT_RD
#define SD_SEND SHUT_WR
#define SD_BOTH SHUT_RDWR
#ifndef INADDR_NONE
#define INADDR_NONE 0xffffffff
#endif
#define IOC_VENDOR 0x18000000
#ifndef IOC_IN
#define IOC_IN 0x80000000
#endif
#define _WSAIOW(x, y) (IOC_IN | (x) | (y))
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#define SIO_UDP_NETRESET _WSAIOW(IOC_VENDOR, 15)
#define SIO_KEEPALIVE_VALS _WSAIOW(IOC_VENDOR, 4)
#define SO_EXCLUSIVEADDRUSE ((int)(~SO_REUSEADDR))
#define SO_CONDITIONAL_ACCEPT 0x3002
#define SO_DONTLINGER ((int)(~SO_LINGER))
#define TCP_KEEPCNT_SHIM TCP_KEEPCNT
#define IPV6_PROTECTION_LEVEL 23
#define PROTECTION_LEVEL_UNRESTRICTED 10
#define POLLRDNORM_SHIM POLLRDNORM

#define WSABASEERR 10000
#define WSAEINTR 10004
#define WSAEBADF 10009
#define WSAEACCES 10013
#define WSAEFAULT 10014
#define WSAEINVAL 10022
#define WSAEMFILE 10024
#define WSAEWOULDBLOCK 10035
#define WSAEINPROGRESS 10036
#define WSAEALREADY 10037
#define WSAENOTSOCK 10038
#define WSAEDESTADDRREQ 10039
#define WSAEMSGSIZE 10040
#define WSAEPROTOTYPE 10041
#define WSAENOPROTOOPT 10042
#define WSAEPROTONOSUPPORT 10043
#define WSAEOPNOTSUPP 10045
#define WSAEAFNOSUPPORT 10047
#define WSAEADDRINUSE 10048
#define WSAEADDRNOTAVAIL 10049
#define WSAENETDOWN 10050
#define WSAENETUNREACH 10051
#define WSAENETRESET 10052
#define WSAECONNABORTED 10053
#define WSAECONNRESET 10054
#define WSAENOBUFS 10055
#define WSAEISCONN 10056
#define WSAENOTCONN 10057
#define WSAESHUTDOWN 10058
#define WSAETIMEDOUT 10060
#define WSAECONNREFUSED 10061
#define WSAEHOSTDOWN 10064
#define WSAEHOSTUNREACH 10065
#define WSANOTINITIALISED 10093
#define WSAEDISCON 10101
#define WSAHOST_NOT_FOUND 11001
#define WSATRY_AGAIN 11002
#define WSANO_RECOVERY 11003
#define WSANO_DATA 11004
#define WSA_IO_PENDING ERROR_IO_PENDING
#define WSA_WAIT_FAILED WAIT_FAILED
#define WSA_WAIT_TIMEOUT WAIT_TIMEOUT
#define WSA_WAIT_EVENT_0 WAIT_OBJECT_0
#define WSA_INFINITE INFINITE
#define FD_READ 0x01
#define FD_WRITE 0x02
#define FD_ACCEPT 0x08
#define FD_CONNECT 0x10
#define FD_CLOSE 0x20

extern "C" int WSAStartup(WORD version, LPWSADATA data);
extern "C" int WSACleanup(void);
extern "C" int WSAGetLastError(void);
extern "C" void WSASetLastError(int error);
extern "C" int closesocket(SOCKET s);
extern "C" int ioctlsocket(SOCKET s, long cmd, unsigned long* arg);
extern "C" int WSAIoctl(SOCKET s, DWORD code, LPVOID in, DWORD inSize, LPVOID out, DWORD outSize, LPDWORD returned, LPWSAOVERLAPPED ov, void* done);
extern "C" int WSAPoll(WSAPOLLFD* fds, ULONG count, INT timeout);
extern "C" WSAEVENT WSACreateEvent(void);
extern "C" BOOL WSACloseEvent(WSAEVENT e);
extern "C" BOOL WSASetEvent(WSAEVENT e);
extern "C" BOOL WSAResetEvent(WSAEVENT e);
extern "C" int WSAEventSelect(SOCKET s, WSAEVENT e, long events);
extern "C" DWORD WSAWaitForMultipleEvents(DWORD count, const WSAEVENT* events, BOOL all, DWORD ms, BOOL alertable);
#undef FIONBIO
#define FIONBIO 0x8004667E
#define MAKEWORD_WSA(a, b) MAKEWORD(a, b)
// recv/send take char* on Windows; both forms work with the POSIX void* ones.
// Windows' send() never raises SIGPIPE; the shim ignores SIGPIPE process-wide at start.

// Windows passes lengths as int*; POSIX wants socklen_t*.
inline ssize_t recvfrom(SOCKET s, char* buf, int len, int flags, sockaddr* from, int* fromlen) { socklen_t l = fromlen ? (socklen_t)*fromlen : 0; ssize_t r = ::recvfrom(s, buf, (size_t)len, flags, from, fromlen ? &l : nullptr); if (fromlen) *fromlen = (int)l; return r; }
inline SOCKET accept(SOCKET s, sockaddr* addr, int* len) { socklen_t l = len ? (socklen_t)*len : 0; SOCKET r = ::accept(s, addr, len ? &l : nullptr); if (len) *len = (int)l; return r; }
inline int getsockname(SOCKET s, sockaddr* addr, int* len) { socklen_t l = (socklen_t)*len; int r = ::getsockname(s, addr, &l); *len = (int)l; return r; }
inline int getpeername(SOCKET s, sockaddr* addr, int* len) { socklen_t l = (socklen_t)*len; int r = ::getpeername(s, addr, &l); *len = (int)l; return r; }
inline int getsockopt(SOCKET s, int level, int name, char* value, int* len) { socklen_t l = (socklen_t)*len; int r = ::getsockopt(s, level, name, value, &l); *len = (int)l; return r; }
