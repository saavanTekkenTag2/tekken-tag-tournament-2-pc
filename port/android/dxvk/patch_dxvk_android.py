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
    # Android returns VK_SUBOPTIMAL_KHR from every present when the swap chain is not in the
    # display's own orientation (a landscape game on a portrait panel: the compositor rotates).
    # The image is still shown; recreating the swap chain on it rebuilt it every frame (log full
    # of "Actual swap chain properties", black screen). Only real errors recreate it now.
    ("src/d3d11/d3d11_swapchain.cpp",
     '    VkResult status = m_device->waitForSubmission(&m_presentStatus);\n    \n    if (status != VK_SUCCESS)\n      RecreateSwapChain(m_vsync);',
     '    VkResult status = m_device->waitForSubmission(&m_presentStatus);\n    \n    if (status != VK_SUCCESS && status != VK_SUBOPTIMAL_KHR)\n      RecreateSwapChain(m_vsync);'),
    # Adreno: a rasterizer state with DepthClipEnable = FALSE (depth clamp on, depth clip off) drew
    # nothing at all on the device - the game's own states and its present use it, so the whole frame
    # stayed black while simple tests passed. Depth clip stays on unless DXVK_ANDROID_FORCE_DEPTH_CLIP=0.
    ("src/d3d11/d3d11_rasterizer.cpp",
     '    m_state.depthClipEnable   = desc.DepthClipEnable;',
     '    m_state.depthClipEnable   = desc.DepthClipEnable || [] { const char* e = std::getenv("DXVK_ANDROID_FORCE_DEPTH_CLIP"); return !(e && e[0] == \'0\'); }();'),
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
