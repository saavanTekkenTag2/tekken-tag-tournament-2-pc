#pragma once
#include <winsock2.h>
#define InetPtonA inet_pton
#define InetNtopA inet_ntop
extern "C" int InetPtonW(int family, const wchar_t* src, void* dst);
extern "C" const wchar_t* InetNtopW(int family, const void* src, wchar_t* dst, size_t size);
typedef struct addrinfoW { int ai_flags, ai_family, ai_socktype, ai_protocol; size_t ai_addrlen; wchar_t* ai_canonname; struct sockaddr* ai_addr; struct addrinfoW* ai_next; } ADDRINFOW, *PADDRINFOW;
extern "C" int GetAddrInfoW(const wchar_t* node, const wchar_t* service, const ADDRINFOW* hints, PADDRINFOW* result);
extern "C" void FreeAddrInfoW(PADDRINFOW info);
