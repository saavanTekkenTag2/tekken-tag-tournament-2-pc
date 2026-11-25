#pragma once
#include <windows.h>
typedef struct tagINITCOMMONCONTROLSEX { DWORD dwSize, dwICC; } INITCOMMONCONTROLSEX;
#define ICC_STANDARD_CLASSES 0x00004000
inline BOOL InitCommonControlsEx(const INITCOMMONCONTROLSEX*) { return TRUE; }
