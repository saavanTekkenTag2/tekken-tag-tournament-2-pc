#!/usr/bin/env python3
"""ARM64 fixes for XenonRecomp's ppc_context.h (both copies: the generated re/ppc one and
tools/XenonRecomp/XenonUtils). Idempotent.

1. simde maps simde_mm_slli/srli_epi64 to NEON vshlq_n/vshrq_n, which need a compile-time
   shift count. simde_mm_vsl shifts by a runtime count, so use the variable-count forms
   (sll/srl with the count in a vector). Same result on x86.
2. The ARM64 __rdtsc() is also defined by the Android Win32 layer's intrinsics header;
   guard both definitions with TTT2_RDTSC_DEFINED so either may come first."""
import sys, pathlib

FIXES = [
    ("return simde_mm_or_si128(simde_mm_slli_epi64(a, shift), simde_mm_srli_epi64(simde_mm_slli_si128(a, 8), 64 - shift));",
     "return simde_mm_or_si128(simde_mm_sll_epi64(a, simde_mm_cvtsi32_si128((int)shift)), "
     "simde_mm_srl_epi64(simde_mm_slli_si128(a, 8), simde_mm_cvtsi32_si128((int)(64 - shift))));"),
    ("#if defined(__aarch64__) || defined(_M_ARM64)\ninline uint64_t __rdtsc()\n{",
     "#if defined(__aarch64__) || defined(_M_ARM64)\n#ifndef TTT2_RDTSC_DEFINED\n#define TTT2_RDTSC_DEFINED 1\ninline uint64_t __rdtsc()\n{"),
    ("                 : \"=r\"(ret)::\"memory\");\n    return ret;\n}\n#elif !defined(__x86_64__)",
     "                 : \"=r\"(ret)::\"memory\");\n    return ret;\n}\n#endif\n#elif !defined(__x86_64__)"),
]

for p in map(pathlib.Path, sys.argv[1:]):
    s = p.read_text()
    changed = 0
    for old, new in FIXES:
        if new in s:
            continue
        if old not in s:
            sys.exit(f"{p}: pattern not found: {old[:60]!r}")
        s = s.replace(old, new)
        changed += 1
    p.write_text(s)
    print(f"{p}: {changed} fix(es) applied" if changed else f"{p}: already fixed")
