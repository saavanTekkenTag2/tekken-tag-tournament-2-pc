#!/usr/bin/env python3
"""ARM64: simde maps simde_mm_slli/srli_epi64 to NEON vshlq_n/vshrq_n, which need a
compile-time shift count. simde_mm_vsl shifts by a runtime count, so use the
variable-count forms (sll/srl with the count in a vector). Same result on x86."""
import sys, pathlib
OLD = "return simde_mm_or_si128(simde_mm_slli_epi64(a, shift), simde_mm_srli_epi64(simde_mm_slli_si128(a, 8), 64 - shift));"
NEW = ("return simde_mm_or_si128(simde_mm_sll_epi64(a, simde_mm_cvtsi32_si128((int)shift)), "
       "simde_mm_srl_epi64(simde_mm_slli_si128(a, 8), simde_mm_cvtsi32_si128((int)(64 - shift))));")
for p in map(pathlib.Path, sys.argv[1:]):
    s = p.read_text()
    if NEW in s: print(f"{p}: already fixed"); continue
    n = s.count(OLD)
    if n == 0: sys.exit(f"{p}: pattern not found")
    p.write_text(s.replace(OLD, NEW)); print(f"{p}: fixed {n} site(s)")
