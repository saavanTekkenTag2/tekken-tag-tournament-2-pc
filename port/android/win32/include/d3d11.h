// dxvk-native's d3d11.h (mingw-w64 era) plus the few newer Windows SDK names the renderer uses.
#pragma once
#include <windows.h>
#include_next <d3d11.h>
#ifndef D3D11_FILTER_REDUCTION_TYPE_STANDARD
typedef enum D3D11_FILTER_REDUCTION_TYPE {
    D3D11_FILTER_REDUCTION_TYPE_STANDARD = 0,
    D3D11_FILTER_REDUCTION_TYPE_COMPARISON = 1,
    D3D11_FILTER_REDUCTION_TYPE_MINIMUM = 2,
    D3D11_FILTER_REDUCTION_TYPE_MAXIMUM = 3
} D3D11_FILTER_REDUCTION_TYPE;
#endif
