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
    # Adreno: DXVK asked for DenormFlushToZero on 32-bit floats (the D3D11 rule). DXVK and vkd3d move
    # integers through float registers as bit patterns; small integers are denormal floats, so on
    # Adreno they became 0 (self-test 8c flat uint varying, 16 cbuffer uint4[i][j]) - the game's
    # uint constants (texture swizzles, vertex fetch descriptors) read as 0 and nothing was drawn.
    # Denormals are preserved instead (when the driver can), and the driver's support is logged.
    ("src/dxbc/dxbc_options.h",
     '    PreserveNan64,\n  };',
     '    PreserveNan64,\n    DenormPreserve32,\n  };'),
    ("src/dxbc/dxbc_options.cpp",
     '        if (devInfo.khrShaderFloatControls.shaderDenormFlushToZeroFloat32)\n          floatControl.set(DxbcFloatControlFlag::DenormFlushToZero32);\n',
     '        // Android: no DenormFlushToZero32 (it zeroed integers held in float registers on Adreno)\n'),
    ("src/dxbc/dxbc_options.cpp",
     '    // Figure out float control flags to match D3D11 rules\n    if (options.floatControls) {',
     '    // Figure out float control flags to match D3D11 rules\n'
     '    Logger::warn(str::format("DXBC float controls (Android): denorm independence ", uint32_t(devInfo.khrShaderFloatControls.denormBehaviorIndependence),\n'
     '      ", preserve32 ", uint32_t(devInfo.khrShaderFloatControls.shaderDenormPreserveFloat32),\n'
     '      ", ftz32 ", uint32_t(devInfo.khrShaderFloatControls.shaderDenormFlushToZeroFloat32)));\n'
     '    if (devInfo.khrShaderFloatControls.shaderDenormPreserveFloat32)\n'
     '      floatControl.set(DxbcFloatControlFlag::DenormPreserve32);\n'
     '    if (options.floatControls) {'),
    ("src/dxbc/dxbc_compiler.cpp",
     '    if (flags.test(DxbcFloatControlFlag::PreserveNan32)) {',
     '    if (flags.test(DxbcFloatControlFlag::DenormPreserve32)) {\n'
     '      m_module.enableCapability(spv::CapabilityDenormPreserve);\n'
     '      m_module.setExecutionMode(m_entryPointId, spv::ExecutionModeDenormPreserve, 1, &width32);\n'
     '    }\n\n'
     '    if (flags.test(DxbcFloatControlFlag::PreserveNan32)) {'),
    # Android: the window's surface is gone while the app is in the background (and replaced when it comes back).
    # Presenting then made DXVK recreate the swap chain and build a Vulkan surface from a null ANativeWindow - a
    # crash inside libvulkan (createSurface <- recreateSwapChain <- Present). Without a window the surface is
    # reported lost, the swap chain stays empty (presents answer DXGI_STATUS_OCCLUDED) and is rebuilt once the
    # window is back.
    ("src/wsi/sdl2/wsi_presenter_sdl2.cpp",
     '#include <SDL2/SDL_vulkan.h>\n',
     '#include <SDL2/SDL_vulkan.h>\n#include <SDL2/SDL_syswm.h>\n'),
    ("src/wsi/sdl2/wsi_presenter_sdl2.cpp",
     '    SDL_Window* window = fromHwnd(hWindow);\n\n    return SDL_Vulkan_CreateSurface(',
     '    SDL_Window* window = fromHwnd(hWindow);\n'
     '#ifdef __ANDROID__\n'
     '    {\n'
     '      SDL_SysWMinfo wm;\n'
     '      SDL_VERSION(&wm.version);\n'
     '      if (!window || !SDL_GetWindowWMInfo(window, &wm) || !wm.info.android.window)\n'
     '        return VK_ERROR_SURFACE_LOST_KHR;\n'
     '    }\n'
     '#endif\n\n'
     '    return SDL_Vulkan_CreateSurface('),
    ("src/vulkan/vulkan_presenter.cpp",
     '    VkResult status;\n    \n    if ((status = m_vki->vkGetPhysicalDeviceSurfaceCapabilitiesKHR(',
     '    VkResult status;\n    \n'
     '    if (!m_surface && (status = createSurface()) != VK_SUCCESS)\n'
     '      return status;   // Android: no window yet (app in the background)\n\n'
     '    if ((status = m_vki->vkGetPhysicalDeviceSurfaceCapabilitiesKHR('),
    ("src/d3d11/d3d11_swapchain.cpp",
     '    if (m_presenter->recreateSwapChain(presenterDesc) != VK_SUCCESS)\n      throw DxvkError("D3D11SwapChain: Failed to recreate swap chain");\n',
     '    if (m_presenter->recreateSwapChain(presenterDesc) != VK_SUCCESS) {\n'
     '      static uint32_t notes = 0;\n'
     '      if (notes++ < 20) Logger::warn("D3D11SwapChain: no swap chain for now (no window: app in the background?) - presents skipped");\n'
     '      return;\n'
     '    }\n'),
    ("src/d3d11/d3d11_swapchain.cpp",
     '      SynchronizePresent();\n\n      if (!m_presenter->hasSwapChain())\n        return DXGI_STATUS_OCCLUDED;\n',
     '      SynchronizePresent();\n\n      if (!m_presenter->hasSwapChain()) {\n        RecreateSwapChain(m_vsync);   // the window may be back\n        if (!m_presenter->hasSwapChain())\n          return DXGI_STATUS_OCCLUDED;\n      }\n'),
    # destroySurface freed the surface but kept its handle: the "no surface yet" check above never fired and the
    # next recreation queried a destroyed surface (a jump to address 0 inside libvulkan after the window came back).
    ("src/vulkan/vulkan_presenter.cpp",
     '    m_vki->vkDestroySurfaceKHR(m_vki->instance(), m_surface, nullptr);\n  }',
     '    m_vki->vkDestroySurfaceKHR(m_vki->instance(), m_surface, nullptr);\n    m_surface = VK_NULL_HANDLE;\n  }'),
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
