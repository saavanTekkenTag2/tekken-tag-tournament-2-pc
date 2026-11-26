#pragma once
// TTT2 native renderer (r24): a Direct3D 11 device implemented directly on Vulkan, written for this port's
// renderer (gpu_hw_d3d11.h) - only the parts of D3D11 / DXGI the game's renderer uses, without DXVK in between.
// Chosen with TTT2_RENDERER=native (launcher: OPTIONS > PERFORMANCE > RENDERER); the game falls back to DXVK
// when this returns a failure (no Vulkan 1.1 device, a missing feature).
#include <windows.h>
#include <d3d11_1.h>

// device / context: the new device's interfaces (AddRef'd). Returns S_OK, or E_FAIL with the reason logged.
HRESULT Ttt2VkCreateDevice(ID3D11Device** device, ID3D11DeviceContext** context);

// True when the native renderer is requested (TTT2_RENDERER=native).
bool Ttt2VkRequested();
