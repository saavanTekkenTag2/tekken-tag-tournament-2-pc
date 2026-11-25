#!/usr/bin/env python3
"""Android/ARM64 changes to dxvk-native 1.9.2b (Joshua-Ashton/dxvk-native, tag native-1.9.2b).
Idempotent: run on the source tree before building.  Usage: patch_dxvk_android.py <dxvk-native dir>"""
import pathlib, sys

root = pathlib.Path(sys.argv[1])
PATCHES = [
    # x86 inline assembly in tzcnt: the portable builtin elsewhere
    ("src/util/util_bit.h",
     '    #elif defined(__GNUC__) || defined(__clang__)\n    uint32_t res;\n    uint32_t tmp;\n    asm (',
     '    #elif (defined(__GNUC__) || defined(__clang__)) && !defined(__x86_64__) && !defined(__i386__)\n    return n != 0 ? uint32_t(__builtin_ctz(n)) : 32u;\n    #elif defined(__GNUC__) || defined(__clang__)\n    uint32_t res;\n    uint32_t tmp;\n    asm ('),
]

def apply(rel, old, new):
    p = root / rel
    s = p.read_text()
    if new in s:
        return f"{rel}: already patched"
    if old not in s:
        sys.exit(f"{rel}: pattern not found: {old[:70]!r}")
    p.write_text(s.replace(old, new, 1))
    return f"{rel}: patched"

for rel, old, new in PATCHES:
    print(apply(rel, old, new))
