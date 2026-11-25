#pragma once
#ifndef SIMDE_ENABLE_NATIVE_ALIASES
#define SIMDE_ENABLE_NATIVE_ALIASES
#endif
#include <x86/sse.h>
#include <x86/sse2.h>
#include <x86/sse3.h>
#include <x86/ssse3.h>
#include <x86/sse4.1.h>
#include <x86/sse4.2.h>
#include <x86/avx.h>
#include <cstdint>
#ifndef _MM_DENORMALS_ZERO_MASK
#define _MM_DENORMALS_ZERO_MASK 0x0040
#define _MM_DENORMALS_ZERO_ON 0x0040
#define _MM_DENORMALS_ZERO_OFF 0x0000
#endif
#ifndef _MM_SET_DENORMALS_ZERO_MODE
#define _MM_SET_DENORMALS_ZERO_MODE(m) ((void)0)
#define _MM_GET_DENORMALS_ZERO_MODE() 0
#endif
// Counters and CPU identification have no ARM64 equivalent the runtime needs:
// __rdtsc reads the generic timer (cntvct_el0, ~19.2-24 MHz), __cpuid reports nothing.
#if defined(__aarch64__)
#ifndef TTT2_RDTSC_DEFINED
#define TTT2_RDTSC_DEFINED 1
inline uint64_t __rdtsc() { uint64_t v; __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(v)); return v; }
#endif
inline unsigned long long __rdtscp(unsigned int* aux) { if (aux) *aux = 0; return __rdtsc(); }
#ifndef _mm_pause
inline void _mm_pause() { __asm__ __volatile__("yield" ::: "memory"); }
#endif
inline void __cpuid(int info[4], int) { info[0] = info[1] = info[2] = info[3] = 0; }
inline void __cpuidex(int info[4], int, int) { info[0] = info[1] = info[2] = info[3] = 0; }
inline unsigned long long _xgetbv(unsigned int) { return 0; }
#ifndef _MM_HINT_T0
#define _MM_HINT_T0 3
#define _MM_HINT_T1 2
#define _MM_HINT_T2 1
#define _MM_HINT_NTA 0
#endif
#endif
