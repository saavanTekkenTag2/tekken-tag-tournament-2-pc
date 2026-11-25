// System-info, module (Xex), and time kernel imports.
#include "kernel_shared.h"
#include "kernel_trace.h"
#include "guest_time.h"
#include "guest_heap.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
void DiagNameThread(const wchar_t* name);   // kernel_thread.cpp (TEST ONLY)

// --- Xex module queries. The recompiler already resolved the game's own calls, so these serve
//     optional runtime probing; a fake module handle + "not found" for arbitrary procs is safe. ---
PPC_FUNC(__imp__XexCheckExecutablePrivilege)
{
    KTrace("XexCheckExecutablePrivilege"); ctx.r3.u32 = 0; }              // privilege not held
PPC_FUNC(__imp__XexGetModuleHandle)
{
    KTrace("XexGetModuleHandle"); if (ctx.r4.u32) GWrite32(base, ctx.r4.u32, 0x80000000); ctx.r3.u32 = STATUS_SUCCESS; }
PPC_FUNC(__imp__XexGetModuleSection)
{
    KTrace("XexGetModuleSection"); ctx.r3.u32 = STATUS_UNSUCCESSFUL; }
// The game resolves system procs (e.g. xam.xex ordinal 36 = XNetLogonTaskContinue, a Live logon fn)
// and calls the result unconditionally. Offline we don't need those, but we must return a *callable*
// address. Hand out one registered no-op thunk placed in a free slot of the indirect-call table
// (that table only covers the code range, so the thunk address must live there too).
static void GuestProcNoop(PPCContext& ctx, uint8_t* /*base*/) { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__XexGetProcedureAddress)
{
    KTrace("XexGetProcedureAddress");
    static uint32_t s_thunk = 0;
    if (!s_thunk)
    {
        for (uint32_t a = PPC_CODE_BASE + (uint32_t)PPC_CODE_SIZE - 4; a > PPC_CODE_BASE; a -= 4)
        {
            PPCFunc*& slot = PPC_LOOKUP_FUNC(base, a);
            if (slot == nullptr || slot == g_unmappedReporter) { slot = GuestProcNoop; s_thunk = a; break; }
        }
    }
    if (ctx.r5.u32) GWrite32(base, ctx.r5.u32, s_thunk);
    ctx.r3.u32 = STATUS_SUCCESS;
}
PPC_FUNC(__imp__XexLoadImage)
{
    KTrace("XexLoadImage"); ctx.r3.u32 = STATUS_UNSUCCESSFUL; }
PPC_FUNC(__imp__XexLoadImageHeaders)
{
    KTrace("XexLoadImageHeaders"); ctx.r3.u32 = STATUS_UNSUCCESSFUL; }
PPC_FUNC(__imp__XexUnloadImage)
{
    KTrace("XexUnloadImage"); ctx.r3.u32 = STATUS_SUCCESS; }

// --- status / error mapping ---
PPC_FUNC(__imp__RtlNtStatusToDosError)
{
    KTrace("RtlNtStatusToDosError");
    // The file source distinguishes ERROR_HANDLE_EOF (38) from other failures.
    // NTSTATUS low bits are not Win32 error numbers: STATUS_END_OF_FILE's are 17.
    // Use Windows' documented mapping, including ERROR_MR_MID_NOT_FOUND for an
    // unknown status, rather than inventing a successful or unrelated error code.
    using StatusToDosError = ULONG (NTAPI*)(LONG);
    static const StatusToDosError convert = reinterpret_cast<StatusToDosError>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlNtStatusToDosError"));
    const uint32_t status = ctx.r3.u32;
    if (convert) { ctx.r3.u32 = convert(static_cast<LONG>(status)); return; }
    // No ntdll (Android): Windows' mapping for the statuses the game and XAPI can see. MEASURED 2026-11-25 (Android
    // r14/r15 traces): every status came back ERROR_MR_MID_NOT_FOUND, so XAPI's ReadFile reported the XMV player's read
    // past the end of data047.bin as an unknown failure instead of ERROR_HANDLE_EOF; the player tore down and the game
    // froze on the title movie after the opening movie was skipped.
    static const struct { uint32_t status, error; } kMap[] = {
        { 0x00000000u, 0 },    { 0x00000103u, 997 },  { 0x00000102u, 258 },  { 0x00000101u, 0 },    { 0x00000080u, 0 },
        { 0x80000005u, 234 },  { 0x80000006u, 18 },   { 0x8000001Au, 259 },  { 0x80000011u, 170 },
        { 0xC0000001u, 31 },   { 0xC0000002u, 1 },    { 0xC0000004u, 24 },   { 0xC0000005u, 998 },  { 0xC0000008u, 6 },
        { 0xC000000Du, 87 },   { 0xC000000Eu, 2 },    { 0xC000000Fu, 2 },    { 0xC0000010u, 1 },    { 0xC0000011u, 38 },
        { 0xC0000013u, 21 },   { 0xC0000017u, 8 },    { 0xC000001Du, 1 },    { 0xC0000022u, 5 },    { 0xC0000023u, 122 },
        { 0xC0000024u, 6 },    { 0xC0000033u, 123 },  { 0xC0000034u, 2 },    { 0xC0000035u, 183 },  { 0xC000003Au, 3 },
        { 0xC000003Bu, 161 },  { 0xC0000043u, 32 },   { 0xC000004Au, 156 },  { 0xC0000054u, 33 },   { 0xC0000056u, 5 },
        { 0xC000007Fu, 112 },  { 0xC000009Au, 1450 }, { 0xC000009Cu, 23 },   { 0xC00000A3u, 21 },   { 0xC00000B5u, 121 },
        { 0xC00000BAu, 5 },    { 0xC00000BBu, 50 },   { 0xC00000E5u, 1359 }, { 0xC00000EFu, 87 },   { 0xC0000101u, 145 },
        { 0xC0000102u, 1392 }, { 0xC0000103u, 267 },  { 0xC0000120u, 995 },  { 0xC0000185u, 1117 }, { 0xC000000Cu, 31 },
    };
    uint32_t error = ERROR_MR_MID_NOT_FOUND;
    bool found = false;
    for (const auto& m : kMap)
        if (m.status == status) { error = m.error; found = true; break; }
    if (!found && (status & 0xFFFF0000u) == 0xC0070000u) error = status & 0xFFFFu;   // FACILITY_WIN32 statuses
    else if (!found && (status & 0xFFFF0000u) == 0x80070000u) error = status & 0xFFFFu;
    if (!found)
    {
        static std::atomic<int> unknownLogs{ 0 };
        if (unknownLogs.fetch_add(1, std::memory_order_relaxed) < 20)
            std::fprintf(stderr, "[kernel] RtlNtStatusToDosError: status %08X not in the table -> %u\n", status, error);
    }
    ctx.r3.u32 = error;
}

// --- memory statistics: fill the guest MM_STATISTICS struct with plausible values ---
PPC_FUNC(__imp__MmQueryStatistics)
{
    KTrace("MmQueryStatistics");
    uint32_t p = ctx.r3.u32;
    if (p)
    {
        // Zero the struct, then set a couple of size fields (total/available physical pages).
        for (uint32_t i = 0; i < 0x40; i += 4) GWrite32(base, p + i, 0);
        GWrite32(base, p + 0x00, 0x40);          // Length
        GWrite32(base, p + 0x04, 0x00020000);    // TotalPhysicalPages (~512 MiB / 4 KiB)
        GWrite32(base, p + 0x08, 0x00010000);    // AvailablePages
    }
    ctx.r3.u32 = STATUS_SUCCESS;
}

// --- time ---
// KeTimeStampBundle (xboxkrnl data export 0xAD): {u64 InterruptTime (100 ns since boot), u64
// SystemTime (FILETIME), u32 TickCount (ms since boot)}, kept current by the kernel. Game code
// reads it straight from memory through the import slot 0x82000720 (e.g. sub_82BEF480 = NACE's
// clock, which drives the online session search and its timeouts). The loader never patched
// the slot (it still held the ordinal 0xAD), so those clocks read 0 and never advanced
// (MEASURED 2026-10-12: the Player Match search waited forever on a 1 s timer). A host thread
// refreshes one guest cell every millisecond.
void KernelInstallTimeStampBundle(uint8_t* base)
{
    static std::atomic<bool> started{ false };
    if (started.exchange(true)) return;
    void* cell = g_virtualHeap.Alloc(0x18, 16);
    if (!cell) return;
    const uint32_t guest = uint32_t(static_cast<uint8_t*>(cell) - base);
    std::memset(cell, 0, 0x18);
    GWrite32(base, 0x82000720u, guest);
    std::thread([base, guest] {
        DiagNameThread(L"clock-cell");
        const auto boot = std::chrono::steady_clock::now();
        for (;;)
        {
            const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - boot).count());
            FILETIME ft;
            GetSystemTimeAsFileTime(&ft);
            const uint64_t system = (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
            const uint64_t interrupt = ns / 100;
            GWrite32(base, guest + 0, uint32_t(interrupt >> 32));
            GWrite32(base, guest + 4, uint32_t(interrupt));
            GWrite32(base, guest + 8, uint32_t(system >> 32));
            GWrite32(base, guest + 12, uint32_t(system));
            GWrite32(base, guest + 16, uint32_t(ns / 1000000));
            Sleep(1);
        }
    }).detach();
    std::fprintf(stderr, "[kernel] KeTimeStampBundle -> %08X (import slot 0x82000720)\n", guest);
}

PPC_FUNC(__imp__KeQuerySystemTime)
{
    KTrace("KeQuerySystemTime");
    // Xbox system time = 100 ns intervals since 1601 (same epoch as FILETIME).
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    uint64_t t = (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    uint32_t p = ctx.r3.u32;
    if (p) { GWrite32(base, p + 0, uint32_t(t >> 32)); GWrite32(base, p + 4, uint32_t(t)); }  // BE 64-bit
}
PPC_FUNC(__imp__KeQueryPerformanceFrequency)
{
    KTrace("KeQueryPerformanceFrequency");
    // Return in r3; the guest's QueryPerformanceFrequency wrapper stores r3
    // directly as a 64-bit integer. r4 is not the low half of this result.
    ctx.r3.u64 = PPC_TIME_BASE_FREQUENCY;
}
