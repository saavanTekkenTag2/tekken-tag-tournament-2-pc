// MSVC CRT spellings that Windows code uses without including windows.h. Force-included
// into every runtime source on Android (-include msvc_crt.h), and included by windows.h.
#pragma once
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cctype>
#include <cwctype>
#include <ctime>
#include <utility>
#include <strings.h>
#include <unistd.h>
#include <alloca.h>
#include <sys/stat.h>
#include <sys/types.h>

typedef long long __time64_t;
typedef long __time32_t;
inline __time64_t _time64(__time64_t* t) { __time64_t v = (__time64_t)::time(nullptr); if (t) *t = v; return v; }
inline int localtime_s(struct tm* out, const time_t* t) { return localtime_r(t, out) ? 0 : 22; }
inline int gmtime_s(struct tm* out, const time_t* t) { return gmtime_r(t, out) ? 0 : 22; }
inline int _localtime64_s(struct tm* out, const __time64_t* t) { time_t v = (time_t)*t; return localtime_r(&v, out) ? 0 : 22; }
inline int _gmtime64_s(struct tm* out, const __time64_t* t) { time_t v = (time_t)*t; return gmtime_r(&v, out) ? 0 : 22; }
inline __time64_t _mkgmtime64(struct tm* t) { return (__time64_t)timegm(t); }
inline time_t _mkgmtime(struct tm* t) { return timegm(t); }
inline __time64_t _mktime64(struct tm* t) { return (__time64_t)mktime(t); }
inline int ctime_s(char* buf, size_t size, const time_t* t) { char tmp[32]; if (!ctime_r(t, tmp)) return 22; std::snprintf(buf, size, "%s", tmp); return 0; }
inline int asctime_s(char* buf, size_t size, const struct tm* t) { char tmp[32]; if (!asctime_r(t, tmp)) return 22; std::snprintf(buf, size, "%s", tmp); return 0; }

// ---------------------------------------------------------------------------------
// CRT extensions (MSVC spellings)
// ---------------------------------------------------------------------------------
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#define stricmp strcasecmp
#define strnicmp strncasecmp
#define _strdup strdup
#define _wcsdup wcsdup
#define _snprintf snprintf
#define _snwprintf swprintf
#define _vsnprintf vsnprintf
#define _vsnwprintf vswprintf
#define _fileno fileno
#define _isatty isatty
#define _close close
#define _dup dup
#define _dup2 dup2
#define _unlink unlink
#define _access access
#define _getpid getpid
#define _strtoui64 strtoull
#define _strtoi64 strtoll
#define _wcstoui64 wcstoull
#define _wcstoi64 wcstoll
#define _atoi64 atoll
#define _fseeki64 fseeko
#define _ftelli64 ftello
#define _alloca alloca
#define _O_RDONLY 0x0000
#define _O_WRONLY 0x0001
#define _O_RDWR 0x0002
#define _O_APPEND 0x0008
#define _O_CREAT 0x0100
#define _O_TRUNC 0x0200
#define _O_TEXT 0x4000
#define _O_BINARY 0x8000
#define _O_U8TEXT 0x40000
#define _O_U16TEXT 0x20000
#define _O_WTEXT 0x10000
inline int _setmode(int, int) { return 0; }
// HANDLE <-> fd (win32_file.cpp): a HANDLE for an fd is a non-owning file object.
extern "C" intptr_t _get_osfhandle(int fd);
extern "C" int _open_osfhandle(intptr_t handle, int flags);
inline char* _strlwr(char* s) { for (char* p = s; *p; ++p) *p = (char)std::tolower((unsigned char)*p); return s; }
inline char* _strupr(char* s) { for (char* p = s; *p; ++p) *p = (char)std::toupper((unsigned char)*p); return s; }
inline wchar_t* _wcslwr(wchar_t* s) { for (wchar_t* p = s; *p; ++p) *p = (wchar_t)std::towlower(*p); return s; }
inline wchar_t* _wcsupr(wchar_t* s) { for (wchar_t* p = s; *p; ++p) *p = (wchar_t)std::towupper(*p); return s; }
inline int _strlwr_s(char* s, size_t) { _strlwr(s); return 0; }
inline int _strupr_s(char* s, size_t) { _strupr(s); return 0; }
inline int _wcslwr_s(wchar_t* s, size_t) { _wcslwr(s); return 0; }
inline int strcpy_s(char* d, size_t n, const char* s) { if (!d || !n) return 22; std::snprintf(d, n, "%s", s ? s : ""); return 0; }
template <size_t N> inline int strcpy_s(char (&d)[N], const char* s) { return strcpy_s(d, N, s); }
inline int strncpy_s(char* d, size_t n, const char* s, size_t c) { if (!d || !n) return 22; size_t l = s ? strnlen(s, c) : 0; if (l >= n) l = n - 1; std::memcpy(d, s, l); d[l] = 0; return 0; }
template <size_t N> inline int strncpy_s(char (&d)[N], const char* s, size_t c) { return strncpy_s(d, N, s, c); }
inline int strcat_s(char* d, size_t n, const char* s) { size_t l = strnlen(d, n); if (l >= n) return 22; std::snprintf(d + l, n - l, "%s", s); return 0; }
template <size_t N> inline int strcat_s(char (&d)[N], const char* s) { return strcat_s(d, N, s); }
inline int wcscpy_s(wchar_t* d, size_t n, const wchar_t* s) { if (!d || !n) return 22; size_t l = s ? wcsnlen(s, n - 1) : 0; std::wmemcpy(d, s, l); d[l] = 0; return 0; }
template <size_t N> inline int wcscpy_s(wchar_t (&d)[N], const wchar_t* s) { return wcscpy_s(d, N, s); }
inline int wcsncpy_s(wchar_t* d, size_t n, const wchar_t* s, size_t c) { if (!d || !n) return 22; size_t l = s ? wcsnlen(s, c) : 0; if (l >= n) l = n - 1; std::wmemcpy(d, s, l); d[l] = 0; return 0; }
template <size_t N> inline int wcsncpy_s(wchar_t (&d)[N], const wchar_t* s, size_t c) { return wcsncpy_s(d, N, s, c); }
inline int wcscat_s(wchar_t* d, size_t n, const wchar_t* s) { size_t l = wcsnlen(d, n); if (l >= n) return 22; return wcscpy_s(d + l, n - l, s); }
template <size_t N> inline int wcscat_s(wchar_t (&d)[N], const wchar_t* s) { return wcscat_s(d, N, s); }
inline int memcpy_s(void* d, size_t n, const void* s, size_t c) { if (c > n) return 34; std::memcpy(d, s, c); return 0; }
inline int memmove_s(void* d, size_t n, const void* s, size_t c) { if (c > n) return 34; std::memmove(d, s, c); return 0; }
inline int vsprintf_s(char* d, size_t n, const char* f, va_list a) { return std::vsnprintf(d, n, f, a); }
inline int sprintf_s(char* d, size_t n, const char* f, ...) { va_list a; va_start(a, f); int r = std::vsnprintf(d, n, f, a); va_end(a); return r; }
template <size_t N> inline int sprintf_s(char (&d)[N], const char* f, ...) { va_list a; va_start(a, f); int r = std::vsnprintf(d, N, f, a); va_end(a); return r; }
template <size_t N> inline int vsprintf_s(char (&d)[N], const char* f, va_list a) { return std::vsnprintf(d, N, f, a); }
inline int _snprintf_s(char* d, size_t n, size_t, const char* f, ...) { va_list a; va_start(a, f); int r = std::vsnprintf(d, n, f, a); va_end(a); return r; }
template <size_t N> inline int _snprintf_s(char (&d)[N], size_t, const char* f, ...) { va_list a; va_start(a, f); int r = std::vsnprintf(d, N, f, a); va_end(a); return r; }
inline int _vsnprintf_s(char* d, size_t n, size_t, const char* f, va_list a) { return std::vsnprintf(d, n, f, a); }
inline int swprintf_s(wchar_t* d, size_t n, const wchar_t* f, ...) { va_list a; va_start(a, f); int r = std::vswprintf(d, n, f, a); va_end(a); return r; }
template <size_t N> inline int swprintf_s(wchar_t (&d)[N], const wchar_t* f, ...) { va_list a; va_start(a, f); int r = std::vswprintf(d, N, f, a); va_end(a); return r; }
inline int _snwprintf_s(wchar_t* d, size_t n, size_t, const wchar_t* f, ...) { va_list a; va_start(a, f); int r = std::vswprintf(d, n, f, a); va_end(a); return r; }
template <size_t N> inline int _snwprintf_s(wchar_t (&d)[N], size_t, const wchar_t* f, ...) { va_list a; va_start(a, f); int r = std::vswprintf(d, N, f, a); va_end(a); return r; }
inline int vswprintf_s(wchar_t* d, size_t n, const wchar_t* f, va_list a) { return std::vswprintf(d, n, f, a); }
inline int _itoa_s(int v, char* d, size_t n, int radix) { if (radix == 16) std::snprintf(d, n, "%x", v); else std::snprintf(d, n, "%d", v); return 0; }
inline char* _itoa(int v, char* d, int radix) { if (radix == 16) std::sprintf(d, "%x", v); else std::sprintf(d, "%d", v); return d; }
inline char* _ui64toa(unsigned long long v, char* d, int radix) { if (radix == 16) std::sprintf(d, "%llx", v); else std::sprintf(d, "%llu", v); return d; }
inline int fopen_s(FILE** f, const char* name, const char* mode) { *f = std::fopen(name, mode); return *f ? 0 : 2; }
inline char* strtok_s(char* s, const char* d, char** ctx) { return strtok_r(s, d, ctx); }
inline wchar_t* wcstok_s(wchar_t* s, const wchar_t* d, wchar_t** ctx) { return std::wcstok(s, d, ctx); }
inline size_t strnlen_s(const char* s, size_t n) { return s ? strnlen(s, n) : 0; }
inline size_t wcsnlen_s(const wchar_t* s, size_t n) { return s ? wcsnlen(s, n) : 0; }
inline int _putenv_s(const char* name, const char* value) { return (value && *value) ? setenv(name, value, 1) : unsetenv(name); }
inline int _putenv(const char* kv) { return putenv(const_cast<char*>(kv)); }
inline int _dupenv_s(char** out, size_t* len, const char* name) { const char* v = std::getenv(name); *out = v ? strdup(v) : nullptr; if (len) *len = v ? std::strlen(v) + 1 : 0; return 0; }
extern "C" wchar_t* _wgetenv(const wchar_t* name);
extern "C" int _wputenv_s(const wchar_t* name, const wchar_t* value);
extern "C" FILE* _wfopen(const wchar_t* name, const wchar_t* mode);
extern "C" int _wfopen_s(FILE** f, const wchar_t* name, const wchar_t* mode);
extern "C" FILE* _wfreopen(const wchar_t* name, const wchar_t* mode, FILE* stream);
extern "C" FILE* _fsopen(const char* name, const char* mode, int share);
extern "C" FILE* _wfsopen(const wchar_t* name, const wchar_t* mode, int share);
extern "C" int _wremove(const wchar_t* name);
extern "C" int _wrename(const wchar_t* from, const wchar_t* to);
extern "C" int _wmkdir(const wchar_t* name);
extern "C" int _mkdir(const char* name);
inline FILE* freopen_s_impl(const char* n, const char* m, FILE* s) { return std::freopen(n, m, s); }
inline int freopen_s(FILE** out, const char* n, const char* m, FILE* s) { *out = std::freopen(n, m, s); return *out ? 0 : 2; }
inline int _wfreopen_s(FILE** out, const wchar_t* n, const wchar_t* m, FILE* s) { *out = _wfreopen(n, m, s); return *out ? 0 : 2; }
#define _SH_DENYNO 0x40
#define _SH_DENYWR 0x20
#define _SH_DENYRW 0x10
typedef void (*_purecall_handler)(void);
inline _purecall_handler _set_purecall_handler(_purecall_handler) { return nullptr; }
typedef void (*_invalid_parameter_handler)(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t);
inline _invalid_parameter_handler _set_invalid_parameter_handler(_invalid_parameter_handler) { return nullptr; }
inline unsigned int _set_abort_behavior(unsigned int, unsigned int) { return 0; }
inline int _set_error_mode(int) { return 0; }
inline int _CrtSetReportMode(int, int) { return 0; }
#define _WRITE_ABORT_MSG 0x1
#define _CALL_REPORTFAULT 0x2
#define _OUT_TO_STDERR 1
#define _CRT_ASSERT 2
#define _CRTDBG_MODE_DEBUG 0x2
inline unsigned int _controlfp(unsigned int, unsigned int) { return 0; }
inline int _controlfp_s(unsigned int* cur, unsigned int, unsigned int) { if (cur) *cur = 0; return 0; }
inline unsigned int _clearfp() { return 0; }
inline unsigned int _statusfp() { return 0; }
#define _MCW_DN 0x03000000
#define _DN_FLUSH 0x01000000
#define _MCW_EM 0x0008001f
#define _MCW_RC 0x00000300
#define _RC_NEAR 0x00000000
#define _EM_INVALID 0x00000010
#define _EM_ZERODIVIDE 0x00000008
#define _EM_OVERFLOW 0x00000004
inline unsigned short _byteswap_ushort(unsigned short v) { return __builtin_bswap16(v); }
inline unsigned long _byteswap_ulong(unsigned long v) { return (unsigned long)__builtin_bswap32((uint32_t)v); }
inline unsigned long long _byteswap_uint64(unsigned long long v) { return __builtin_bswap64(v); }
inline unsigned int _rotl(unsigned int v, int s) { s &= 31; return (v << s) | (v >> ((32 - s) & 31)); }
inline unsigned int _rotr(unsigned int v, int s) { s &= 31; return (v >> s) | (v << ((32 - s) & 31)); }
inline unsigned long long _rotl64(unsigned long long v, int s) { s &= 63; return (v << s) | (v >> ((64 - s) & 63)); }
inline unsigned long long _rotr64(unsigned long long v, int s) { s &= 63; return (v >> s) | (v << ((64 - s) & 63)); }
inline unsigned char _BitScanForward(unsigned long* i, unsigned long m) { if (!m) return 0; *i = (unsigned long)__builtin_ctzl(m); return 1; }
inline unsigned char _BitScanReverse(unsigned long* i, unsigned long m) { if (!m) return 0; *i = 63ul - (unsigned long)__builtin_clzl(m); return 1; }
inline unsigned char _BitScanForward64(unsigned long* i, unsigned long long m) { if (!m) return 0; *i = (unsigned long)__builtin_ctzll(m); return 1; }
inline unsigned char _BitScanReverse64(unsigned long* i, unsigned long long m) { if (!m) return 0; *i = 63ul - (unsigned long)__builtin_clzll(m); return 1; }
inline unsigned int __popcnt(unsigned int v) { return (unsigned int)__builtin_popcount(v); }
inline unsigned long long __popcnt64(unsigned long long v) { return (unsigned long long)__builtin_popcountll(v); }
inline unsigned short __popcnt16(unsigned short v) { return (unsigned short)__builtin_popcount(v); }
inline unsigned int __lzcnt(unsigned int v) { return v ? (unsigned int)__builtin_clz(v) : 32u; }
inline unsigned long long __lzcnt64(unsigned long long v) { return v ? (unsigned long long)__builtin_clzll(v) : 64ull; }
inline unsigned long long __umulh(unsigned long long a, unsigned long long b) { return (unsigned long long)(((unsigned __int128)a * b) >> 64); }
inline long long __mulh(long long a, long long b) { return (long long)(((__int128)a * b) >> 64); }
inline unsigned long long _umul128(unsigned long long a, unsigned long long b, unsigned long long* hi) { unsigned __int128 r = (unsigned __int128)a * b; *hi = (unsigned long long)(r >> 64); return (unsigned long long)r; }
inline long long _mul128(long long a, long long b, long long* hi) { __int128 r = (__int128)a * b; *hi = (long long)(r >> 64); return (long long)r; }
inline unsigned long long __shiftleft128(unsigned long long lo, unsigned long long hi, unsigned char s) { s &= 63; return s ? (hi << s) | (lo >> (64 - s)) : hi; }
inline unsigned long long __shiftright128(unsigned long long lo, unsigned long long hi, unsigned char s) { s &= 63; return s ? (lo >> s) | (hi << (64 - s)) : lo; }
inline void __debugbreak() { __builtin_trap(); }
[[noreturn]] inline void __fastfail(unsigned int) { __builtin_trap(); }
#define __assume(x) ((void)0)
#define __noop(...) ((void)0)
inline void* _ReturnAddress() { return __builtin_return_address(0); }
inline void* _AddressOfReturnAddress() { return __builtin_frame_address(0); }
// Spin hint and x86 counters on ARM64
#if defined(__aarch64__)
inline void YieldProcessor() { __asm__ __volatile__("yield" ::: "memory"); }
#else
inline void YieldProcessor() { __builtin_ia32_pause(); }
#endif
#define FAST_FAIL_FATAL_APP_EXIT 7


// Windows' std::filesystem::path::c_str() is wchar_t*, Linux's is char*. The runtime mixes
// path::c_str() with wide literals, so the wide CRT helpers accept both.
inline int _wcsicmp(const wchar_t* a, const wchar_t* b) { return wcscasecmp(a, b); }
inline int _wcsnicmp(const wchar_t* a, const wchar_t* b, size_t n) { return wcsncasecmp(a, b, n); }
inline int _wcsicmp(const char* a, const char* b) { return strcasecmp(a, b); }
inline int _wcsicmp(const char* a, const wchar_t* b) { for (;; ++a, ++b) { int ca = std::towlower((wint_t)(unsigned char)*a), cb = std::towlower((wint_t)*b); if (ca != cb) return ca - cb; if (!ca) return 0; } }
inline int _wcsicmp(const wchar_t* a, const char* b) { return -_wcsicmp(b, a); }
inline int _wcsnicmp(const char* a, const char* b, size_t n) { return strncasecmp(a, b, n); }
extern "C" FILE* _wfopen(const wchar_t* name, const wchar_t* mode);
inline FILE* _wfopen(const char* name, const wchar_t* mode) { char m[16] = {}; for (int i = 0; i < 15 && mode[i]; ++i) m[i] = (char)mode[i]; return std::fopen(name, m); }
inline FILE* _wfopen(const char* name, const char* mode) { return std::fopen(name, mode); }
