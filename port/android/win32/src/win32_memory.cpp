// Android Win32 layer: virtual memory (VirtualAlloc/Protect/Query), write watching
// (MEM_WRITE_WATCH, GetWriteWatch, ResetWriteWatch) and vectored exception handlers over
// POSIX signals.
//
// Write watch: Windows tracks writes in hardware. Here a watched page is write-protected
// when its range is reset; the first write faults, the handler marks the page dirty and
// restores the page's protection, and the write is retried. Pages never reset report as
// dirty (conservative: callers only re-upload more data than needed).
//
// Faults the write watch does not own go to the vectored handlers registered with
// AddVectoredExceptionHandler (the runtime's copy-on-write, physical-mirror and guard
// handlers), in registration order. On Android, ART's libsigchain sees the signal first
// and passes on what is not its own.
#include "win32_internal.h"

#include <algorithm>
#include <cerrno>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <vector>

#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <ucontext.h>
#include <unistd.h>
#include <unwind.h>
#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace {

constexpr size_t kPage = 0x1000;          // Win32 page size the runtime assumes
constexpr size_t kGranularity = 0x10000;  // Win32 allocation granularity

enum : uint8_t { P_COMMITTED = 1, P_DIRTY = 2, P_ARMED = 4, P_GUARD = 8 };

struct Region
{
    uintptr_t base = 0;
    size_t size = 0;
    uintptr_t mapBase = 0;        // the actual mmap (may be larger for alignment)
    size_t mapSize = 0;
    DWORD allocProtect = 0;
    bool watch = false;
    std::vector<uint8_t> flags;   // per 4 KB page
    std::vector<uint8_t> prot;    // per 4 KB page: Win32 PAGE_* (low byte, without PAGE_GUARD)
};

std::shared_mutex g_regionsMutex;
std::map<uintptr_t, Region*>& Regions() { static auto* m = new std::map<uintptr_t, Region*>(); return *m; }

int HostProt(DWORD p)
{
    switch (p & 0xFF)
    {
    case PAGE_NOACCESS: return PROT_NONE;
    case PAGE_READONLY: return PROT_READ;
    case PAGE_READWRITE: case PAGE_WRITECOPY: return PROT_READ | PROT_WRITE;
    case PAGE_EXECUTE: return PROT_EXEC;
    case PAGE_EXECUTE_READ: return PROT_READ | PROT_EXEC;
    case PAGE_EXECUTE_READWRITE: case PAGE_EXECUTE_WRITECOPY: return PROT_READ | PROT_WRITE | PROT_EXEC;
    default: return PROT_READ | PROT_WRITE;
    }
}
bool Writable(DWORD p) { const DWORD b = p & 0xFF; return b == PAGE_READWRITE || b == PAGE_WRITECOPY || b == PAGE_EXECUTE_READWRITE || b == PAGE_EXECUTE_WRITECOPY; }

// Region containing addr (caller holds the lock)
Region* Find(uintptr_t addr)
{
    auto& m = Regions();
    auto it = m.upper_bound(addr);
    if (it == m.begin()) return nullptr;
    --it;
    Region* r = it->second;
    return addr < r->base + r->size ? r : nullptr;
}

int EffectiveProt(const Region* r, size_t page)
{
    const uint8_t f = r->flags[page];
    if (!(f & P_COMMITTED) || (f & P_GUARD)) return PROT_NONE;
    int p = HostProt(r->prot[page]);
    if (r->watch && (f & P_ARMED)) p &= ~PROT_WRITE;
    return p;
}

// Apply the effective protection to pages [first, last) in runs of equal protection.
void Apply(const Region* r, size_t first, size_t last)
{
    size_t i = first;
    while (i < last)
    {
        const int p = EffectiveProt(r, i);
        size_t j = i + 1;
        while (j < last && EffectiveProt(r, j) == p) ++j;
        mprotect(reinterpret_cast<void*>(r->base + i * kPage), (j - i) * kPage, p);
        i = j;
    }
}

} // namespace

using namespace w32;

extern "C" LPVOID VirtualAlloc(LPVOID address, SIZE_T size, DWORD type, DWORD protect)
{
    if (!size) { SetLastError(ERROR_INVALID_PARAMETER); return nullptr; }
    const uintptr_t want = reinterpret_cast<uintptr_t>(address);
    std::unique_lock lk(g_regionsMutex);

    // Commit inside an existing reservation
    if (want && !(type & MEM_RESERVE))
    {
        Region* r = Find(want);
        if (!r) { SetLastError(ERROR_INVALID_ADDRESS); return nullptr; }
        const uintptr_t start = want & ~(kPage - 1);
        const uintptr_t end = std::min<uintptr_t>((want + size + kPage - 1) & ~(kPage - 1), r->base + r->size);
        const size_t first = (start - r->base) / kPage, last = (end - r->base) / kPage;
        for (size_t i = first; i < last; ++i)
        {
            if (!(r->flags[i] & P_COMMITTED) || (type & MEM_RESET)) { r->flags[i] = uint8_t(P_COMMITTED | (r->watch ? P_DIRTY : 0)); }
            r->prot[i] = uint8_t(protect & 0xFF);
            if (protect & PAGE_GUARD) r->flags[i] |= P_GUARD; else r->flags[i] &= uint8_t(~P_GUARD);
        }
        Apply(r, first, last);
        return reinterpret_cast<LPVOID>(start);
    }

    // New reservation (optionally committed), 64 KB aligned
    const size_t rsize = (size + kPage - 1) & ~(kPage - 1);
    const size_t base = want & ~(kGranularity - 1);
    const bool commit = (type & MEM_COMMIT) != 0;
    const int prot = commit ? HostProt(protect) : PROT_NONE;
    void* m = MAP_FAILED;
    size_t mapSize = rsize;
    uintptr_t aligned = 0;
    if (base)
    {
        m = mmap(reinterpret_cast<void*>(base), rsize, prot, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (m != MAP_FAILED && reinterpret_cast<uintptr_t>(m) != base) { munmap(m, rsize); m = MAP_FAILED; SetLastError(ERROR_INVALID_ADDRESS); return nullptr; }
        aligned = base;
    }
    else
    {
        mapSize = rsize + kGranularity;
        m = mmap(nullptr, mapSize, prot, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (m != MAP_FAILED)
        {
            const uintptr_t raw = reinterpret_cast<uintptr_t>(m);
            aligned = (raw + kGranularity - 1) & ~(kGranularity - 1);
            if (aligned > raw) munmap(m, aligned - raw);
            const uintptr_t tail = aligned + rsize, rawEnd = raw + mapSize;
            if (rawEnd > tail) munmap(reinterpret_cast<void*>(tail), rawEnd - tail);
            mapSize = rsize;
        }
    }
    if (m == MAP_FAILED) { SetErrno(errno); return nullptr; }
    auto* r = new Region();
    r->base = aligned; r->size = rsize; r->mapBase = aligned; r->mapSize = mapSize;
    r->allocProtect = protect; r->watch = (type & MEM_WRITE_WATCH) != 0;
    r->flags.assign(rsize / kPage, uint8_t(commit ? (P_COMMITTED | (r->watch ? P_DIRTY : 0)) : 0));
    r->prot.assign(rsize / kPage, uint8_t(protect & 0xFF));
    if (commit && (protect & PAGE_GUARD))
    {
        for (auto& f : r->flags) f |= P_GUARD;
        mprotect(reinterpret_cast<void*>(aligned), rsize, PROT_NONE);
    }
    Regions()[aligned] = r;
    return reinterpret_cast<LPVOID>(aligned);
}

extern "C" BOOL VirtualFree(LPVOID address, SIZE_T size, DWORD type)
{
    const uintptr_t a = reinterpret_cast<uintptr_t>(address);
    std::unique_lock lk(g_regionsMutex);
    Region* r = Find(a);
    if (!r) { SetLastError(ERROR_INVALID_ADDRESS); return FALSE; }
    if (type & MEM_RELEASE)
    {
        if (a != r->base) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
        munmap(reinterpret_cast<void*>(r->mapBase), r->mapSize);
        Regions().erase(r->base);
        delete r;
        return TRUE;
    }
    if (type & MEM_DECOMMIT)
    {
        const uintptr_t start = a & ~(kPage - 1);
        const uintptr_t end = size ? std::min<uintptr_t>((a + size + kPage - 1) & ~(kPage - 1), r->base + r->size) : r->base + r->size;
        madvise(reinterpret_cast<void*>(start), end - start, MADV_DONTNEED);
        const size_t first = (start - r->base) / kPage, last = (end - r->base) / kPage;
        for (size_t i = first; i < last; ++i) r->flags[i] = 0;
        Apply(r, first, last);
        return TRUE;
    }
    SetLastError(ERROR_INVALID_PARAMETER);
    return FALSE;
}

extern "C" BOOL VirtualProtect(LPVOID address, SIZE_T size, DWORD protect, PDWORD oldProtect)
{
    const uintptr_t a = reinterpret_cast<uintptr_t>(address);
    const uintptr_t start = a & ~(kPage - 1);
    const uintptr_t end = (a + size + kPage - 1) & ~(kPage - 1);
    std::unique_lock lk(g_regionsMutex);
    Region* r = Find(a);
    if (!r)
    {
        // Memory this layer did not allocate (e.g. a static buffer): plain mprotect.
        if (oldProtect) *oldProtect = PAGE_READWRITE;
        if (mprotect(reinterpret_cast<void*>(start), end - start, HostProt(protect)) != 0) { SetErrno(errno); return FALSE; }
        return TRUE;
    }
    const size_t first = (start - r->base) / kPage;
    const size_t last = std::min<size_t>((end - r->base) / kPage, r->flags.size());
    if (oldProtect) *oldProtect = r->prot[first] | ((r->flags[first] & P_GUARD) ? PAGE_GUARD : 0);
    for (size_t i = first; i < last; ++i)
    {
        if (!(r->flags[i] & P_COMMITTED)) { SetLastError(ERROR_INVALID_ADDRESS); return FALSE; }
        r->prot[i] = uint8_t(protect & 0xFF);
        if (protect & PAGE_GUARD) r->flags[i] |= P_GUARD; else r->flags[i] &= uint8_t(~P_GUARD);
    }
    Apply(r, first, last);
    return TRUE;
}

extern "C" SIZE_T VirtualQuery(LPCVOID address, PMEMORY_BASIC_INFORMATION info, SIZE_T length)
{
    if (!info || length < sizeof(*info)) return 0;
    const uintptr_t a = reinterpret_cast<uintptr_t>(address) & ~(kPage - 1);
    std::shared_lock lk(g_regionsMutex);
    std::memset(info, 0, sizeof(*info));
    Region* r = Find(a);
    if (!r)
    {
        // Not ours: report it as committed read/write if it is mapped (mincore succeeds), else free.
        unsigned char vec;
        const bool mapped = mincore(reinterpret_cast<void*>(a), kPage, &vec) == 0;
        info->BaseAddress = reinterpret_cast<PVOID>(a);
        info->AllocationBase = mapped ? reinterpret_cast<PVOID>(a) : nullptr;
        info->RegionSize = kPage;
        info->State = mapped ? MEM_COMMIT : MEM_FREE;
        info->Protect = mapped ? PAGE_READWRITE : PAGE_NOACCESS;
        info->AllocationProtect = info->Protect;
        info->Type = mapped ? MEM_PRIVATE : 0;
        return sizeof(*info);
    }
    const size_t page = (a - r->base) / kPage;
    const uint8_t f0 = r->flags[page] & (P_COMMITTED | P_GUARD);
    const uint8_t p0 = r->prot[page];
    size_t j = page + 1;
    while (j < r->flags.size() && (r->flags[j] & (P_COMMITTED | P_GUARD)) == f0 && ((f0 & P_COMMITTED) == 0 || r->prot[j] == p0)) ++j;
    info->BaseAddress = reinterpret_cast<PVOID>(a);
    info->AllocationBase = reinterpret_cast<PVOID>(r->base);
    info->AllocationProtect = r->allocProtect;
    info->RegionSize = (j - page) * kPage;
    info->State = (f0 & P_COMMITTED) ? MEM_COMMIT : MEM_RESERVE;
    info->Protect = (f0 & P_COMMITTED) ? (p0 | ((f0 & P_GUARD) ? PAGE_GUARD : 0)) : 0;
    info->Type = MEM_PRIVATE;
    return sizeof(*info);
}
extern "C" BOOL VirtualLock(LPVOID a, SIZE_T s) { return mlock(a, s) == 0; }
extern "C" BOOL VirtualUnlock(LPVOID a, SIZE_T s) { return munlock(a, s) == 0; }
extern "C" BOOL FlushInstructionCache(HANDLE, LPCVOID address, SIZE_T size)
{
    if (address && size) __builtin___clear_cache(static_cast<char*>(const_cast<void*>(address)), static_cast<char*>(const_cast<void*>(address)) + size);
    return TRUE;
}

extern "C" UINT ResetWriteWatch(LPVOID base, SIZE_T size)
{
    const uintptr_t a = reinterpret_cast<uintptr_t>(base);
    std::unique_lock lk(g_regionsMutex);
    Region* r = Find(a);
    if (!r || !r->watch) return 1;
    const uintptr_t start = a & ~(kPage - 1);
    const uintptr_t end = std::min<uintptr_t>((a + size + kPage - 1) & ~(kPage - 1), r->base + r->size);
    const size_t first = (start - r->base) / kPage, last = (end - r->base) / kPage;
    size_t i = first;
    while (i < last)
    {
        // Arm runs of writable committed pages with a single mprotect each
        while (i < last && !((r->flags[i] & P_COMMITTED) && Writable(r->prot[i]))) { r->flags[i] &= uint8_t(~P_DIRTY); ++i; }
        size_t j = i;
        while (j < last && (r->flags[j] & P_COMMITTED) && Writable(r->prot[j]))
        {
            r->flags[j] = uint8_t((r->flags[j] & ~P_DIRTY) | P_ARMED);
            ++j;
        }
        if (j > i)
        {
            bool guard = false;
            for (size_t k = i; k < j && !guard; ++k) guard = r->flags[k] & P_GUARD;
            if (guard) Apply(r, i, j);
            else mprotect(reinterpret_cast<void*>(r->base + i * kPage), (j - i) * kPage, HostProt(r->prot[i]) & ~PROT_WRITE);
        }
        i = j;
    }
    return 0;
}

extern "C" UINT GetWriteWatch(DWORD flags, PVOID base, SIZE_T size, PVOID* addresses, ULONG_PTR* count, LPDWORD granularity)
{
    if (!count) return 1;
    const uintptr_t a = reinterpret_cast<uintptr_t>(base);
    std::unique_lock lk(g_regionsMutex);
    Region* r = Find(a);
    if (!r || !r->watch) { *count = 0; return 1; }
    if (granularity) *granularity = DWORD(kPage);
    const uintptr_t start = a & ~(kPage - 1);
    const uintptr_t end = std::min<uintptr_t>((a + size + kPage - 1) & ~(kPage - 1), r->base + r->size);
    const size_t first = (start - r->base) / kPage, last = (end - r->base) / kPage;
    ULONG_PTR n = 0;
    const ULONG_PTR cap = *count;
    std::vector<size_t> rearm;
    for (size_t i = first; i < last && n < cap; ++i)
    {
        if (!(r->flags[i] & P_DIRTY)) continue;
        if (addresses) addresses[n] = reinterpret_cast<PVOID>(r->base + i * kPage);
        ++n;
        if (flags & WRITE_WATCH_FLAG_RESET) rearm.push_back(i);
    }
    *count = n;
    for (size_t k = 0; k < rearm.size();)
    {
        size_t i = rearm[k], j = i + 1, kk = k + 1;
        while (kk < rearm.size() && rearm[kk] == j) { ++j; ++kk; }
        for (size_t p = i; p < j; ++p)
            if ((r->flags[p] & P_COMMITTED) && Writable(r->prot[p])) r->flags[p] = uint8_t((r->flags[p] & ~P_DIRTY) | P_ARMED);
            else r->flags[p] &= uint8_t(~P_DIRTY);
        Apply(r, i, j);
        k = kk;
    }
    return 0;
}

// ======================================================================================
// Vectored exception handling over signals
// ======================================================================================
namespace {

struct Veh { PVECTORED_EXCEPTION_HANDLER fn; bool first; };
std::mutex g_vehMutex;
std::atomic<int> g_vehCount{ 0 };
constexpr int kMaxVeh = 32;
PVECTORED_EXCEPTION_HANDLER g_veh[kMaxVeh];          // copied into a stable array for the handler
std::atomic<PTOP_LEVEL_EXCEPTION_FILTER> g_unhandled{ nullptr };
struct sigaction g_prev[32];
std::once_flag g_installOnce;

bool WriteFault(const ucontext_t* uc, int sig)
{
#if defined(__aarch64__)
    // ESR from the signal frame's esr_context record: WnR (bit 6) for data aborts.
    const auto* ctx = reinterpret_cast<const uint8_t*>(&uc->uc_mcontext.__reserved);
    for (size_t off = 0; off + 8 <= sizeof(uc->uc_mcontext.__reserved);)
    {
        const uint32_t magic = *reinterpret_cast<const uint32_t*>(ctx + off);
        const uint32_t size = *reinterpret_cast<const uint32_t*>(ctx + off + 4);
        if (magic == 0 || size == 0) break;
        if (magic == 0x45535201u /* ESR_MAGIC */)
        {
            const uint64_t esr = *reinterpret_cast<const uint64_t*>(ctx + off + 8);
            const uint32_t ec = uint32_t(esr >> 26) & 0x3F;
            if (ec == 0x24 || ec == 0x25) return (esr >> 6) & 1;   // data abort (lower/same EL)
            return false;
        }
        off += size;
    }
    (void)sig;
    return true;   // unknown: treat as a write (write-watch faults are the common case)
#else
    (void)uc; (void)sig;
    return true;
#endif
}

void FillContext(const ucontext_t* uc, CONTEXT* c)
{
    std::memset(c, 0, sizeof(*c));
    c->ContextFlags = CONTEXT_FULL;
#if defined(__aarch64__)
    for (int i = 0; i < 31; ++i) c->X[i] = uc->uc_mcontext.regs[i];
    c->Sp = uc->uc_mcontext.sp; c->Pc = uc->uc_mcontext.pc; c->Lr = uc->uc_mcontext.regs[30]; c->Fp = uc->uc_mcontext.regs[29];
    c->Cpsr = DWORD(uc->uc_mcontext.pstate);
    c->Rip = c->Pc; c->Rsp = c->Sp; c->Rbp = c->Fp;
    c->Rax = c->X[0]; c->Rcx = c->X[1]; c->Rdx = c->X[2]; c->Rbx = c->X[3];
    c->Rsi = c->X[4]; c->Rdi = c->X[5]; c->R8 = c->X[8]; c->R9 = c->X[9];
    c->R10 = c->X[10]; c->R11 = c->X[11]; c->R12 = c->X[12]; c->R13 = c->X[13]; c->R14 = c->X[14]; c->R15 = c->X[15];
#else
    (void)uc;
#endif
}

void ChainPrevious(int sig, siginfo_t* info, void* uctx)
{
    const struct sigaction& p = g_prev[sig];
    if (p.sa_flags & SA_SIGINFO)
    {
        if (p.sa_sigaction) { p.sa_sigaction(sig, info, uctx); return; }
    }
    else if (p.sa_handler != SIG_DFL && p.sa_handler != SIG_IGN && p.sa_handler)
    {
        p.sa_handler(sig);
        return;
    }
    // Default action: restore it and return; the faulting instruction runs again and the
    // process dies with the original signal (so Android's debuggerd writes a tombstone).
    signal(sig, SIG_DFL);
}

void Handler(int sig, siginfo_t* info, void* uctx)
{
    const int savedErrno = errno;
    auto* uc = static_cast<ucontext_t*>(uctx);
    const uintptr_t addr = reinterpret_cast<uintptr_t>(info->si_addr);

    if (sig == SIGSEGV || sig == SIGBUS)
    {
        // 1) write watch and guard pages
        // Blocking is safe: no code holds this lock while touching guarded memory.
        std::unique_lock lk(g_regionsMutex);
        {
            if (Region* r = Find(addr))
            {
                const size_t page = (addr - r->base) / kPage;
                uint8_t& f = r->flags[page];
                if (r->watch && (f & P_ARMED) && (f & P_COMMITTED) && !(f & P_GUARD) && Writable(r->prot[page]))
                {
                    f = uint8_t((f & ~P_ARMED) | P_DIRTY);
                    mprotect(reinterpret_cast<void*>(r->base + page * kPage), kPage, HostProt(r->prot[page]));
                    errno = savedErrno;
                    return;
                }
                if (f & P_GUARD)
                {
                    f &= uint8_t(~P_GUARD);
                    Apply(r, page, page + 1);
                    lk.unlock();
                    EXCEPTION_RECORD rec{}; CONTEXT ctx;
                    rec.ExceptionCode = STATUS_GUARD_PAGE_VIOLATION;
                    rec.NumberParameters = 2;
                    rec.ExceptionInformation[0] = WriteFault(uc, sig) ? 1 : 0;
                    rec.ExceptionInformation[1] = addr;
                    FillContext(uc, &ctx);
                    rec.ExceptionAddress = reinterpret_cast<PVOID>(ctx.Pc);
                    EXCEPTION_POINTERS ep{ &rec, &ctx };
                    const int n = g_vehCount.load(std::memory_order_acquire);
                    for (int i = 0; i < n; ++i) if (g_veh[i] && g_veh[i](&ep) == EXCEPTION_CONTINUE_EXECUTION) break;
                    errno = savedErrno;
                    return;   // guard semantics: one-shot, the access is retried either way
                }
            }
        }
    }

    // 2) vectored handlers
    EXCEPTION_RECORD rec{};
    CONTEXT ctx;
    FillContext(uc, &ctx);
    rec.ExceptionAddress = reinterpret_cast<PVOID>(ctx.Pc);
    switch (sig)
    {
    case SIGSEGV: case SIGBUS:
        rec.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
        rec.NumberParameters = 2;
        rec.ExceptionInformation[0] = WriteFault(uc, sig) ? 1 : (addr == ctx.Pc ? 8 : 0);
        rec.ExceptionInformation[1] = addr;
        break;
    case SIGILL: rec.ExceptionCode = EXCEPTION_ILLEGAL_INSTRUCTION; break;
    case SIGFPE: rec.ExceptionCode = info->si_code == FPE_INTDIV ? EXCEPTION_INT_DIVIDE_BY_ZERO : EXCEPTION_FLT_INVALID_OPERATION; break;
    case SIGTRAP: rec.ExceptionCode = EXCEPTION_BREAKPOINT; break;
    default: rec.ExceptionCode = 0xE0000000u | DWORD(sig); break;
    }
    EXCEPTION_POINTERS ep{ &rec, &ctx };
    const int n = g_vehCount.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i)
    {
        if (g_veh[i] && g_veh[i](&ep) == EXCEPTION_CONTINUE_EXECUTION)
        {
#if defined(__aarch64__)
            if (ctx.Pc != uc->uc_mcontext.pc && ctx.Pc) uc->uc_mcontext.pc = ctx.Pc;   // a handler that moved the pc
#endif
            errno = savedErrno;
            return;
        }
    }
    // 3) unhandled: the filter logs, then the previous handler / default action
    if (PTOP_LEVEL_EXCEPTION_FILTER filter = g_unhandled.load()) filter(&ep);
    errno = savedErrno;
    ChainPrevious(sig, info, uctx);
}

void Install()
{
    // Each thread that faults needs stack: an alternate stack for the main thread is enough to log
    // a stack overflow there; others run on their own (large) stacks.
    struct sigaction sa{};
    sa.sa_sigaction = Handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    for (int sig : { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP })
        sigaction(sig, &sa, &g_prev[sig]);
    signal(SIGPIPE, SIG_IGN);   // Windows sockets/pipes never raise SIGPIPE
}

} // namespace

extern "C" void Ttt2InstallSignalHandlers(void) { std::call_once(g_installOnce, Install); }

extern "C" PVOID AddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER handler)
{
    Ttt2InstallSignalHandlers();
    std::lock_guard lk(g_vehMutex);
    int n = g_vehCount.load();
    if (n >= kMaxVeh) return nullptr;
    if (first)
    {
        for (int i = n; i > 0; --i) g_veh[i] = g_veh[i - 1];
        g_veh[0] = handler;
    }
    else g_veh[n] = handler;
    g_vehCount.store(n + 1, std::memory_order_release);
    return reinterpret_cast<PVOID>(handler);
}
extern "C" ULONG RemoveVectoredExceptionHandler(PVOID handle)
{
    std::lock_guard lk(g_vehMutex);
    const int n = g_vehCount.load();
    for (int i = 0; i < n; ++i)
        if (reinterpret_cast<PVOID>(g_veh[i]) == handle)
        {
            for (int j = i; j + 1 < n; ++j) g_veh[j] = g_veh[j + 1];
            g_veh[n - 1] = nullptr;
            g_vehCount.store(n - 1, std::memory_order_release);
            return 1;
        }
    return 0;
}
extern "C" PVOID AddVectoredContinueHandler(ULONG, PVECTORED_EXCEPTION_HANDLER h) { return reinterpret_cast<PVOID>(h); }
extern "C" ULONG RemoveVectoredContinueHandler(PVOID) { return 1; }
extern "C" LPTOP_LEVEL_EXCEPTION_FILTER SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER filter)
{
    Ttt2InstallSignalHandlers();
    return g_unhandled.exchange(filter);
}
extern "C" LONG UnhandledExceptionFilter(struct _EXCEPTION_POINTERS*) { return EXCEPTION_CONTINUE_SEARCH; }
extern "C" void RaiseException(DWORD code, DWORD, DWORD, const ULONG_PTR*)
{
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_FATAL, "TTT2", "RaiseException 0x%08X", code);
#endif
    std::fprintf(stderr, "[fatal] RaiseException 0x%08X\n", code);
    abort();
}
extern "C" void RtlCaptureContext(PCONTEXT context)
{
    std::memset(context, 0, sizeof(*context));
    context->Rip = reinterpret_cast<DWORD64>(__builtin_return_address(0));
    context->Rsp = reinterpret_cast<DWORD64>(__builtin_frame_address(0));
    context->Pc = context->Rip; context->Sp = context->Rsp;
}

namespace {
struct TraceState { PVOID* out; DWORD skip, count, n; };
_Unwind_Reason_Code TraceFn(struct _Unwind_Context* ctx, void* arg)
{
    auto* s = static_cast<TraceState*>(arg);
    const uintptr_t pc = _Unwind_GetIP(ctx);
    if (!pc) return _URC_END_OF_STACK;
    if (s->skip) { --s->skip; return _URC_NO_REASON; }
    if (s->n >= s->count) return _URC_END_OF_STACK;
    s->out[s->n++] = reinterpret_cast<PVOID>(pc);
    return _URC_NO_REASON;
}
} // namespace
extern "C" WORD RtlCaptureStackBackTrace(DWORD skip, DWORD count, PVOID* trace, PDWORD hash)
{
    TraceState s{ trace, skip + 1, count, 0 };
    _Unwind_Backtrace(TraceFn, &s);
    if (hash) { DWORD h = 0; for (DWORD i = 0; i < s.n; ++i) h += DWORD(reinterpret_cast<uintptr_t>(trace[i])); *hash = h; }
    return WORD(s.n);
}
extern "C" PRUNTIME_FUNCTION RtlLookupFunctionEntry(DWORD64, PDWORD64 imageBase, PUNWIND_HISTORY_TABLE) { if (imageBase) *imageBase = 0; return nullptr; }
extern "C" PEXCEPTION_ROUTINE RtlVirtualUnwind(DWORD, DWORD64, DWORD64, PRUNTIME_FUNCTION, PCONTEXT, PVOID*, PDWORD64, PKNONVOLATILE_CONTEXT_POINTERS) { return nullptr; }

extern "C" void GetCurrentThreadStackLimits(PULONG_PTR low, PULONG_PTR high)
{
    pthread_attr_t attr;
    void* addr = nullptr; size_t size = 0;
    if (pthread_getattr_np(pthread_self(), &attr) == 0)
    {
        pthread_attr_getstack(&attr, &addr, &size);
        pthread_attr_destroy(&attr);
    }
    if (low) *low = reinterpret_cast<ULONG_PTR>(addr);
    if (high) *high = reinterpret_cast<ULONG_PTR>(addr) + size;
}

// Reads that may fault: process_vm_readv on our own pid fails cleanly on unmapped memory.
extern "C" BOOL ReadProcessMemory(HANDLE, LPCVOID address, LPVOID buffer, SIZE_T size, SIZE_T* read)
{
    iovec local{ buffer, size }, remote{ const_cast<void*>(address), size };
    const ssize_t n = syscall(SYS_process_vm_readv, getpid(), &local, 1, &remote, 1, 0);
    if (n < 0)
    {
        if (errno == ENOSYS || errno == EPERM)
        {
            // Fall back to checking the pages are mapped and readable via write() to a pipe.
            int fds[2];
            if (pipe(fds) == 0)
            {
                const ssize_t w = write(fds[1], address, size);
                close(fds[0]); close(fds[1]);
                if (w == ssize_t(size)) { std::memcpy(buffer, address, size); if (read) *read = size; return TRUE; }
            }
        }
        if (read) *read = 0;
        SetLastError(ERROR_PARTIAL_COPY);
        return FALSE;
    }
    if (read) *read = SIZE_T(n);
    if (SIZE_T(n) != size) { SetLastError(ERROR_PARTIAL_COPY); return FALSE; }
    return TRUE;
}
extern "C" BOOL WriteProcessMemory(HANDLE, LPVOID address, LPCVOID buffer, SIZE_T size, SIZE_T* written)
{
    iovec local{ const_cast<void*>(buffer), size }, remote{ address, size };
    const ssize_t n = syscall(SYS_process_vm_writev, getpid(), &local, 1, &remote, 1, 0);
    if (written) *written = n > 0 ? SIZE_T(n) : 0;
    return n == ssize_t(size);
}
