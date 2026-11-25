// Builds without DXVK (TTT2_D3D_LIBS empty): the D3D11 / DXGI entry points the renderer imports.
// Every call fails cleanly, so libmain.so links and the platform layer can be tested. Stage 4
// DXVK-native (libdxvk_d3d11.so, libdxvk_dxgi.so) provides the real ones.
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstdio>

extern "C" HRESULT WINAPI CreateDXGIFactory1(REFIID, void** factory)
{
    if (factory) *factory = nullptr;
    std::fprintf(stderr, "[d3d] CreateDXGIFactory1: no DXVK in this build (stage 3)\n");
    return E_NOTIMPL;
}
extern "C" HRESULT WINAPI D3D11CreateDevice(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
                                            ID3D11Device** device, D3D_FEATURE_LEVEL* level, ID3D11DeviceContext** context)
{
    if (device) *device = nullptr;
    if (context) *context = nullptr;
    if (level) *level = D3D_FEATURE_LEVEL_11_0;
    std::fprintf(stderr, "[d3d] D3D11CreateDevice: no DXVK in this build (stage 3)\n");
    return E_NOTIMPL;
}
