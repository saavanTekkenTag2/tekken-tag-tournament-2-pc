// Android: link-level bridges for declarations that differ from their definitions only because
// Windows' DWORD is "unsigned long" while here it is a 32-bit uint32_t (the PC build sees one
// type, the Android build sees two different functions).
#include <cstdint>
#include <windows.h>

bool WaitGpuProgress(uint32_t seen, DWORD ms);                       // kernel_video.cpp
bool WaitGpuProgress(uint32_t seen, unsigned long ms) { return WaitGpuProgress(seen, DWORD(ms)); }
