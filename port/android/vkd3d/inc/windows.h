/* Minimal Win32 types for vkd3d-shader built natively (Android): what vkd3d's own
 * vkd3d_windows.h provides in the standalone build. */
#pragma once
#include <stdint.h>
#include <stddef.h>
typedef int32_t HRESULT;
typedef uint16_t WCHAR;
typedef int BOOL;
typedef uint32_t UINT;
typedef int32_t INT;
typedef float FLOAT;
typedef uint32_t ULONG;
typedef int32_t LONG;
typedef uint32_t DWORD;
typedef uint16_t WORD;
typedef uint8_t BYTE;
typedef size_t SIZE_T;
typedef void *HANDLE;
typedef const char *LPCSTR;
typedef char *LPSTR;
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
typedef struct _GUID { uint32_t Data1; uint16_t Data2; uint16_t Data3; uint8_t Data4[8]; } GUID, IID, CLSID;
#define REFIID const IID *
#define REFGUID const GUID *
#define S_OK ((HRESULT)0)
#define S_FALSE ((HRESULT)1)
#define E_NOTIMPL ((HRESULT)0x80004001)
#define E_NOINTERFACE ((HRESULT)0x80004002)
#define E_POINTER ((HRESULT)0x80004003)
#define E_ABORT ((HRESULT)0x80004004)
#define E_FAIL ((HRESULT)0x80004005)
#define E_OUTOFMEMORY ((HRESULT)0x8007000E)
#define E_INVALIDARG ((HRESULT)0x80070057)
#define DXGI_ERROR_NOT_FOUND ((HRESULT)0x887A0002)
#define DXGI_ERROR_MORE_DATA ((HRESULT)0x887A0003)
#define DXGI_ERROR_ALREADY_EXISTS ((HRESULT)0x887A0036)
#define DXGI_ERROR_UNSUPPORTED ((HRESULT)0x887A0004)
#define SUCCEEDED(hr) ((HRESULT)(hr) >= 0)
#define FAILED(hr) ((HRESULT)(hr) < 0)
#define WINAPI
#define STDMETHODCALLTYPE
#include <string.h>
#include <stdlib.h>
#ifndef min
#define min(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef max
#define max(a, b) (((a) > (b)) ? (a) : (b))
#endif
#ifndef CONTAINING_RECORD
#define CONTAINING_RECORD(address, type, field) ((type *)((char *)(address) - offsetof(type, field)))
#endif
#ifndef D3DVS_VERSION
#define D3DVS_VERSION(major, minor) (0xfffe0000 | ((major) << 8) | (minor))
#define D3DPS_VERSION(major, minor) (0xffff0000 | ((major) << 8) | (minor))
#endif
