// DXVK on ARM64: its SSE helpers (util_bit.h, util_vector.h, sync_spinlock.h) through SIMDe (NEON).
#pragma once
#if defined(__aarch64__)
#ifndef SIMDE_ENABLE_NATIVE_ALIASES
#define SIMDE_ENABLE_NATIVE_ALIASES
#endif
#include <x86/sse2.h>
#ifndef _mm_pause
static inline void _mm_pause(void) { __asm__ __volatile__("yield" ::: "memory"); }
#endif
#else
#include_next <x86intrin.h>
#endif
