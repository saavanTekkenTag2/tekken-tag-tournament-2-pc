// Force-included into every recompiled PPC file on ARM64.
// The patched XenonRecomp emits a raw _mm_pause() for PPC spin hints
// (db16cyc / or 31,31,31); give it an ARM64 equivalent.
#pragma once
#if defined(__aarch64__) && !defined(TTT2_MM_PAUSE_SHIM)
#define TTT2_MM_PAUSE_SHIM 1
static inline void _mm_pause(void) { __asm__ __volatile__("yield" ::: "memory"); }
#endif
