// Android Win32 layer: handles, errors, threads, synchronization, time, process, modules,
// environment, strings, locale, random/crypt helpers.
#include "win32_internal.h"
#include <process.h>
#include <bcrypt.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <functional>
#include <cmath>
#include <map>
#include <thread>
#include <unordered_map>

#include <dirent.h>
#include <dlfcn.h>
#include <linux/futex.h>
#include <sched.h>
#include <signal.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace w32 {

// ------------------------------------------------------------------------------------
// Errors
// ------------------------------------------------------------------------------------
static thread_local DWORD t_lastError = 0;

uint32_t ErrnoToWin32(int e)
{
    switch (e)
    {
    case 0: return ERROR_SUCCESS;
    case ENOENT: return ERROR_FILE_NOT_FOUND;
    case ENOTDIR: return ERROR_PATH_NOT_FOUND;
    case EACCES: case EPERM: case EROFS: return ERROR_ACCESS_DENIED;
    case EEXIST: return ERROR_ALREADY_EXISTS;
    case EBADF: return ERROR_INVALID_HANDLE;
    case ENOMEM: return ERROR_NOT_ENOUGH_MEMORY;
    case EINVAL: return ERROR_INVALID_PARAMETER;
    case ENOSPC: return ERROR_DISK_FULL;
    case EMFILE: case ENFILE: return ERROR_TOO_MANY_OPEN_FILES;
    case ENOTEMPTY: return ERROR_DIR_NOT_EMPTY;
    case ENAMETOOLONG: return ERROR_FILENAME_EXCED_RANGE;
    case EBUSY: return ERROR_BUSY;
    case EXDEV: return ERROR_NOT_SAME_DEVICE;
    case EISDIR: return ERROR_ACCESS_DENIED;
    case ETIMEDOUT: return ERROR_TIMEOUT;
    case EIO: return ERROR_IO_DEVICE;
    case ENOSYS: return ERROR_CALL_NOT_IMPLEMENTED;
    case EPIPE: return ERROR_BROKEN_PIPE;
    case EAGAIN: return ERROR_IO_PENDING;
    default: return ERROR_INVALID_FUNCTION;
    }
}
void SetErrno(int e) { t_lastError = ErrnoToWin32(e); }

// ------------------------------------------------------------------------------------
// Handles
// ------------------------------------------------------------------------------------
static constexpr uintptr_t kPseudoProcess = uintptr_t(-1);   // == INVALID_HANDLE_VALUE, as on Windows
static constexpr uintptr_t kPseudoThread = uintptr_t(-2);

static std::mutex g_liveMutex;
static std::unordered_map<Object*, bool>& Live() { static auto* m = new std::unordered_map<Object*, bool>(); return *m; }

HANDLE Wrap(Object* o)
{
    std::lock_guard lk(g_liveMutex);
    Live()[o] = true;
    return reinterpret_cast<HANDLE>(o);
}
Object* Get(HANDLE h)
{
    const uintptr_t v = reinterpret_cast<uintptr_t>(h);
    if (!h || v == kPseudoProcess || v == kPseudoThread || v < 0x10000) return nullptr;
    Object* o = reinterpret_cast<Object*>(h);
    std::lock_guard lk(g_liveMutex);
    return Live().count(o) ? o : nullptr;
}
void Release(Object* o)
{
    if (!o) return;
    if (o->refs.fetch_sub(1) == 1)
    {
        {
            std::lock_guard lk(g_liveMutex);
            Live().erase(o);
        }
        delete o;
    }
}

// ------------------------------------------------------------------------------------
// Text
// ------------------------------------------------------------------------------------
std::string Narrow(const wchar_t* w, int len)
{
    std::string out;
    if (!w) return out;
    const size_t n = len < 0 ? std::wcslen(w) : size_t(len);
    out.reserve(n);
    for (size_t i = 0; i < n; ++i)
    {
        uint32_t c = uint32_t(w[i]);
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n && uint32_t(w[i + 1]) >= 0xDC00 && uint32_t(w[i + 1]) <= 0xDFFF)
            c = 0x10000 + ((c - 0xD800) << 10) + (uint32_t(w[++i]) - 0xDC00);   // UTF-16 pair inside a wchar_t string
        if (c < 0x80) out += char(c);
        else if (c < 0x800) { out += char(0xC0 | (c >> 6)); out += char(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { out += char(0xE0 | (c >> 12)); out += char(0x80 | ((c >> 6) & 0x3F)); out += char(0x80 | (c & 0x3F)); }
        else { out += char(0xF0 | (c >> 18)); out += char(0x80 | ((c >> 12) & 0x3F)); out += char(0x80 | ((c >> 6) & 0x3F)); out += char(0x80 | (c & 0x3F)); }
    }
    return out;
}
std::wstring Widen(const char* s, int len)
{
    std::wstring out;
    if (!s) return out;
    const size_t n = len < 0 ? std::strlen(s) : size_t(len);
    for (size_t i = 0; i < n;)
    {
        const unsigned char c = (unsigned char)s[i];
        uint32_t cp; int extra;
        if (c < 0x80) { cp = c; extra = 0; }
        else if ((c >> 5) == 6) { cp = c & 0x1F; extra = 1; }
        else if ((c >> 4) == 14) { cp = c & 0x0F; extra = 2; }
        else if ((c >> 3) == 30) { cp = c & 0x07; extra = 3; }
        else { out += wchar_t(0xFFFD); ++i; continue; }
        if (i + extra >= n + (extra == 0)) { if (extra) { out += wchar_t(0xFFFD); break; } }
        ++i;
        for (int k = 0; k < extra && i < n; ++k, ++i) cp = (cp << 6) | ((unsigned char)s[i] & 0x3F);
        out += wchar_t(cp);
    }
    return out;
}

// ------------------------------------------------------------------------------------
// Time
// ------------------------------------------------------------------------------------
uint64_t MonotonicNs()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}
static constexpr uint64_t kEpochDiff100ns = 116444736000000000ull;   // 1601 -> 1970
uint64_t UnixNsToFileTime(int64_t ns) { return uint64_t(ns / 100) + kEpochDiff100ns; }

pid_t CurrentTid() { static thread_local pid_t tid = pid_t(syscall(SYS_gettid)); return tid; }

// ------------------------------------------------------------------------------------
// Waitable objects
// ------------------------------------------------------------------------------------
struct Event : Object
{
    Event(bool manual, bool initial) : Object(Kind::Event), manual(manual), state(initial) {}
    bool manual, state;
    bool Signaled() override { return state; }
    void Acquire() override { if (!manual) state = false; }
};
struct Semaphore : Object
{
    Semaphore(LONG c, LONG m) : Object(Kind::Semaphore), count(c), max(m) {}
    LONG count, max;
    bool Signaled() override { return count > 0; }
    void Acquire() override { --count; }
};
struct MutexObj : Object
{
    MutexObj() : Object(Kind::Mutex) {}
    pid_t owner = 0; int depth = 0;
    bool Signaled() override { return owner == 0 || owner == CurrentTid(); }
    void Acquire() override { owner = CurrentTid(); ++depth; }
};
struct Timer : Object
{
    Timer(bool manual) : Object(Kind::Timer), manual(manual) {}
    bool manual; bool armed = false; bool state = false; uint64_t dueNs = 0; int64_t periodMs = 0;
    bool Signaled() override
    {
        if (!state && armed && MonotonicNs() >= dueNs)
        {
            state = true;
            if (periodMs > 0) dueNs += uint64_t(periodMs) * 1000000ull; else armed = false;
        }
        return state;
    }
    void Acquire() override { if (!manual) state = false; }
};
struct Thread : Object
{
    Thread() : Object(Kind::Thread) {}
    pid_t tid = 0;
    pthread_t pt{};
    bool hasPthread = false;
    bool done = false;
    DWORD exitCode = STILL_ACTIVE;
    // creation
    LPTHREAD_START_ROUTINE start = nullptr;
    unsigned (*startEx)(void*) = nullptr;
    void* param = nullptr;
    bool suspended = false;
    std::string name;
    bool Signaled() override { return done; }
};

// Thread registry: tid -> thread object (for OpenThread, GetCurrentThread duplication)
static std::mutex g_threadsMutex;
static std::unordered_map<pid_t, Thread*>& Threads() { static auto* m = new std::unordered_map<pid_t, Thread*>(); return *m; }
static thread_local Thread* t_self = nullptr;

static Thread* SelfThread()
{
    if (t_self) return t_self;
    auto* t = new Thread();
    t->tid = CurrentTid();
    t->pt = pthread_self();
    t->hasPthread = true;
    t->refs = 1;   // registry reference, dropped when the thread exits (never, for attached threads)
    {
        std::lock_guard lk(g_threadsMutex);
        Threads()[t->tid] = t;
    }
    Wrap(t);
    t_self = t;
    return t;
}

static uint64_t DeadlineFor(DWORD ms) { return ms == INFINITE ? UINT64_MAX : MonotonicNs() + uint64_t(ms) * 1000000ull; }

static bool WaitOne(Object* o, uint64_t deadline)
{
    std::unique_lock lk(o->m);
    for (;;)
    {
        if (o->Signaled()) { o->Acquire(); return true; }
        if (deadline == UINT64_MAX)
        {
            if (o->kind == Kind::Timer) o->cv.wait_for(lk, std::chrono::milliseconds(1));
            else o->cv.wait(lk);
            continue;
        }
        const uint64_t now = MonotonicNs();
        if (now >= deadline) return false;
        uint64_t wait = deadline - now;
        if (o->kind == Kind::Timer)
        {
            auto* t = static_cast<Timer*>(o);
            if (t->armed && t->dueNs > now) wait = std::min<uint64_t>(wait, t->dueNs - now);
        }
        o->cv.wait_for(lk, std::chrono::nanoseconds(wait));
    }
}

static void NotifyAll(Object* o) { o->cv.notify_all(); }

} // namespace w32

using namespace w32;

// ======================================================================================
// Errors
// ======================================================================================
extern "C" DWORD GetLastError(void) { return t_lastError; }
extern "C" void SetLastError(DWORD e) { t_lastError = e; }
extern "C" UINT SetErrorMode(UINT) { return 0; }

// ======================================================================================
// Handles
// ======================================================================================
extern "C" BOOL CloseHandle(HANDLE h)
{
    Object* o = Get(h);
    if (!o) { SetLastError(ERROR_INVALID_HANDLE); return reinterpret_cast<uintptr_t>(h) >= uintptr_t(-2); }
    Release(o);
    return TRUE;
}
extern "C" BOOL DuplicateHandle(HANDLE, HANDLE src, HANDLE, LPHANDLE dst, DWORD, BOOL, DWORD options)
{
    if (!dst) return FALSE;
    const uintptr_t v = reinterpret_cast<uintptr_t>(src);
    Object* o = nullptr;
    if (v == uintptr_t(-2)) o = SelfThread();
    else if (v == uintptr_t(-1)) { *dst = src; return TRUE; }
    else o = Get(src);
    if (!o) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    o->refs.fetch_add(1);
    *dst = reinterpret_cast<HANDLE>(o);
    if (options & DUPLICATE_CLOSE_SOURCE) Release(o);
    return TRUE;
}
extern "C" BOOL SetHandleInformation(HANDLE, DWORD, DWORD) { return TRUE; }
extern "C" BOOL GetHandleInformation(HANDLE, LPDWORD f) { if (f) *f = 0; return TRUE; }

// ======================================================================================
// Processes
// ======================================================================================
extern "C" HANDLE GetCurrentProcess(void) { return reinterpret_cast<HANDLE>(uintptr_t(-1)); }
extern "C" DWORD GetCurrentProcessId(void) { return DWORD(getpid()); }
extern "C" DWORD GetProcessId(HANDLE h) { return reinterpret_cast<uintptr_t>(h) == uintptr_t(-1) ? DWORD(getpid()) : 0; }
extern "C" HANDLE OpenProcess(DWORD, BOOL, DWORD pid) { return pid == DWORD(getpid()) ? GetCurrentProcess() : nullptr; }
extern "C" [[noreturn]] void ExitProcess(UINT code) { fflush(stdout); fflush(stderr); _exit(int(code)); }
extern "C" BOOL TerminateProcess(HANDLE h, UINT code)
{
    if (reinterpret_cast<uintptr_t>(h) == uintptr_t(-1)) { fflush(stderr); _exit(int(code)); }
    SetLastError(ERROR_ACCESS_DENIED); return FALSE;
}
extern "C" BOOL GetExitCodeProcess(HANDLE h, LPDWORD code)
{
    if (code) *code = STILL_ACTIVE;
    return reinterpret_cast<uintptr_t>(h) == uintptr_t(-1);
}
extern "C" BOOL CreateProcessW(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION)
{ SetLastError(ERROR_NOT_SUPPORTED); return FALSE; }
extern "C" BOOL CreateProcessA(LPCSTR, LPSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCSTR, LPSTARTUPINFOA, LPPROCESS_INFORMATION)
{ SetLastError(ERROR_NOT_SUPPORTED); return FALSE; }
extern "C" BOOL InitializeProcThreadAttributeList(LPPROC_THREAD_ATTRIBUTE_LIST, DWORD, DWORD, PSIZE_T size) { if (size) *size = 64; SetLastError(ERROR_NOT_SUPPORTED); return FALSE; }
extern "C" BOOL UpdateProcThreadAttribute(LPPROC_THREAD_ATTRIBUTE_LIST, DWORD, DWORD_PTR, PVOID, SIZE_T, PVOID, PSIZE_T) { return FALSE; }
extern "C" void DeleteProcThreadAttributeList(LPPROC_THREAD_ATTRIBUTE_LIST) {}
extern "C" HANDLE CreateJobObjectW(LPSECURITY_ATTRIBUTES, LPCWSTR) { return nullptr; }
extern "C" BOOL AssignProcessToJobObject(HANDLE, HANDLE) { return FALSE; }
extern "C" BOOL SetInformationJobObject(HANDLE, JOBOBJECTINFOCLASS, LPVOID, DWORD) { return FALSE; }
extern "C" BOOL SetPriorityClass(HANDLE, DWORD) { return TRUE; }
extern "C" DWORD GetPriorityClass(HANDLE) { return NORMAL_PRIORITY_CLASS; }
extern "C" BOOL SetProcessInformation(HANDLE, PROCESS_INFORMATION_CLASS, LPVOID, DWORD) { return TRUE; }
extern "C" BOOL SetThreadInformation(HANDLE, THREAD_INFORMATION_CLASS, LPVOID, DWORD) { return TRUE; }
extern "C" BOOL SetProcessPriorityBoost(HANDLE, BOOL) { return TRUE; }
extern "C" BOOL SetProcessWorkingSetSize(HANDLE, SIZE_T, SIZE_T) { return TRUE; }
extern "C" EXECUTION_STATE SetThreadExecutionState(EXECUTION_STATE f) { return f; }
extern "C" BOOL IsWow64Process(HANDLE, PBOOL w) { if (w) *w = FALSE; return TRUE; }
extern "C" BOOL GetProcessHandleCount(HANDLE, PDWORD c) { if (c) *c = 0; return TRUE; }
extern "C" BOOL QueryFullProcessImageNameW(HANDLE, DWORD, LPWSTR name, PDWORD size)
{
    char buf[PATH_MAX] = {};
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0 || !name || !size) return FALSE;
    std::wstring w = Widen(buf);
    if (w.size() + 1 > *size) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    std::wmemcpy(name, w.c_str(), w.size() + 1); *size = DWORD(w.size());
    return TRUE;
}
extern "C" BOOL QueryFullProcessImageNameA(HANDLE, DWORD, LPSTR name, PDWORD size)
{
    char buf[PATH_MAX] = {};
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0 || !name || !size || DWORD(n) + 1 > *size) return FALSE;
    std::memcpy(name, buf, size_t(n) + 1); *size = DWORD(n);
    return TRUE;
}
static void FileTimeFromTimeval(const timeval& tv, LPFILETIME ft)
{
    if (!ft) return;
    const uint64_t v = (uint64_t(tv.tv_sec) * 1000000ull + uint64_t(tv.tv_usec)) * 10ull;
    ft->dwLowDateTime = DWORD(v); ft->dwHighDateTime = DWORD(v >> 32);
}
extern "C" BOOL GetProcessTimes(HANDLE, LPFILETIME creation, LPFILETIME exitTime, LPFILETIME kernel, LPFILETIME user)
{
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    if (creation) { creation->dwLowDateTime = creation->dwHighDateTime = 0; }
    if (exitTime) { exitTime->dwLowDateTime = exitTime->dwHighDateTime = 0; }
    FileTimeFromTimeval(ru.ru_stime, kernel);
    FileTimeFromTimeval(ru.ru_utime, user);
    return TRUE;
}
extern "C" BOOL GetThreadTimes(HANDLE, LPFILETIME creation, LPFILETIME exitTime, LPFILETIME kernel, LPFILETIME user)
{
    rusage ru{};
    getrusage(RUSAGE_THREAD, &ru);
    if (creation) { creation->dwLowDateTime = creation->dwHighDateTime = 0; }
    if (exitTime) { exitTime->dwLowDateTime = exitTime->dwHighDateTime = 0; }
    FileTimeFromTimeval(ru.ru_stime, kernel);
    FileTimeFromTimeval(ru.ru_utime, user);
    return TRUE;
}
// "Cycles": CPU time of the thread in nanoseconds (relative values are all callers use).
extern "C" BOOL QueryThreadCycleTime(HANDLE h, PULONG64 cycles)
{
    if (!cycles) return FALSE;
    clockid_t cid = CLOCK_THREAD_CPUTIME_ID;
    Object* o = Get(h);
    if (o && o->kind == Kind::Thread && static_cast<Thread*>(o)->hasPthread && static_cast<Thread*>(o)->tid != CurrentTid())
        if (pthread_getcpuclockid(static_cast<Thread*>(o)->pt, &cid) != 0) cid = CLOCK_THREAD_CPUTIME_ID;
    timespec ts{};
    clock_gettime(cid, &ts);
    *cycles = uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
    return TRUE;
}
extern "C" BOOL QueryProcessCycleTime(HANDLE, PULONG64 cycles)
{
    timespec ts{}; clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    if (cycles) *cycles = uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
    return TRUE;
}
extern "C" BOOL QueryIdleProcessorCycleTime(PULONG size, PULONG64 times) { if (size) *size = 0; (void)times; return FALSE; }

// ======================================================================================
// Threads
// ======================================================================================
extern "C" HANDLE GetCurrentThread(void) { return reinterpret_cast<HANDLE>(uintptr_t(-2)); }
extern "C" DWORD GetCurrentThreadId(void) { return DWORD(CurrentTid()); }
extern "C" DWORD GetCurrentProcessorNumber(void) { const int c = sched_getcpu(); return c < 0 ? 0 : DWORD(c); }
extern "C" void GetCurrentProcessorNumberEx(PPROCESSOR_NUMBER n) { if (n) { n->Group = 0; n->Number = BYTE(GetCurrentProcessorNumber()); n->Reserved = 0; } }
extern "C" DWORD GetActiveProcessorCount(WORD) { const long n = sysconf(_SC_NPROCESSORS_CONF); return n > 0 ? DWORD(n) : 1; }
extern "C" DWORD GetMaximumProcessorCount(WORD g) { return GetActiveProcessorCount(g); }

static Thread* ThreadOf(HANDLE h)
{
    if (reinterpret_cast<uintptr_t>(h) == uintptr_t(-2)) return SelfThread();
    Object* o = Get(h);
    return o && o->kind == Kind::Thread ? static_cast<Thread*>(o) : nullptr;
}
extern "C" DWORD GetThreadId(HANDLE h) { Thread* t = ThreadOf(h); return t ? DWORD(t->tid) : 0; }

static void* ThreadMain(void* arg)
{
    auto* t = static_cast<Thread*>(arg);
    t->tid = CurrentTid();
    t_self = t;
    {
        std::lock_guard lk(g_threadsMutex);
        Threads()[t->tid] = t;
    }
    {
        std::unique_lock lk(t->m);
        t->cv.notify_all();                     // creator waits for the tid
        while (t->suspended) t->cv.wait(lk);
    }
    if (!t->name.empty()) pthread_setname_np(pthread_self(), t->name.substr(0, 15).c_str());
    DWORD code = 0;
    if (t->start) code = t->start(t->param);
    else if (t->startEx) code = t->startEx(t->param);
    {
        std::lock_guard lk(g_threadsMutex);
        Threads().erase(t->tid);
    }
    {
        std::lock_guard lk(t->m);
        t->exitCode = code;
        t->done = true;
    }
    t->cv.notify_all();
    t_self = nullptr;
    Release(t);                                 // the thread's own reference
    return nullptr;
}

static Thread* StartThread(SIZE_T stack, LPTHREAD_START_ROUTINE start, unsigned (*startEx)(void*), void* param, DWORD flags)
{
    auto* t = new Thread();
    t->start = start; t->startEx = startEx; t->param = param;
    t->suspended = (flags & CREATE_SUSPENDED) != 0;
    t->refs = 2;                                // caller's handle + the running thread
    Wrap(t);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, std::max<size_t>(stack ? stack : 1024 * 1024, 256 * 1024));
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    const int rc = pthread_create(&t->pt, &attr, ThreadMain, t);
    pthread_attr_destroy(&attr);
    if (rc != 0) { t->refs = 1; Release(t); SetErrno(rc); return nullptr; }
    t->hasPthread = true;
    std::unique_lock lk(t->m);
    t->cv.wait(lk, [t] { return t->tid != 0; });
    return t;
}

extern "C" HANDLE CreateThread(LPSECURITY_ATTRIBUTES, SIZE_T stack, LPTHREAD_START_ROUTINE start, LPVOID param, DWORD flags, LPDWORD id)
{
    Thread* t = StartThread(stack, start, nullptr, param, flags);
    if (!t) return nullptr;
    if (id) *id = DWORD(t->tid);
    return reinterpret_cast<HANDLE>(t);
}
extern "C" uintptr_t _beginthreadex(void*, unsigned stack, unsigned (*start)(void*), void* arg, unsigned flags, unsigned* id)
{
    Thread* t = StartThread(stack, nullptr, start, arg, flags);
    if (!t) return 0;
    if (id) *id = unsigned(t->tid);
    return reinterpret_cast<uintptr_t>(t);
}
extern "C" uintptr_t _beginthread(void (*start)(void*), unsigned stack, void* arg)
{
    struct Box { void (*fn)(void*); void* arg; };
    auto* box = new Box{ start, arg };
    Thread* t = StartThread(stack, nullptr, [](void* p) -> unsigned { auto* b = static_cast<Box*>(p); b->fn(b->arg); delete b; return 0; }, box, 0);
    if (!t) { delete box; return uintptr_t(-1); }
    Release(t);
    return 1;
}
extern "C" void _endthreadex(unsigned code) { ExitThread(code); }
extern "C" [[noreturn]] void ExitThread(DWORD code)
{
    if (Thread* t = t_self)
    {
        { std::lock_guard lk(g_threadsMutex); Threads().erase(t->tid); }
        { std::lock_guard lk(t->m); t->exitCode = code; t->done = true; }
        t->cv.notify_all();
    }
    pthread_exit(nullptr);
}
extern "C" HANDLE OpenThread(DWORD, BOOL, DWORD id)
{
    if (DWORD(CurrentTid()) == id) { Thread* s = SelfThread(); s->refs.fetch_add(1); return reinterpret_cast<HANDLE>(s); }
    std::lock_guard lk(g_threadsMutex);
    auto it = Threads().find(pid_t(id));
    if (it != Threads().end()) { it->second->refs.fetch_add(1); return reinterpret_cast<HANDLE>(it->second); }
    // A thread this layer did not create (Java/SDL/std::thread before first contact): a tid-only handle.
    if (kill(getpid(), 0) == 0)
    {
        char path[64]; std::snprintf(path, sizeof(path), "/proc/self/task/%u", id);
        if (access(path, F_OK) == 0)
        {
            auto* t = new Thread(); t->tid = pid_t(id);
            return Wrap(t);
        }
    }
    SetLastError(ERROR_INVALID_PARAMETER);
    return nullptr;
}
extern "C" DWORD ResumeThread(HANDLE h)
{
    Thread* t = ThreadOf(h);
    if (!t) return DWORD(-1);
    std::lock_guard lk(t->m);
    const DWORD prev = t->suspended ? 1 : 0;
    t->suspended = false;
    t->cv.notify_all();
    return prev;
}
// Suspending another thread is not possible on Android without ptrace; callers are diagnostics.
extern "C" DWORD SuspendThread(HANDLE) { SetLastError(ERROR_NOT_SUPPORTED); return DWORD(-1); }
extern "C" BOOL GetThreadContext(HANDLE, LPCONTEXT) { SetLastError(ERROR_NOT_SUPPORTED); return FALSE; }
extern "C" BOOL SetThreadContext(HANDLE, const CONTEXT*) { SetLastError(ERROR_NOT_SUPPORTED); return FALSE; }
extern "C" BOOL TerminateThread(HANDLE, DWORD) { SetLastError(ERROR_NOT_SUPPORTED); return FALSE; }
extern "C" BOOL GetExitCodeThread(HANDLE h, LPDWORD code)
{
    Thread* t = ThreadOf(h);
    if (!t || !code) return FALSE;
    std::lock_guard lk(t->m);
    *code = t->done ? t->exitCode : STILL_ACTIVE;
    return TRUE;
}

// Windows priorities -> Linux nice values (per thread on Linux).
static int NiceFor(int priority)
{
    if (priority >= THREAD_PRIORITY_TIME_CRITICAL) return -10;
    if (priority >= THREAD_PRIORITY_HIGHEST) return -8;
    if (priority >= THREAD_PRIORITY_ABOVE_NORMAL) return -4;
    if (priority == THREAD_PRIORITY_NORMAL) return 0;
    if (priority >= THREAD_PRIORITY_BELOW_NORMAL) return 4;
    if (priority >= THREAD_PRIORITY_LOWEST) return 8;
    return 15;
}
static std::mutex g_prioMutex;
static std::unordered_map<pid_t, int>& Priorities() { static auto* m = new std::unordered_map<pid_t, int>(); return *m; }
extern "C" BOOL SetThreadPriority(HANDLE h, int priority)
{
    Thread* t = ThreadOf(h);
    if (!t || !t->tid) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    if (priority == THREAD_MODE_BACKGROUND_BEGIN) priority = THREAD_PRIORITY_IDLE;
    if (priority == THREAD_MODE_BACKGROUND_END) priority = THREAD_PRIORITY_NORMAL;
    setpriority(PRIO_PROCESS, id_t(t->tid), NiceFor(priority));   // may fail without privilege for <0: harmless
    std::lock_guard lk(g_prioMutex);
    Priorities()[t->tid] = priority;
    return TRUE;
}
extern "C" int GetThreadPriority(HANDLE h)
{
    Thread* t = ThreadOf(h);
    if (!t) return THREAD_PRIORITY_ERROR_RETURN;
    std::lock_guard lk(g_prioMutex);
    auto it = Priorities().find(t->tid);
    return it == Priorities().end() ? THREAD_PRIORITY_NORMAL : it->second;
}
extern "C" BOOL SetThreadPriorityBoost(HANDLE, BOOL) { return TRUE; }
extern "C" DWORD_PTR SetThreadAffinityMask(HANDLE h, DWORD_PTR mask)
{
    Thread* t = ThreadOf(h);
    if (!t || !t->tid || !mask) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    cpu_set_t old; CPU_ZERO(&old);
    sched_getaffinity(t->tid, sizeof(old), &old);
    DWORD_PTR prev = 0;
    for (int i = 0; i < 64; ++i) if (CPU_ISSET(i, &old)) prev |= DWORD_PTR(1) << i;
    cpu_set_t set; CPU_ZERO(&set);
    for (int i = 0; i < 64; ++i) if (mask & (DWORD_PTR(1) << i)) CPU_SET(i, &set);
    if (sched_setaffinity(t->tid, sizeof(set), &set) != 0) { SetErrno(errno); return 0; }
    return prev ? prev : 1;
}
extern "C" BOOL GetProcessAffinityMask(HANDLE, PDWORD_PTR processMask, PDWORD_PTR systemMask)
{
    cpu_set_t set; CPU_ZERO(&set);
    sched_getaffinity(0, sizeof(set), &set);
    DWORD_PTR m = 0;
    for (int i = 0; i < 64; ++i) if (CPU_ISSET(i, &set)) m |= DWORD_PTR(1) << i;
    DWORD_PTR all = 0;
    for (DWORD i = 0; i < GetActiveProcessorCount(0) && i < 64; ++i) all |= DWORD_PTR(1) << i;
    if (processMask) *processMask = m ? m : all;
    if (systemMask) *systemMask = all;
    return TRUE;
}
extern "C" BOOL SetProcessAffinityMask(HANDLE, DWORD_PTR) { return TRUE; }
extern "C" DWORD SetThreadIdealProcessor(HANDLE, DWORD) { return 0; }
extern "C" BOOL SetThreadGroupAffinity(HANDLE h, const GROUP_AFFINITY* a, PGROUP_AFFINITY prev)
{
    if (prev) { prev->Mask = 0; prev->Group = 0; }
    return a ? SetThreadAffinityMask(h, a->Mask) != 0 : FALSE;
}
extern "C" BOOL GetThreadGroupAffinity(HANDLE, PGROUP_AFFINITY a) { if (!a) return FALSE; DWORD_PTR p, s; GetProcessAffinityMask(nullptr, &p, &s); a->Mask = p; a->Group = 0; return TRUE; }
extern "C" BOOL SetThreadSelectedCpuSets(HANDLE, const ULONG*, ULONG) { return TRUE; }
extern "C" BOOL GetSystemCpuSetInformation(void*, ULONG, PULONG returned, HANDLE, ULONG) { if (returned) *returned = 0; return FALSE; }
extern "C" HRESULT SetThreadDescription(HANDLE h, PCWSTR name)
{
    Thread* t = ThreadOf(h);
    if (!t) return E_HANDLE;
    std::string n = Narrow(name);
    {
        std::lock_guard lk(t->m);
        t->name = n;
    }
    if (t == t_self || t->tid == CurrentTid()) pthread_setname_np(pthread_self(), n.substr(0, 15).c_str());
    else if (t->hasPthread) pthread_setname_np(t->pt, n.substr(0, 15).c_str());
    return S_OK;
}
extern "C" HRESULT GetThreadDescription(HANDLE h, PWSTR* name)
{
    if (!name) return E_POINTER;
    Thread* t = ThreadOf(h);
    std::string n;
    if (t) { std::lock_guard lk(t->m); n = t->name; }
    if (n.empty() && t && t->tid)
    {
        char path[64]; std::snprintf(path, sizeof(path), "/proc/self/task/%d/comm", t->tid);
        if (FILE* f = std::fopen(path, "r")) { char b[64] = {}; if (std::fgets(b, sizeof(b), f)) n = b; std::fclose(f); }
        while (!n.empty() && (n.back() == '\n' || n.back() == '\r')) n.pop_back();
    }
    std::wstring w = Widen(n.c_str());
    *name = static_cast<PWSTR>(LocalAlloc(LPTR, (w.size() + 1) * sizeof(wchar_t)));
    if (!*name) return E_OUTOFMEMORY;
    std::wmemcpy(*name, w.c_str(), w.size() + 1);
    return S_OK;
}
extern "C" BOOL SwitchToThread(void) { return sched_yield() == 0; }
extern "C" void Sleep(DWORD ms)
{
    if (ms == 0) { sched_yield(); return; }
    if (ms == INFINITE) { for (;;) pause(); }
    timespec ts{ time_t(ms / 1000), long(ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}
extern "C" DWORD SleepEx(DWORD ms, BOOL) { Sleep(ms); return 0; }
extern "C" NTSTATUS NtDelayExecution(BOOLEAN, PLARGE_INTEGER interval)
{
    if (!interval) return 0;
    const int64_t v = interval->QuadPart;
    if (v < 0)
    {
        const uint64_t ns = uint64_t(-v) * 100ull;
        timespec ts{ time_t(ns / 1000000000ull), long(ns % 1000000000ull) };
        while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
    }
    return 0;
}

// TLS
static std::mutex g_tlsMutex;
static bool g_tlsUsed[1088];
static pthread_key_t g_tlsKeys[1088];
extern "C" DWORD TlsAlloc(void)
{
    std::lock_guard lk(g_tlsMutex);
    for (DWORD i = 0; i < 1088; ++i)
        if (!g_tlsUsed[i]) { if (pthread_key_create(&g_tlsKeys[i], nullptr) != 0) return TLS_OUT_OF_INDEXES; g_tlsUsed[i] = true; return i; }
    return TLS_OUT_OF_INDEXES;
}
extern "C" BOOL TlsFree(DWORD i) { std::lock_guard lk(g_tlsMutex); if (i >= 1088 || !g_tlsUsed[i]) return FALSE; pthread_key_delete(g_tlsKeys[i]); g_tlsUsed[i] = false; return TRUE; }
extern "C" LPVOID TlsGetValue(DWORD i) { SetLastError(0); return i < 1088 ? pthread_getspecific(g_tlsKeys[i]) : nullptr; }
extern "C" BOOL TlsSetValue(DWORD i, LPVOID v) { return i < 1088 && pthread_setspecific(g_tlsKeys[i], v) == 0; }

extern "C" BOOL GetLogicalProcessorInformationEx(LOGICAL_PROCESSOR_RELATIONSHIP rel, PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX buffer, PDWORD length)
{
    // One "core" record per CPU; EfficiencyClass from the max frequency (big.LITTLE: bigger = faster cluster).
    if (!length) return FALSE;
    const DWORD n = GetActiveProcessorCount(0);
    if (rel != RelationProcessorCore && rel != RelationAll)
    {
        *length = 0; return TRUE;
    }
    const DWORD need = n * DWORD(sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX));
    if (!buffer || *length < need) { *length = need; SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    std::vector<long> freq(n, 0);
    std::vector<long> levels;
    for (DWORD i = 0; i < n; ++i)
    {
        char path[96]; std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%u/cpufreq/cpuinfo_max_freq", i);
        if (FILE* f = std::fopen(path, "r")) { if (std::fscanf(f, "%ld", &freq[i]) != 1) freq[i] = 0; std::fclose(f); }
        if (std::find(levels.begin(), levels.end(), freq[i]) == levels.end()) levels.push_back(freq[i]);
    }
    std::sort(levels.begin(), levels.end());
    auto* p = buffer;
    for (DWORD i = 0; i < n; ++i, ++p)
    {
        std::memset(p, 0, sizeof(*p));
        p->Relationship = RelationProcessorCore;
        p->Size = sizeof(*p);
        p->Processor.EfficiencyClass = BYTE(std::find(levels.begin(), levels.end(), freq[i]) - levels.begin());
        p->Processor.GroupCount = 1;
        p->Processor.GroupMask[0].Mask = KAFFINITY(1) << i;
    }
    *length = need;
    return TRUE;
}

// ======================================================================================
// Synchronization
// ======================================================================================
extern "C" HANDLE CreateEventW(LPSECURITY_ATTRIBUTES, BOOL manual, BOOL initial, LPCWSTR) { return Wrap(new Event(manual, initial)); }
extern "C" HANDLE CreateEventA(LPSECURITY_ATTRIBUTES, BOOL manual, BOOL initial, LPCSTR) { return Wrap(new Event(manual, initial)); }
extern "C" HANDLE CreateEventExW(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD flags, DWORD) { return Wrap(new Event(flags & CREATE_EVENT_MANUAL_RESET, flags & CREATE_EVENT_INITIAL_SET)); }
extern "C" HANDLE CreateEventExA(LPSECURITY_ATTRIBUTES, LPCSTR, DWORD flags, DWORD) { return Wrap(new Event(flags & CREATE_EVENT_MANUAL_RESET, flags & CREATE_EVENT_INITIAL_SET)); }
extern "C" BOOL SetEvent(HANDLE h)
{
    Object* o = Get(h);
    if (!o || o->kind != Kind::Event) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    { std::lock_guard lk(o->m); static_cast<Event*>(o)->state = true; }
    NotifyAll(o);
    return TRUE;
}
extern "C" BOOL ResetEvent(HANDLE h)
{
    Object* o = Get(h);
    if (!o || o->kind != Kind::Event) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    std::lock_guard lk(o->m); static_cast<Event*>(o)->state = false;
    return TRUE;
}
extern "C" BOOL PulseEvent(HANDLE h) { SetEvent(h); return ResetEvent(h); }
extern "C" HANDLE CreateSemaphoreW(LPSECURITY_ATTRIBUTES, LONG initial, LONG maximum, LPCWSTR) { return Wrap(new Semaphore(initial, maximum)); }
extern "C" HANDLE CreateSemaphoreA(LPSECURITY_ATTRIBUTES, LONG initial, LONG maximum, LPCSTR) { return Wrap(new Semaphore(initial, maximum)); }
extern "C" HANDLE CreateSemaphoreExW(LPSECURITY_ATTRIBUTES, LONG initial, LONG maximum, LPCWSTR, DWORD, DWORD) { return Wrap(new Semaphore(initial, maximum)); }
extern "C" BOOL ReleaseSemaphore(HANDLE h, LONG count, LPLONG previous)
{
    Object* o = Get(h);
    if (!o || o->kind != Kind::Semaphore) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    auto* s = static_cast<Semaphore*>(o);
    {
        std::lock_guard lk(o->m);
        if (previous) *previous = s->count;
        if (s->count + count > s->max) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
        s->count += count;
    }
    NotifyAll(o);
    return TRUE;
}
extern "C" HANDLE CreateMutexW(LPSECURITY_ATTRIBUTES, BOOL owner, LPCWSTR)
{
    auto* m = new MutexObj();
    if (owner) { m->owner = CurrentTid(); m->depth = 1; }
    return Wrap(m);
}
extern "C" HANDLE CreateMutexA(LPSECURITY_ATTRIBUTES sa, BOOL owner, LPCSTR) { return CreateMutexW(sa, owner, nullptr); }
extern "C" BOOL ReleaseMutex(HANDLE h)
{
    Object* o = Get(h);
    if (!o || o->kind != Kind::Mutex) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    auto* m = static_cast<MutexObj*>(o);
    {
        std::lock_guard lk(o->m);
        if (m->owner != CurrentTid()) { SetLastError(ERROR_NOT_OWNER); return FALSE; }
        if (--m->depth == 0) m->owner = 0;
    }
    NotifyAll(o);
    return TRUE;
}
extern "C" HANDLE CreateWaitableTimerW(LPSECURITY_ATTRIBUTES, BOOL manual, LPCWSTR) { return Wrap(new Timer(manual)); }
extern "C" HANDLE CreateWaitableTimerExW(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD flags, DWORD) { return Wrap(new Timer(flags & CREATE_WAITABLE_TIMER_MANUAL_RESET)); }
extern "C" BOOL SetWaitableTimer(HANDLE h, const LARGE_INTEGER* due, LONG period, PTIMERAPCROUTINE, LPVOID, BOOL)
{
    Object* o = Get(h);
    if (!o || o->kind != Kind::Timer || !due) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    auto* t = static_cast<Timer*>(o);
    {
        std::lock_guard lk(o->m);
        const int64_t v = due->QuadPart;
        uint64_t when;
        if (v < 0) when = MonotonicNs() + uint64_t(-v) * 100ull;
        else
        {
            // absolute FILETIME (UTC)
            timespec rt; clock_gettime(CLOCK_REALTIME, &rt);
            const int64_t nowFt = int64_t(UnixNsToFileTime(int64_t(rt.tv_sec) * 1000000000ll + rt.tv_nsec));
            when = MonotonicNs() + uint64_t(std::max<int64_t>(0, v - nowFt)) * 100ull;
        }
        t->dueNs = when; t->periodMs = period; t->armed = true; t->state = false;
    }
    NotifyAll(o);
    return TRUE;
}
extern "C" BOOL SetWaitableTimerEx(HANDLE h, const LARGE_INTEGER* due, LONG period, PTIMERAPCROUTINE fn, LPVOID arg, void*, ULONG)
{ return SetWaitableTimer(h, due, period, fn, arg, FALSE); }
extern "C" BOOL CancelWaitableTimer(HANDLE h)
{
    Object* o = Get(h);
    if (!o || o->kind != Kind::Timer) return FALSE;
    std::lock_guard lk(o->m); static_cast<Timer*>(o)->armed = false;
    return TRUE;
}

extern "C" DWORD WaitForSingleObject(HANDLE h, DWORD ms)
{
    if (reinterpret_cast<uintptr_t>(h) == uintptr_t(-2)) { Sleep(ms); return WAIT_TIMEOUT; }
    Object* o = Get(h);
    if (!o) { SetLastError(ERROR_INVALID_HANDLE); return WAIT_FAILED; }
    if (o->kind == Kind::File || o->kind == Kind::Find || o->kind == Kind::Mapping) return WAIT_OBJECT_0;
    o->refs.fetch_add(1);
    const bool ok = WaitOne(o, DeadlineFor(ms));
    Release(o);
    return ok ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
}
extern "C" DWORD WaitForSingleObjectEx(HANDLE h, DWORD ms, BOOL) { return WaitForSingleObject(h, ms); }
extern "C" DWORD WaitForMultipleObjects(DWORD count, const HANDLE* handles, BOOL all, DWORD ms)
{
    if (count == 0) { Sleep(ms == INFINITE ? 0 : ms); return WAIT_TIMEOUT; }
    if (count == 1) return WaitForSingleObject(handles[0], ms);
    const uint64_t deadline = DeadlineFor(ms);
    // Poll with short sleeps: host-side multi-waits are rare (the guest kernel has its own).
    for (;;)
    {
        DWORD ready = 0;
        for (DWORD i = 0; i < count; ++i)
        {
            Object* o = Get(handles[i]);
            if (!o) { SetLastError(ERROR_INVALID_HANDLE); return WAIT_FAILED; }
            std::lock_guard lk(o->m);
            if (o->Signaled())
            {
                if (!all) { o->Acquire(); return WAIT_OBJECT_0 + i; }
                ++ready;
            }
        }
        if (all && ready == count)
        {
            for (DWORD i = 0; i < count; ++i) { Object* o = Get(handles[i]); std::lock_guard lk(o->m); o->Acquire(); }
            return WAIT_OBJECT_0;
        }
        if (MonotonicNs() >= deadline) return WAIT_TIMEOUT;
        timespec ts{ 0, 500000 };
        nanosleep(&ts, nullptr);
    }
}
extern "C" DWORD WaitForMultipleObjectsEx(DWORD count, const HANDLE* handles, BOOL all, DWORD ms, BOOL) { return WaitForMultipleObjects(count, handles, all, ms); }
extern "C" DWORD SignalObjectAndWait(HANDLE signal, HANDLE wait, DWORD ms, BOOL)
{
    Object* o = Get(signal);
    if (o && o->kind == Kind::Event) SetEvent(signal);
    else if (o && o->kind == Kind::Semaphore) ReleaseSemaphore(signal, 1, nullptr);
    else if (o && o->kind == Kind::Mutex) ReleaseMutex(signal);
    return WaitForSingleObject(wait, ms);
}
extern "C" void Ttt2PumpSdlEvents(DWORD ms);   // win32_sdl.cpp
extern "C" DWORD MsgWaitForMultipleObjects(DWORD count, const HANDLE* handles, BOOL all, DWORD ms, DWORD)
{
    if (count == 0) { Ttt2PumpSdlEvents(ms); return WAIT_TIMEOUT; }
    return WaitForMultipleObjects(count, handles, all, ms);
}
extern "C" DWORD QueueUserAPC(PAPCFUNC, HANDLE, ULONG_PTR) { return 0; }

// WaitOnAddress / WakeByAddress over futexes on the containing aligned 32-bit word.
static long Futex(void* addr, int op, uint32_t val, const timespec* ts)
{
    return syscall(SYS_futex, addr, op | FUTEX_PRIVATE_FLAG, val, ts, nullptr, 0);
}
extern "C" BOOL WaitOnAddress(volatile void* address, PVOID compare, SIZE_T size, DWORD ms)
{
    auto* word = reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(address) & ~uintptr_t(3));
    const uint64_t deadline = DeadlineFor(ms);
    for (;;)
    {
        bool equal;
        switch (size)
        {
        case 1: equal = *static_cast<volatile uint8_t*>(address) == *static_cast<uint8_t*>(compare); break;
        case 2: equal = *static_cast<volatile uint16_t*>(address) == *static_cast<uint16_t*>(compare); break;
        case 8: equal = *static_cast<volatile uint64_t*>(address) == *static_cast<uint64_t*>(compare); break;
        default: equal = *static_cast<volatile uint32_t*>(address) == *static_cast<uint32_t*>(compare); break;
        }
        if (!equal) return TRUE;
        const uint32_t seen = __atomic_load_n(word, __ATOMIC_SEQ_CST);
        timespec ts, *pts = nullptr;
        if (deadline != UINT64_MAX)
        {
            const uint64_t now = MonotonicNs();
            if (now >= deadline) { SetLastError(ERROR_TIMEOUT); return FALSE; }
            const uint64_t left = deadline - now;
            ts.tv_sec = time_t(left / 1000000000ull); ts.tv_nsec = long(left % 1000000000ull); pts = &ts;
        }
        // 64-bit values: the low word may not change; cap the sleep so a high-word change is seen.
        timespec cap{ 0, 2000000 };
        if (size == 8 && (!pts || pts->tv_sec > 0 || pts->tv_nsec > cap.tv_nsec)) pts = &cap;
        Futex(word, FUTEX_WAIT, seen, pts);
        if (deadline == UINT64_MAX || MonotonicNs() < deadline) continue;
        // re-check once after the timeout
        switch (size)
        {
        case 1: equal = *static_cast<volatile uint8_t*>(address) == *static_cast<uint8_t*>(compare); break;
        case 2: equal = *static_cast<volatile uint16_t*>(address) == *static_cast<uint16_t*>(compare); break;
        case 8: equal = *static_cast<volatile uint64_t*>(address) == *static_cast<uint64_t*>(compare); break;
        default: equal = *static_cast<volatile uint32_t*>(address) == *static_cast<uint32_t*>(compare); break;
        }
        if (!equal) return TRUE;
        SetLastError(ERROR_TIMEOUT);
        return FALSE;
    }
}
extern "C" void WakeByAddressSingle(PVOID address)
{
    auto* word = reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(address) & ~uintptr_t(3));
    Futex(word, FUTEX_WAKE, INT_MAX, nullptr);   // all: waiters on other bytes share the word
}
extern "C" void WakeByAddressAll(PVOID address)
{
    auto* word = reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(address) & ~uintptr_t(3));
    Futex(word, FUTEX_WAKE, INT_MAX, nullptr);
}

// CRITICAL_SECTION
static_assert(sizeof(pthread_mutex_t) <= 64, "CRITICAL_SECTION storage");
static pthread_mutex_t* CsMutex(LPCRITICAL_SECTION cs) { return reinterpret_cast<pthread_mutex_t*>(cs->storage); }
extern "C" void InitializeCriticalSection(LPCRITICAL_SECTION cs)
{
    std::memset(cs, 0, sizeof(*cs));
    pthread_mutexattr_t a; pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(CsMutex(cs), &a);
    pthread_mutexattr_destroy(&a);
}
extern "C" BOOL InitializeCriticalSectionAndSpinCount(LPCRITICAL_SECTION cs, DWORD) { InitializeCriticalSection(cs); return TRUE; }
extern "C" BOOL InitializeCriticalSectionEx(LPCRITICAL_SECTION cs, DWORD, DWORD) { InitializeCriticalSection(cs); return TRUE; }
extern "C" void DeleteCriticalSection(LPCRITICAL_SECTION cs) { pthread_mutex_destroy(CsMutex(cs)); }
extern "C" void EnterCriticalSection(LPCRITICAL_SECTION cs) { pthread_mutex_lock(CsMutex(cs)); cs->RecursionCount++; }
extern "C" BOOL TryEnterCriticalSection(LPCRITICAL_SECTION cs) { if (pthread_mutex_trylock(CsMutex(cs)) != 0) return FALSE; cs->RecursionCount++; return TRUE; }
extern "C" void LeaveCriticalSection(LPCRITICAL_SECTION cs) { cs->RecursionCount--; pthread_mutex_unlock(CsMutex(cs)); }
extern "C" DWORD SetCriticalSectionSpinCount(LPCRITICAL_SECTION, DWORD) { return 0; }

// SRWLOCK: lazily created pthread_rwlock (zero-initialised SRWLOCK is valid on Windows)
static pthread_rwlock_t* Rw(PSRWLOCK l)
{
    auto* p = static_cast<pthread_rwlock_t*>(__atomic_load_n(&l->Ptr, __ATOMIC_ACQUIRE));
    if (p) return p;
    auto* fresh = new pthread_rwlock_t;
    pthread_rwlock_init(fresh, nullptr);
    void* expected = nullptr;
    if (__atomic_compare_exchange_n(&l->Ptr, &expected, fresh, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return fresh;
    pthread_rwlock_destroy(fresh); delete fresh;
    return static_cast<pthread_rwlock_t*>(expected);
}
extern "C" void InitializeSRWLock(PSRWLOCK l) { l->Ptr = nullptr; }
extern "C" void AcquireSRWLockExclusive(PSRWLOCK l) { pthread_rwlock_wrlock(Rw(l)); }
extern "C" void ReleaseSRWLockExclusive(PSRWLOCK l) { pthread_rwlock_unlock(Rw(l)); }
extern "C" void AcquireSRWLockShared(PSRWLOCK l) { pthread_rwlock_rdlock(Rw(l)); }
extern "C" void ReleaseSRWLockShared(PSRWLOCK l) { pthread_rwlock_unlock(Rw(l)); }
extern "C" BOOLEAN TryAcquireSRWLockExclusive(PSRWLOCK l) { return pthread_rwlock_trywrlock(Rw(l)) == 0; }
extern "C" BOOLEAN TryAcquireSRWLockShared(PSRWLOCK l) { return pthread_rwlock_tryrdlock(Rw(l)) == 0; }

// CONDITION_VARIABLE: generation counter + futex (works with any lock type)
extern "C" void InitializeConditionVariable(PCONDITION_VARIABLE cv) { cv->Ptr = nullptr; }
static uint32_t* CvWord(PCONDITION_VARIABLE cv) { return reinterpret_cast<uint32_t*>(&cv->Ptr); }
static BOOL CvSleep(PCONDITION_VARIABLE cv, DWORD ms, const std::function<void()>& unlock, const std::function<void()>& lock)
{
    uint32_t* w = CvWord(cv);
    const uint32_t seq = __atomic_load_n(w, __ATOMIC_SEQ_CST);
    unlock();
    timespec ts, *pts = nullptr;
    if (ms != INFINITE) { ts.tv_sec = time_t(ms / 1000); ts.tv_nsec = long(ms % 1000) * 1000000L; pts = &ts; }
    const long r = Futex(w, FUTEX_WAIT, seq, pts);
    const int err = errno;
    lock();
    if (r != 0 && err == ETIMEDOUT) { SetLastError(ERROR_TIMEOUT); return FALSE; }
    return TRUE;
}
extern "C" BOOL SleepConditionVariableCS(PCONDITION_VARIABLE cv, PCRITICAL_SECTION cs, DWORD ms)
{ return CvSleep(cv, ms, [cs] { LeaveCriticalSection(cs); }, [cs] { EnterCriticalSection(cs); }); }
extern "C" BOOL SleepConditionVariableSRW(PCONDITION_VARIABLE cv, PSRWLOCK l, DWORD ms, ULONG flags)
{
    const bool shared = flags & CONDITION_VARIABLE_LOCKMODE_SHARED;
    return CvSleep(cv, ms, [l, shared] { shared ? ReleaseSRWLockShared(l) : ReleaseSRWLockExclusive(l); },
                           [l, shared] { shared ? AcquireSRWLockShared(l) : AcquireSRWLockExclusive(l); });
}
extern "C" void WakeConditionVariable(PCONDITION_VARIABLE cv) { __atomic_add_fetch(CvWord(cv), 1, __ATOMIC_SEQ_CST); Futex(CvWord(cv), FUTEX_WAKE, 1, nullptr); }
extern "C" void WakeAllConditionVariable(PCONDITION_VARIABLE cv) { __atomic_add_fetch(CvWord(cv), 1, __ATOMIC_SEQ_CST); Futex(CvWord(cv), FUTEX_WAKE, INT_MAX, nullptr); }
extern "C" BOOL InitOnceExecuteOnce(PINIT_ONCE once, PINIT_ONCE_FN fn, PVOID param, LPVOID* context)
{
    static std::mutex m;
    std::lock_guard lk(m);
    if (once->Ptr == reinterpret_cast<PVOID>(2)) return TRUE;
    if (!fn(once, param, context)) return FALSE;
    once->Ptr = reinterpret_cast<PVOID>(2);
    return TRUE;
}

// ======================================================================================
// Time
// ======================================================================================
extern "C" BOOL QueryPerformanceCounter(LARGE_INTEGER* c) { c->QuadPart = LONGLONG(MonotonicNs()); return TRUE; }
extern "C" BOOL QueryPerformanceFrequency(LARGE_INTEGER* f) { f->QuadPart = 1000000000ll; return TRUE; }
extern "C" DWORD GetTickCount(void) { return DWORD(MonotonicNs() / 1000000ull); }
extern "C" ULONGLONG GetTickCount64(void) { return MonotonicNs() / 1000000ull; }
extern "C" DWORD timeGetTime(void) { return GetTickCount(); }
extern "C" MMRESULT timeBeginPeriod(UINT) { return TIMERR_NOERROR; }
extern "C" MMRESULT timeEndPeriod(UINT) { return TIMERR_NOERROR; }
extern "C" MMRESULT timeGetDevCaps(LPTIMECAPS caps, UINT) { if (caps) { caps->wPeriodMin = 1; caps->wPeriodMax = 1000000; } return TIMERR_NOERROR; }
extern "C" NTSTATUS NtSetTimerResolution(ULONG desired, BOOLEAN, PULONG current) { if (current) *current = desired ? desired : 5000; return 0; }
extern "C" NTSTATUS NtQueryTimerResolution(PULONG mn, PULONG mx, PULONG cur) { if (mn) *mn = 156250; if (mx) *mx = 5000; if (cur) *cur = 5000; return 0; }
extern "C" BOOL QueryUnbiasedInterruptTime(PULONGLONG t) { if (t) *t = MonotonicNs() / 100ull; return TRUE; }
extern "C" void QueryInterruptTime(PULONGLONG t) { if (t) *t = MonotonicNs() / 100ull; }
extern "C" void QueryInterruptTimePrecise(PULONGLONG t) { if (t) *t = MonotonicNs() / 100ull; }

static uint64_t NowFileTime()
{
    timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    return UnixNsToFileTime(int64_t(ts.tv_sec) * 1000000000ll + ts.tv_nsec);
}
static void PutFt(uint64_t v, LPFILETIME ft) { ft->dwLowDateTime = DWORD(v); ft->dwHighDateTime = DWORD(v >> 32); }
static uint64_t GetFt(const FILETIME* ft) { return (uint64_t(ft->dwHighDateTime) << 32) | ft->dwLowDateTime; }
extern "C" void GetSystemTimeAsFileTime(LPFILETIME ft) { PutFt(NowFileTime(), ft); }
extern "C" void GetSystemTimePreciseAsFileTime(LPFILETIME ft) { PutFt(NowFileTime(), ft); }
static void TmToSystemTime(const tm& t, int ms, LPSYSTEMTIME st)
{
    st->wYear = WORD(t.tm_year + 1900); st->wMonth = WORD(t.tm_mon + 1); st->wDayOfWeek = WORD(t.tm_wday);
    st->wDay = WORD(t.tm_mday); st->wHour = WORD(t.tm_hour); st->wMinute = WORD(t.tm_min); st->wSecond = WORD(t.tm_sec);
    st->wMilliseconds = WORD(ms);
}
extern "C" void GetSystemTime(LPSYSTEMTIME st)
{
    timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    tm t{}; time_t s = ts.tv_sec; gmtime_r(&s, &t);
    TmToSystemTime(t, int(ts.tv_nsec / 1000000), st);
}
extern "C" void GetLocalTime(LPSYSTEMTIME st)
{
    timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    tm t{}; time_t s = ts.tv_sec; localtime_r(&s, &t);
    TmToSystemTime(t, int(ts.tv_nsec / 1000000), st);
}
extern "C" BOOL FileTimeToSystemTime(const FILETIME* ft, LPSYSTEMTIME st)
{
    const uint64_t v = GetFt(ft);
    if (v < kEpochDiff100ns) { std::memset(st, 0, sizeof(*st)); st->wYear = 1601; st->wMonth = 1; st->wDay = 1; return TRUE; }
    const uint64_t u = v - kEpochDiff100ns;
    time_t s = time_t(u / 10000000ull);
    tm t{}; gmtime_r(&s, &t);
    TmToSystemTime(t, int((u / 10000ull) % 1000ull), st);
    return TRUE;
}
extern "C" BOOL SystemTimeToFileTime(const SYSTEMTIME* st, LPFILETIME ft)
{
    tm t{}; t.tm_year = st->wYear - 1900; t.tm_mon = st->wMonth - 1; t.tm_mday = st->wDay;
    t.tm_hour = st->wHour; t.tm_min = st->wMinute; t.tm_sec = st->wSecond;
    const time_t s = timegm(&t);
    PutFt(UnixNsToFileTime(int64_t(s) * 1000000000ll + int64_t(st->wMilliseconds) * 1000000ll), ft);
    return TRUE;
}
static long LocalOffsetSeconds(time_t at) { tm t{}; localtime_r(&at, &t); return t.tm_gmtoff; }
extern "C" BOOL FileTimeToLocalFileTime(const FILETIME* ft, LPFILETIME local)
{
    const uint64_t v = GetFt(ft);
    const time_t s = time_t((v - kEpochDiff100ns) / 10000000ull);
    PutFt(uint64_t(int64_t(v) + int64_t(LocalOffsetSeconds(s)) * 10000000ll), local);
    return TRUE;
}
extern "C" BOOL LocalFileTimeToFileTime(const FILETIME* local, LPFILETIME ft)
{
    const uint64_t v = GetFt(local);
    const time_t s = time_t((v - kEpochDiff100ns) / 10000000ull);
    PutFt(uint64_t(int64_t(v) - int64_t(LocalOffsetSeconds(s)) * 10000000ll), ft);
    return TRUE;
}
extern "C" BOOL SystemTimeToTzSpecificLocalTime(const TIME_ZONE_INFORMATION*, const SYSTEMTIME* utc, LPSYSTEMTIME local)
{
    FILETIME ft, lft;
    SystemTimeToFileTime(utc, &ft);
    FileTimeToLocalFileTime(&ft, &lft);
    return FileTimeToSystemTime(&lft, local);
}
extern "C" DWORD GetTimeZoneInformation(LPTIME_ZONE_INFORMATION tz)
{
    if (!tz) return TIME_ZONE_ID_UNKNOWN;
    std::memset(tz, 0, sizeof(*tz));
    tz->Bias = LONG(-LocalOffsetSeconds(time(nullptr)) / 60);
    return TIME_ZONE_ID_UNKNOWN;
}
extern "C" LONG CompareFileTime(const FILETIME* a, const FILETIME* b)
{
    const uint64_t x = GetFt(a), y = GetFt(b);
    return x < y ? -1 : x > y ? 1 : 0;
}

// ======================================================================================
// System information
// ======================================================================================
extern "C" void GetSystemInfo(LPSYSTEM_INFO si)
{
    std::memset(si, 0, sizeof(*si));
    si->wProcessorArchitecture = PROCESSOR_ARCHITECTURE_ARM64;
    si->dwPageSize = DWORD(sysconf(_SC_PAGESIZE));
    si->lpMinimumApplicationAddress = reinterpret_cast<LPVOID>(0x10000);
    si->lpMaximumApplicationAddress = reinterpret_cast<LPVOID>(0x7FFFFFFFFFFFull);
    si->dwNumberOfProcessors = GetActiveProcessorCount(0);
    DWORD_PTR p, s; GetProcessAffinityMask(nullptr, &p, &s);
    si->dwActiveProcessorMask = s;
    si->dwAllocationGranularity = 0x10000;
}
extern "C" void GetNativeSystemInfo(LPSYSTEM_INFO si) { GetSystemInfo(si); }
extern "C" BOOL GlobalMemoryStatusEx(LPMEMORYSTATUSEX st)
{
    struct sysinfo info{};
    sysinfo(&info);
    const uint64_t unit = info.mem_unit ? info.mem_unit : 1;
    st->ullTotalPhys = uint64_t(info.totalram) * unit;
    st->ullAvailPhys = uint64_t(info.freeram + info.bufferram) * unit;
    // MemAvailable is the better "available" figure
    if (FILE* f = std::fopen("/proc/meminfo", "r"))
    {
        char line[128];
        while (std::fgets(line, sizeof(line), f))
        {
            unsigned long long kb;
            if (std::sscanf(line, "MemAvailable: %llu kB", &kb) == 1) { st->ullAvailPhys = kb * 1024ull; break; }
        }
        std::fclose(f);
    }
    st->dwMemoryLoad = st->ullTotalPhys ? DWORD(100 - st->ullAvailPhys * 100 / st->ullTotalPhys) : 0;
    st->ullTotalPageFile = st->ullTotalPhys + uint64_t(info.totalswap) * unit;
    st->ullAvailPageFile = st->ullAvailPhys + uint64_t(info.freeswap) * unit;
    st->ullTotalVirtual = 0x7FFFFFFFFFFFull;
    st->ullAvailVirtual = 0x7FFF00000000ull;
    st->ullAvailExtendedVirtual = 0;
    return TRUE;
}
extern "C" BOOL IsProcessorFeaturePresent(DWORD f)
{
    switch (f)
    {
    case PF_ARM_NEON_INSTRUCTIONS_AVAILABLE: case PF_FASTFAIL_AVAILABLE: return TRUE;
    // SIMDe provides the SSE family on NEON, so code paths gated on these work.
    case PF_XMMI64_INSTRUCTIONS_AVAILABLE: case PF_SSE3_INSTRUCTIONS_AVAILABLE: case PF_SSSE3_INSTRUCTIONS_AVAILABLE:
    case PF_SSE4_1_INSTRUCTIONS_AVAILABLE: case PF_SSE4_2_INSTRUCTIONS_AVAILABLE: return TRUE;
    default: return FALSE;
    }
}
extern "C" NTSTATUS RtlGetVersion(PRTL_OSVERSIONINFOW info)
{
    if (!info) return -1;
    info->dwMajorVersion = 10; info->dwMinorVersion = 0; info->dwBuildNumber = 19045; info->dwPlatformId = VER_PLATFORM_WIN32_NT;
    info->szCSDVersion[0] = 0;
    return 0;
}
extern "C" BOOL GetVersionExW(LPOSVERSIONINFOW info) { return RtlGetVersion(info) == 0; }
extern "C" BOOL GetSystemPowerStatus(LPSYSTEM_POWER_STATUS st)
{
    if (!st) return FALSE;
    std::memset(st, 0, sizeof(*st));
    st->ACLineStatus = 255; st->BatteryFlag = 255; st->BatteryLifePercent = 255;
    st->BatteryLifeTime = DWORD(-1); st->BatteryFullLifeTime = DWORD(-1);
    int cap = -1;
    if (FILE* f = std::fopen("/sys/class/power_supply/battery/capacity", "r")) { if (std::fscanf(f, "%d", &cap) != 1) cap = -1; std::fclose(f); }
    if (cap >= 0) { st->BatteryLifePercent = BYTE(cap); st->BatteryFlag = 0; }
    char status[32] = {};
    if (FILE* f = std::fopen("/sys/class/power_supply/battery/status", "r")) { if (!std::fgets(status, sizeof(status), f)) status[0] = 0; std::fclose(f); }
    if (status[0]) st->ACLineStatus = (std::strncmp(status, "Charging", 8) == 0 || std::strncmp(status, "Full", 4) == 0) ? 1 : 0;
    return TRUE;
}

// ======================================================================================
// Modules (one module: libmain.so)
// ======================================================================================
static HMODULE SelfModule()
{
    static HMODULE self = [] {
        Dl_info info{};
        dladdr(reinterpret_cast<void*>(&SelfModule), &info);
        return static_cast<HMODULE>(info.dli_fbase);
    }();
    return self;
}
static std::string SelfPath()
{
    Dl_info info{};
    dladdr(reinterpret_cast<void*>(&SelfModule), &info);
    return info.dli_fname ? info.dli_fname : "";
}
extern "C" HMODULE GetModuleHandleW(LPCWSTR name) { return name ? nullptr : SelfModule(); }
extern "C" HMODULE GetModuleHandleA(LPCSTR name) { return name ? nullptr : SelfModule(); }
extern "C" BOOL GetModuleHandleExW(DWORD flags, LPCWSTR name, HMODULE* module)
{
    if (!module) return FALSE;
    if (flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS)
    {
        Dl_info info{};
        if (!dladdr(name, &info) || !info.dli_fbase) { *module = nullptr; return FALSE; }
        *module = static_cast<HMODULE>(info.dli_fbase);
        return TRUE;
    }
    *module = GetModuleHandleW(name);
    return *module != nullptr;
}
extern "C" BOOL GetModuleHandleExA(DWORD flags, LPCSTR name, HMODULE* module) { return GetModuleHandleExW(flags, reinterpret_cast<LPCWSTR>(name), module); }
// GetModuleFileName returns a path under the data root, "<root>\\Tekken Tag Tournament 2.exe", so code
// that derives folders from the executable's location (logs, the game folder) lands in app storage.
extern "C" DWORD GetModuleFileNameA(HMODULE module, LPSTR name, DWORD size)
{
    std::string path;
    // The game's own module: a Windows-style path on drive C:, which the path layer maps to the data
    // root, so "<exe folder>\\game\\tekken.img" is <root>/game/tekken.img.
    if (!module || module == SelfModule()) path = "C:\\Tekken Tag Tournament 2.exe";
    else
    {
        // another loaded library: its real path
        Dl_info info{};
        if (dladdr(module, &info) && info.dli_fname) path = info.dli_fname; else path = SelfPath();
    }
    if (!name || !size) return 0;
    const DWORD n = DWORD(std::min<size_t>(path.size(), size - 1));
    std::memcpy(name, path.c_str(), n); name[n] = 0;
    if (path.size() >= size) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return size; }
    return n;
}
extern "C" DWORD GetModuleFileNameW(HMODULE module, LPWSTR name, DWORD size)
{
    char buf[4096];
    GetModuleFileNameA(module, buf, sizeof(buf));
    std::wstring w = Widen(buf);
    if (!name || !size) return 0;
    const DWORD n = DWORD(std::min<size_t>(w.size(), size - 1));
    std::wmemcpy(name, w.c_str(), n); name[n] = 0;
    if (w.size() >= size) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return size; }
    return n;
}
extern "C" BOOL GetModuleInformation(HANDLE, HMODULE module, LPMODULEINFO info, DWORD)
{
    if (!info) return FALSE;
    info->lpBaseOfDll = module ? module : SelfModule(); info->SizeOfImage = 0; info->EntryPoint = nullptr;
    return TRUE;
}
extern "C" HMODULE LoadLibraryA(LPCSTR name)
{
    if (!name) return nullptr;
    // Windows DLLs do not exist here; anything else may be a real .so (e.g. libvulkan.so).
    const size_t n = std::strlen(name);
    if (n > 4 && strcasecmp(name + n - 4, ".dll") == 0) { SetLastError(ERROR_MOD_NOT_FOUND); return nullptr; }
    void* h = dlopen(name, RTLD_NOW | RTLD_LOCAL);
    if (!h) SetLastError(ERROR_MOD_NOT_FOUND);
    return static_cast<HMODULE>(h);
}
extern "C" HMODULE LoadLibraryW(LPCWSTR name) { return LoadLibraryA(Narrow(name).c_str()); }
extern "C" HMODULE LoadLibraryExA(LPCSTR name, HANDLE, DWORD) { return LoadLibraryA(name); }
extern "C" HMODULE LoadLibraryExW(LPCWSTR name, HANDLE, DWORD) { return LoadLibraryW(name); }
extern "C" BOOL FreeLibrary(HMODULE m) { if (m && m != SelfModule()) dlclose(m); return TRUE; }
extern "C" FARPROC GetProcAddress(HMODULE m, LPCSTR name)
{
    if (!m || !name || reinterpret_cast<uintptr_t>(name) < 0x10000) return nullptr;
    if (m == SelfModule()) return reinterpret_cast<FARPROC>(dlsym(RTLD_DEFAULT, name));
    return reinterpret_cast<FARPROC>(dlsym(m, name));
}
extern "C" BOOL SetDllDirectoryW(LPCWSTR) { return TRUE; }
extern "C" BOOL GetProcessMemoryInfo(HANDLE, PPROCESS_MEMORY_COUNTERS c, DWORD size)
{
    if (!c) return FALSE;
    std::memset(c, 0, size);
    c->cb = size;
    long pages = 0, resident = 0;
    if (FILE* f = std::fopen("/proc/self/statm", "r")) { if (std::fscanf(f, "%ld %ld", &pages, &resident) != 2) pages = resident = 0; std::fclose(f); }
    const SIZE_T page = SIZE_T(sysconf(_SC_PAGESIZE));
    c->WorkingSetSize = SIZE_T(resident) * page;
    c->PeakWorkingSetSize = c->WorkingSetSize;
    c->PagefileUsage = SIZE_T(pages) * page;
    c->PeakPagefileUsage = c->PagefileUsage;
    if (size >= sizeof(PROCESS_MEMORY_COUNTERS_EX)) reinterpret_cast<PROCESS_MEMORY_COUNTERS_EX*>(c)->PrivateUsage = c->PagefileUsage;
    return TRUE;
}

// Toolhelp: enumerate /proc/self/task
struct Snapshot : Object
{
    Snapshot() : Object(Kind::Snapshot) {}
    std::vector<DWORD> tids;
    size_t at = 0;
};
extern "C" HANDLE CreateToolhelp32Snapshot(DWORD flags, DWORD)
{
    if (!(flags & TH32CS_SNAPTHREAD)) { SetLastError(ERROR_NOT_SUPPORTED); return INVALID_HANDLE_VALUE; }
    auto* s = new Snapshot();
    if (DIR* d = opendir("/proc/self/task"))
    {
        while (dirent* e = readdir(d)) if (e->d_name[0] != '.') s->tids.push_back(DWORD(std::strtoul(e->d_name, nullptr, 10)));
        closedir(d);
    }
    return Wrap(s);
}
static BOOL SnapNext(HANDLE h, LPTHREADENTRY32 te)
{
    Object* o = Get(h);
    if (!o || o->kind != Kind::Snapshot || !te) return FALSE;
    auto* s = static_cast<Snapshot*>(o);
    if (s->at >= s->tids.size()) { SetLastError(ERROR_NO_MORE_FILES); return FALSE; }
    te->th32ThreadID = s->tids[s->at++];
    te->th32OwnerProcessID = DWORD(getpid());
    te->cntUsage = 0; te->tpBasePri = 8; te->tpDeltaPri = 0; te->dwFlags = 0;
    return TRUE;
}
extern "C" BOOL Thread32First(HANDLE h, LPTHREADENTRY32 te) { if (Object* o = Get(h)) static_cast<Snapshot*>(o)->at = 0; return SnapNext(h, te); }
extern "C" BOOL Thread32Next(HANDLE h, LPTHREADENTRY32 te) { return SnapNext(h, te); }

// ======================================================================================
// Heap
// ======================================================================================
extern "C" HANDLE GetProcessHeap(void) { return reinterpret_cast<HANDLE>(uintptr_t(0x10)); }
extern "C" LPVOID HeapAlloc(HANDLE, DWORD flags, SIZE_T size) { return (flags & HEAP_ZERO_MEMORY) ? std::calloc(1, size ? size : 1) : std::malloc(size ? size : 1); }
extern "C" LPVOID HeapReAlloc(HANDLE, DWORD, LPVOID mem, SIZE_T size) { return std::realloc(mem, size); }
extern "C" BOOL HeapFree(HANDLE, DWORD, LPVOID mem) { std::free(mem); return TRUE; }
extern "C" HLOCAL LocalAlloc(UINT flags, SIZE_T size) { return (flags & LMEM_ZEROINIT) ? std::calloc(1, size ? size : 1) : std::malloc(size ? size : 1); }
extern "C" HLOCAL LocalFree(HLOCAL mem) { std::free(mem); return nullptr; }
extern "C" HGLOBAL GlobalAlloc(UINT flags, SIZE_T size) { return (flags & GMEM_ZEROINIT) ? std::calloc(1, size ? size : 1) : std::malloc(size ? size : 1); }
extern "C" HGLOBAL GlobalFree(HGLOBAL mem) { std::free(mem); return nullptr; }
extern "C" LPVOID GlobalLock(HGLOBAL mem) { return mem; }
extern "C" BOOL GlobalUnlock(HGLOBAL) { return TRUE; }
extern "C" void CoTaskMemFree(LPVOID p) { std::free(p); }
extern "C" LPVOID CoTaskMemAlloc(SIZE_T size) { return std::malloc(size); }
extern "C" HRESULT CoInitialize(LPVOID) { return S_OK; }
extern "C" HRESULT CoInitializeEx(LPVOID, DWORD) { return S_OK; }
extern "C" void CoUninitialize(void) {}
extern "C" HRESULT CoCreateInstance(REFCLSID, IUnknown*, DWORD, REFIID, LPVOID* out) { if (out) *out = nullptr; return E_NOINTERFACE; }
extern "C" BSTR SysAllocString(const OLECHAR* s) { if (!s) return nullptr; const size_t n = std::wcslen(s); auto* p = static_cast<BSTR>(std::malloc((n + 1) * sizeof(OLECHAR))); std::wmemcpy(p, s, n + 1); return p; }
extern "C" void SysFreeString(BSTR s) { std::free(s); }

// ======================================================================================
// Environment, command line, names
// ======================================================================================
extern "C" DWORD GetEnvironmentVariableA(LPCSTR name, LPSTR buffer, DWORD size)
{
    const char* v = name ? std::getenv(name) : nullptr;
    if (!v) { SetLastError(ERROR_ENVVAR_NOT_FOUND); return 0; }
    const DWORD n = DWORD(std::strlen(v));
    if (!buffer || size <= n) return n + 1;
    std::memcpy(buffer, v, n + 1);
    return n;
}
extern "C" DWORD GetEnvironmentVariableW(LPCWSTR name, LPWSTR buffer, DWORD size)
{
    const char* v = name ? std::getenv(Narrow(name).c_str()) : nullptr;
    if (!v) { SetLastError(ERROR_ENVVAR_NOT_FOUND); return 0; }
    std::wstring w = Widen(v);
    if (!buffer || size <= w.size()) return DWORD(w.size() + 1);
    std::wmemcpy(buffer, w.c_str(), w.size() + 1);
    return DWORD(w.size());
}
extern "C" BOOL SetEnvironmentVariableA(LPCSTR name, LPCSTR value) { return (value ? setenv(name, value, 1) : unsetenv(name)) == 0; }
extern "C" BOOL SetEnvironmentVariableW(LPCWSTR name, LPCWSTR value) { return SetEnvironmentVariableA(Narrow(name).c_str(), value ? Narrow(value).c_str() : nullptr); }
extern "C" wchar_t* _wgetenv(const wchar_t* name)
{
    // The result must stay valid: keep one converted copy per variable name.
    static std::mutex m;
    static std::map<std::string, std::wstring>* cache = new std::map<std::string, std::wstring>();
    const std::string n = Narrow(name);
    const char* v = std::getenv(n.c_str());
    if (!v) return nullptr;
    std::lock_guard lk(m);
    std::wstring& slot = (*cache)[n];
    std::wstring w = Widen(v);
    if (slot != w) slot = w;   // same storage when unchanged
    return slot.data();
}
extern "C" int _wputenv_s(const wchar_t* name, const wchar_t* value) { return _putenv_s(Narrow(name).c_str(), Narrow(value).c_str()); }
extern "C" DWORD ExpandEnvironmentStringsA(LPCSTR src, LPSTR dst, DWORD size)
{
    std::string out;
    for (const char* p = src; p && *p;)
    {
        if (*p == '%')
        {
            const char* e = std::strchr(p + 1, '%');
            if (e) { std::string n(p + 1, e); const char* v = std::getenv(n.c_str()); out += v ? v : std::string(p, e + 1); p = e + 1; continue; }
        }
        out += *p++;
    }
    if (dst && size > out.size()) std::memcpy(dst, out.c_str(), out.size() + 1);
    return DWORD(out.size() + 1);
}
extern "C" DWORD ExpandEnvironmentStringsW(LPCWSTR src, LPWSTR dst, DWORD size)
{
    char buf[8192];
    ExpandEnvironmentStringsA(Narrow(src).c_str(), buf, sizeof(buf));
    std::wstring w = Widen(buf);
    if (dst && size > w.size()) std::wmemcpy(dst, w.c_str(), w.size() + 1);
    return DWORD(w.size() + 1);
}
static std::wstring g_cmdW = L"\"Tekken Tag Tournament 2.exe\"";
static std::string g_cmdA = "\"Tekken Tag Tournament 2.exe\"";
extern "C" LPWSTR GetCommandLineW(void) { return g_cmdW.data(); }
extern "C" LPSTR GetCommandLineA(void) { return g_cmdA.data(); }
extern "C" void Ttt2SetCommandLine(int argc, char** argv)
{
    std::string c;
    for (int i = 0; i < argc; ++i) { if (i) c += ' '; c += '"'; c += argv[i]; c += '"'; }
    g_cmdA = c; g_cmdW = Widen(c.c_str());
}
extern "C" LPWSTR* CommandLineToArgvW(LPCWSTR cmd, int* argc)
{
    std::vector<std::wstring> args;
    std::wstring cur; bool q = false, any = false;
    for (const wchar_t* p = cmd ? cmd : L""; *p; ++p)
    {
        if (*p == L'"') { q = !q; any = true; continue; }
        if (!q && (*p == L' ' || *p == L'\t')) { if (any || !cur.empty()) args.push_back(cur); cur.clear(); any = false; continue; }
        cur += *p; any = true;
    }
    if (any || !cur.empty()) args.push_back(cur);
    size_t bytes = (args.size() + 1) * sizeof(LPWSTR);
    for (auto& a : args) bytes += (a.size() + 1) * sizeof(wchar_t);
    auto** out = static_cast<LPWSTR*>(LocalAlloc(LPTR, bytes));
    auto* text = reinterpret_cast<wchar_t*>(out + args.size() + 1);
    for (size_t i = 0; i < args.size(); ++i) { out[i] = text; std::wmemcpy(text, args[i].c_str(), args[i].size() + 1); text += args[i].size() + 1; }
    if (argc) *argc = int(args.size());
    return out;
}
extern "C" BOOL GetComputerNameA(LPSTR name, LPDWORD size)
{
    utsname u{}; uname(&u);
    const std::string n = u.nodename[0] ? u.nodename : "ANDROID";
    if (!name || !size || *size <= n.size()) { if (size) *size = DWORD(n.size() + 1); SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    std::memcpy(name, n.c_str(), n.size() + 1); *size = DWORD(n.size());
    return TRUE;
}
extern "C" BOOL GetComputerNameW(LPWSTR name, LPDWORD size)
{
    char b[256]; DWORD n = sizeof(b);
    if (!GetComputerNameA(b, &n)) return FALSE;
    std::wstring w = Widen(b);
    if (!name || !size || *size <= w.size()) { if (size) *size = DWORD(w.size() + 1); return FALSE; }
    std::wmemcpy(name, w.c_str(), w.size() + 1); *size = DWORD(w.size());
    return TRUE;
}
extern "C" BOOL GetUserNameA(LPSTR name, LPDWORD size)
{
    const char* n = "Player";
    if (!name || !size || *size <= std::strlen(n)) { if (size) *size = DWORD(std::strlen(n) + 1); return FALSE; }
    std::strcpy(name, n); *size = DWORD(std::strlen(n) + 1);
    return TRUE;
}
extern "C" BOOL GetUserNameW(LPWSTR name, LPDWORD size)
{
    const wchar_t* n = L"Player";
    if (!name || !size || *size <= std::wcslen(n)) { if (size) *size = DWORD(std::wcslen(n) + 1); return FALSE; }
    std::wcscpy(name, n); *size = DWORD(std::wcslen(n) + 1);
    return TRUE;
}

// ======================================================================================
// Strings / code pages (wchar_t is 32-bit here: "wide" = UTF-32)
// ======================================================================================
extern "C" int MultiByteToWideChar(UINT, DWORD, LPCSTR src, int srcLen, LPWSTR dst, int dstLen)
{
    if (!src) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    const bool withNul = srcLen < 0;
    std::wstring w = Widen(src, withNul ? -1 : srcLen);
    const int need = int(w.size()) + (withNul ? 1 : 0);
    if (dstLen == 0) return need;
    if (!dst || dstLen < need) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return 0; }
    std::wmemcpy(dst, w.c_str(), w.size());
    if (withNul) dst[w.size()] = 0;
    return need;
}
extern "C" int WideCharToMultiByte(UINT, DWORD, LPCWSTR src, int srcLen, LPSTR dst, int dstLen, LPCSTR, LPBOOL usedDef)
{
    if (usedDef) *usedDef = FALSE;
    if (!src) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    const bool withNul = srcLen < 0;
    std::string s = Narrow(src, withNul ? -1 : srcLen);
    const int need = int(s.size()) + (withNul ? 1 : 0);
    if (dstLen == 0) return need;
    if (!dst || dstLen < need) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return 0; }
    std::memcpy(dst, s.data(), s.size());
    if (withNul) dst[s.size()] = 0;
    return need;
}
extern "C" UINT GetACP(void) { return CP_UTF8; }
extern "C" UINT GetOEMCP(void) { return CP_UTF8; }
static std::string LocaleTag()
{
    // Android: the launcher passes the system language in TTT2_LOCALE (e.g. "en-US"); fall back to LANG.
    const char* l = std::getenv("TTT2_LOCALE");
    if (!l || !*l) l = std::getenv("LANG");
    std::string s = (l && *l) ? l : "en-US";
    for (char& c : s) if (c == '_') c = '-';
    const size_t dot = s.find('.'); if (dot != std::string::npos) s.resize(dot);
    if (s == "C" || s == "POSIX") s = "en-US";
    return s;
}
static LANGID LangIdFromTag(const std::string& tag)
{
    const std::string lang = tag.substr(0, 2);
    const std::string rest = tag.size() > 3 ? tag.substr(3) : "";
    if (lang == "ja") return MAKELANGID(LANG_JAPANESE, SUBLANG_DEFAULT);
    if (lang == "fr") return MAKELANGID(LANG_FRENCH, SUBLANG_DEFAULT);
    if (lang == "de") return MAKELANGID(LANG_GERMAN, SUBLANG_DEFAULT);
    if (lang == "it") return MAKELANGID(LANG_ITALIAN, SUBLANG_DEFAULT);
    if (lang == "es") return MAKELANGID(LANG_SPANISH, SUBLANG_DEFAULT);
    if (lang == "pt") return MAKELANGID(LANG_PORTUGUESE, SUBLANG_DEFAULT);
    if (lang == "ko") return MAKELANGID(LANG_KOREAN, SUBLANG_DEFAULT);
    if (lang == "ru") return MAKELANGID(LANG_RUSSIAN, SUBLANG_DEFAULT);
    if (lang == "pl") return MAKELANGID(LANG_POLISH, SUBLANG_DEFAULT);
    if (lang == "zh") return MAKELANGID(LANG_CHINESE, (rest == "TW" || rest == "HK" || rest == "Hant") ? SUBLANG_CHINESE_TRADITIONAL : SUBLANG_CHINESE_SIMPLIFIED);
    return MAKELANGID(LANG_ENGLISH, SUBLANG_DEFAULT);
}
extern "C" LANGID GetUserDefaultUILanguage(void) { return LangIdFromTag(LocaleTag()); }
extern "C" LANGID GetSystemDefaultUILanguage(void) { return GetUserDefaultUILanguage(); }
extern "C" LANGID GetUserDefaultLangID(void) { return GetUserDefaultUILanguage(); }
extern "C" LCID GetUserDefaultLCID(void) { return GetUserDefaultUILanguage(); }
extern "C" int GetUserDefaultLocaleName(LPWSTR name, int size)
{
    std::wstring w = Widen(LocaleTag().c_str());
    if (!name || size <= int(w.size())) return 0;
    std::wmemcpy(name, w.c_str(), w.size() + 1);
    return int(w.size() + 1);
}
extern "C" BOOL GetUserPreferredUILanguages(DWORD, PULONG count, PWSTR buffer, PULONG size)
{
    std::wstring w = Widen(LocaleTag().c_str());
    const ULONG need = ULONG(w.size() + 2);
    if (count) *count = 1;
    if (!buffer || !size || *size < need) { if (size) *size = need; return buffer == nullptr; }
    std::wmemcpy(buffer, w.c_str(), w.size()); buffer[w.size()] = 0; buffer[w.size() + 1] = 0;
    *size = need;
    return TRUE;
}
static int LocaleInfo(DWORD type, std::string& out)
{
    const std::string tag = LocaleTag();
    switch (type & ~LOCALE_RETURN_NUMBER)
    {
    case LOCALE_SISO639LANGNAME: out = tag.substr(0, 2); return 1;
    case LOCALE_SISO3166CTRYNAME: out = tag.size() >= 5 ? tag.substr(3, 2) : "US"; return 1;
    case LOCALE_SNAME: out = tag; return 1;
    case LOCALE_IGEOID: out = "244"; return 1;
    default: return 0;
    }
}
extern "C" int GetLocaleInfoEx(LPCWSTR, DWORD type, LPWSTR data, int size)
{
    std::string v;
    if (!LocaleInfo(type, v)) { SetLastError(ERROR_INVALID_FLAGS); return 0; }
    if (type & LOCALE_RETURN_NUMBER) { if (data && size >= 2) *reinterpret_cast<DWORD*>(data) = DWORD(std::atoi(v.c_str())); return 2; }
    std::wstring w = Widen(v.c_str());
    if (!data || size == 0) return int(w.size() + 1);
    if (size <= int(w.size())) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return 0; }
    std::wmemcpy(data, w.c_str(), w.size() + 1);
    return int(w.size() + 1);
}
extern "C" int GetLocaleInfoW(LCID, DWORD type, LPWSTR data, int size) { return GetLocaleInfoEx(nullptr, type, data, size); }
extern "C" int GetLocaleInfoA(LCID, DWORD type, LPSTR data, int size)
{
    std::string v;
    if (!LocaleInfo(type, v)) return 0;
    if (!data || size == 0) return int(v.size() + 1);
    if (size <= int(v.size())) return 0;
    std::memcpy(data, v.c_str(), v.size() + 1);
    return int(v.size() + 1);
}
extern "C" LONG GetUserGeoID(DWORD) { return 244; }
extern "C" int GetGeoInfoW(LONG, DWORD, LPWSTR data, int size, LANGID)
{
    std::string c; LocaleInfo(LOCALE_SISO3166CTRYNAME, c);
    std::wstring w = Widen(c.c_str());
    if (!data || size == 0) return int(w.size() + 1);
    if (size <= int(w.size())) return 0;
    std::wmemcpy(data, w.c_str(), w.size() + 1);
    return int(w.size() + 1);
}
extern "C" DWORD FormatMessageA(DWORD flags, LPCVOID, DWORD id, DWORD, LPSTR buffer, DWORD size, va_list*)
{
    char text[64];
    std::snprintf(text, sizeof(text), "Win32 error %u", id);
    const size_t n = std::strlen(text);
    if (flags & FORMAT_MESSAGE_ALLOCATE_BUFFER)
    {
        auto* p = static_cast<char*>(LocalAlloc(LPTR, n + 1));
        std::memcpy(p, text, n + 1);
        *reinterpret_cast<char**>(buffer) = p;
        return DWORD(n);
    }
    if (!buffer || size <= n) return 0;
    std::memcpy(buffer, text, n + 1);
    return DWORD(n);
}
extern "C" DWORD FormatMessageW(DWORD flags, LPCVOID src, DWORD id, DWORD lang, LPWSTR buffer, DWORD size, va_list* args)
{
    char text[64];
    std::snprintf(text, sizeof(text), "Win32 error %u", id);
    std::wstring w = Widen(text);
    if (flags & FORMAT_MESSAGE_ALLOCATE_BUFFER)
    {
        auto* p = static_cast<wchar_t*>(LocalAlloc(LPTR, (w.size() + 1) * sizeof(wchar_t)));
        std::wmemcpy(p, w.c_str(), w.size() + 1);
        *reinterpret_cast<wchar_t**>(buffer) = p;
        return DWORD(w.size());
    }
    (void)src; (void)lang; (void)args;
    if (!buffer || size <= w.size()) return 0;
    std::wmemcpy(buffer, w.c_str(), w.size() + 1);
    return DWORD(w.size());
}
extern "C" int lstrlenW(LPCWSTR s) { return s ? int(std::wcslen(s)) : 0; }
extern "C" int lstrlenA(LPCSTR s) { return s ? int(std::strlen(s)) : 0; }
extern "C" int lstrcmpiW(LPCWSTR a, LPCWSTR b) { return wcscasecmp(a, b); }
extern "C" int lstrcmpiA(LPCSTR a, LPCSTR b) { return strcasecmp(a, b); }
extern "C" int lstrcmpW(LPCWSTR a, LPCWSTR b) { return std::wcscmp(a, b); }
extern "C" LPWSTR lstrcpynW(LPWSTR d, LPCWSTR s, int n) { if (n <= 0) return d; wcsncpy_s(d, size_t(n), s, size_t(n - 1)); return d; }
extern "C" LPSTR lstrcpynA(LPSTR d, LPCSTR s, int n) { if (n <= 0) return d; strncpy_s(d, size_t(n), s, size_t(n - 1)); return d; }
extern "C" LPWSTR CharLowerW(LPWSTR s) { return _wcslwr(s); }
extern "C" LPWSTR CharUpperW(LPWSTR s) { return _wcsupr(s); }
extern "C" LPSTR CharLowerA(LPSTR s) { return _strlwr(s); }
extern "C" LPSTR CharUpperA(LPSTR s) { return _strupr(s); }
extern "C" int CompareStringOrdinal(LPCWCH a, int alen, LPCWCH b, int blen, BOOL ignoreCase)
{
    std::wstring x(a, alen < 0 ? std::wcslen(a) : size_t(alen)), y(b, blen < 0 ? std::wcslen(b) : size_t(blen));
    if (ignoreCase) { for (auto& c : x) c = wchar_t(std::towupper(c)); for (auto& c : y) c = wchar_t(std::towupper(c)); }
    const int r = x.compare(y);
    return r < 0 ? CSTR_LESS_THAN : r > 0 ? CSTR_GREATER_THAN : CSTR_EQUAL;
}

// ======================================================================================
// Random / hashing / base64 / DPAPI
// ======================================================================================
extern "C" NTSTATUS BCryptGenRandom(BCRYPT_ALG_HANDLE, PUCHAR buffer, ULONG size, ULONG)
{
    size_t done = 0;
    while (done < size)
    {
        const ssize_t r = getrandom(buffer + done, size - done, 0);
        if (r < 0) { if (errno == EINTR) continue; return NTSTATUS(0xC0000001); }
        done += size_t(r);
    }
    return 0;
}

namespace {
struct Sha256
{
    uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    uint8_t buf[64]; size_t len = 0; uint64_t bits = 0;
    static uint32_t R(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    void Block(const uint8_t* p)
    {
        static const uint32_t k[64] = {
            0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
            0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
            0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
            0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
            0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
            0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) w[i] = uint32_t(p[i * 4]) << 24 | uint32_t(p[i * 4 + 1]) << 16 | uint32_t(p[i * 4 + 2]) << 8 | p[i * 4 + 3];
        for (int i = 16; i < 64; ++i)
        {
            const uint32_t s0 = R(w[i - 15], 7) ^ R(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = R(w[i - 2], 17) ^ R(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i)
        {
            const uint32_t t1 = hh + (R(e, 6) ^ R(e, 11) ^ R(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const uint32_t t2 = (R(a, 2) ^ R(a, 13) ^ R(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    void Update(const uint8_t* p, size_t n)
    {
        bits += uint64_t(n) * 8;
        while (n--) { buf[len++] = *p++; if (len == 64) { Block(buf); len = 0; } }
    }
    void Final(uint8_t out[32])
    {
        const uint64_t b = bits;
        uint8_t one = 0x80; Update(&one, 1);
        uint8_t zero = 0; while (len != 56) Update(&zero, 1);
        uint8_t lenBytes[8]; for (int i = 0; i < 8; ++i) lenBytes[i] = uint8_t(b >> (56 - 8 * i));
        for (int i = 0; i < 8; ++i) { buf[len++] = lenBytes[i]; } Block(buf); len = 0;
        for (int i = 0; i < 8; ++i) { out[i * 4] = uint8_t(h[i] >> 24); out[i * 4 + 1] = uint8_t(h[i] >> 16); out[i * 4 + 2] = uint8_t(h[i] >> 8); out[i * 4 + 3] = uint8_t(h[i]); }
    }
};
struct Sha1
{
    uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    uint8_t buf[64]; size_t len = 0; uint64_t bits = 0;
    static uint32_t L(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
    void Block(const uint8_t* p)
    {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) w[i] = uint32_t(p[i * 4]) << 24 | uint32_t(p[i * 4 + 1]) << 16 | uint32_t(p[i * 4 + 2]) << 8 | p[i * 4 + 3];
        for (int i = 16; i < 80; ++i) w[i] = L(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i)
        {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            const uint32_t t = L(a, 5) + f + e + k + w[i];
            e = d; d = c; c = L(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    void Update(const uint8_t* p, size_t n)
    {
        bits += uint64_t(n) * 8;
        while (n--) { buf[len++] = *p++; if (len == 64) { Block(buf); len = 0; } }
    }
    void Final(uint8_t out[20])
    {
        const uint64_t b = bits;
        uint8_t one = 0x80; Update(&one, 1);
        uint8_t zero = 0; while (len != 56) Update(&zero, 1);
        for (int i = 0; i < 8; ++i) buf[len++] = uint8_t(b >> (56 - 8 * i));
        Block(buf); len = 0;
        for (int i = 0; i < 5; ++i) { out[i * 4] = uint8_t(h[i] >> 24); out[i * 4 + 1] = uint8_t(h[i] >> 16); out[i * 4 + 2] = uint8_t(h[i] >> 8); out[i * 4 + 3] = uint8_t(h[i]); }
    }
};
// A BCrypt hash object: SHA-1 or SHA-256
struct HashObj { bool sha1; Sha1 a; Sha256 b; };
} // namespace
extern "C" void Ttt2Sha256(const void* data, size_t size, uint8_t out[32]) { Sha256 s; s.Update(static_cast<const uint8_t*>(data), size); s.Final(out); }

extern "C" NTSTATUS BCryptOpenAlgorithmProvider(BCRYPT_ALG_HANDLE* alg, LPCWSTR id, LPCWSTR, ULONG)
{
    if (!alg || !id) return NTSTATUS(0xC000000D);
    if (wcscasecmp(id, L"SHA256") == 0) { *alg = BCRYPT_SHA256_ALG_HANDLE; return 0; }
    if (wcscasecmp(id, L"SHA1") == 0) { *alg = BCRYPT_SHA1_ALG_HANDLE; return 0; }
    if (wcscasecmp(id, L"RNG") == 0) { *alg = reinterpret_cast<BCRYPT_ALG_HANDLE>(uintptr_t(0x81)); return 0; }
    return NTSTATUS(0xC00000BB);
}
extern "C" NTSTATUS BCryptCloseAlgorithmProvider(BCRYPT_ALG_HANDLE, ULONG) { return 0; }
extern "C" NTSTATUS BCryptGetProperty(BCRYPT_HANDLE obj, LPCWSTR prop, PUCHAR out, ULONG size, ULONG* result, ULONG)
{
    DWORD v = 0;
    if (wcscasecmp(prop, L"ObjectLength") == 0) v = sizeof(HashObj);
    else if (wcscasecmp(prop, L"HashDigestLength") == 0) v = obj == BCRYPT_SHA1_ALG_HANDLE ? 20 : 32;
    else return NTSTATUS(0xC00000BB);
    if (result) *result = sizeof(DWORD);
    if (out && size >= sizeof(DWORD)) std::memcpy(out, &v, sizeof(v));
    return 0;
}
extern "C" NTSTATUS BCryptCreateHash(BCRYPT_ALG_HANDLE alg, BCRYPT_HASH_HANDLE* hash, PUCHAR, ULONG, PUCHAR, ULONG, ULONG)
{
    auto* h = new HashObj(); h->sha1 = alg == BCRYPT_SHA1_ALG_HANDLE; *hash = h; return 0;
}
extern "C" NTSTATUS BCryptHashData(BCRYPT_HASH_HANDLE hash, PUCHAR input, ULONG size, ULONG)
{
    auto* h = static_cast<HashObj*>(hash);
    if (h->sha1) h->a.Update(input, size); else h->b.Update(input, size);
    return 0;
}
extern "C" NTSTATUS BCryptFinishHash(BCRYPT_HASH_HANDLE hash, PUCHAR out, ULONG size, ULONG)
{
    auto* h = static_cast<HashObj*>(hash);
    if (size < (h->sha1 ? 20u : 32u)) return NTSTATUS(0xC000000D);
    if (h->sha1) h->a.Final(out); else h->b.Final(out);
    return 0;
}
extern "C" NTSTATUS BCryptDestroyHash(BCRYPT_HASH_HANDLE hash) { delete static_cast<HashObj*>(hash); return 0; }
extern "C" NTSTATUS BCryptHash(BCRYPT_ALG_HANDLE alg, PUCHAR, ULONG, PUCHAR input, ULONG inputSize, PUCHAR out, ULONG outSize)
{
    if (alg == BCRYPT_SHA1_ALG_HANDLE)
    {
        if (outSize < 20) return NTSTATUS(0xC000000D);
        Sha1 s; s.Update(input, inputSize); s.Final(out);
        return 0;
    }
    if (outSize < 32) return NTSTATUS(0xC000000D);
    Ttt2Sha256(input, inputSize, out);
    return 0;
}

// DPAPI: app-private storage is already protected by Android's sandbox, so the "protected" blob is
// the data behind a marker (the Windows build used it for a server token).
static const uint8_t kDpapiMagic[8] = { 'T', 'T', 'T', '2', 'D', 'P', 'A', '1' };
extern "C" BOOL CryptProtectData(DATA_BLOB* in, LPCWSTR, DATA_BLOB*, PVOID, PVOID, DWORD, DATA_BLOB* out)
{
    if (!in || !out) return FALSE;
    out->cbData = in->cbData + 8;
    out->pbData = static_cast<BYTE*>(LocalAlloc(LPTR, out->cbData));
    std::memcpy(out->pbData, kDpapiMagic, 8);
    if (in->cbData) std::memcpy(out->pbData + 8, in->pbData, in->cbData);
    return TRUE;
}
extern "C" BOOL CryptUnprotectData(DATA_BLOB* in, LPWSTR* desc, DATA_BLOB*, PVOID, PVOID, DWORD, DATA_BLOB* out)
{
    if (desc) *desc = nullptr;
    if (!in || !out || in->cbData < 8 || std::memcmp(in->pbData, kDpapiMagic, 8)) { SetLastError(ERROR_INVALID_DATA); return FALSE; }
    out->cbData = in->cbData - 8;
    out->pbData = static_cast<BYTE*>(LocalAlloc(LPTR, out->cbData ? out->cbData : 1));
    if (out->cbData) std::memcpy(out->pbData, in->pbData + 8, out->cbData);
    return TRUE;
}
static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
extern "C" BOOL CryptBinaryToStringA(const BYTE* bin, DWORD size, DWORD flags, LPSTR out, DWORD* outSize)
{
    if (!outSize) return FALSE;
    std::string s;
    if ((flags & 0xFFFF) == CRYPT_STRING_HEXRAW || (flags & 0xFFFF) == CRYPT_STRING_HEX)
    {
        static const char hex[] = "0123456789abcdef";
        for (DWORD i = 0; i < size; ++i) { s += hex[bin[i] >> 4]; s += hex[bin[i] & 15]; }
    }
    else
    {
        for (DWORD i = 0; i < size; i += 3)
        {
            const uint32_t v = uint32_t(bin[i]) << 16 | (i + 1 < size ? uint32_t(bin[i + 1]) << 8 : 0) | (i + 2 < size ? bin[i + 2] : 0);
            s += kB64[(v >> 18) & 63]; s += kB64[(v >> 12) & 63];
            s += i + 1 < size ? kB64[(v >> 6) & 63] : '=';
            s += i + 2 < size ? kB64[v & 63] : '=';
        }
        if (!(flags & CRYPT_STRING_NOCRLF)) s += (flags & CRYPT_STRING_NOCR) ? "\n" : "\r\n";
    }
    if (!out) { *outSize = DWORD(s.size() + 1); return TRUE; }
    if (*outSize <= s.size()) { *outSize = DWORD(s.size() + 1); SetLastError(ERROR_MORE_DATA); return FALSE; }
    std::memcpy(out, s.c_str(), s.size() + 1);
    *outSize = DWORD(s.size());
    return TRUE;
}
extern "C" BOOL CryptBinaryToStringW(const BYTE* bin, DWORD size, DWORD flags, LPWSTR out, DWORD* outSize)
{
    DWORD n = 0;
    CryptBinaryToStringA(bin, size, flags, nullptr, &n);
    std::string s(n, '\0');
    if (!CryptBinaryToStringA(bin, size, flags, s.data(), &n)) return FALSE;
    s.resize(n);
    if (!out) { *outSize = DWORD(s.size() + 1); return TRUE; }
    if (*outSize <= s.size()) { *outSize = DWORD(s.size() + 1); return FALSE; }
    for (size_t i = 0; i <= s.size(); ++i) out[i] = wchar_t((unsigned char)s.c_str()[i]);
    *outSize = DWORD(s.size());
    return TRUE;
}
extern "C" BOOL CryptStringToBinaryA(LPCSTR str, DWORD len, DWORD flags, BYTE* out, DWORD* outSize, DWORD* skip, DWORD* usedFlags)
{
    if (!str || !outSize) return FALSE;
    if (skip) *skip = 0;
    if (usedFlags) *usedFlags = flags;
    const size_t n = len ? len : std::strlen(str);
    std::vector<uint8_t> bytes;
    if ((flags & 0xFFFF) == CRYPT_STRING_HEXRAW || (flags & 0xFFFF) == CRYPT_STRING_HEX)
    {
        int hi = -1;
        for (size_t i = 0; i < n; ++i)
        {
            const char c = str[i]; int v;
            if (c >= '0' && c <= '9') v = c - '0'; else if (c >= 'a' && c <= 'f') v = c - 'a' + 10; else if (c >= 'A' && c <= 'F') v = c - 'A' + 10; else continue;
            if (hi < 0) hi = v; else { bytes.push_back(uint8_t(hi << 4 | v)); hi = -1; }
        }
    }
    else
    {
        uint32_t acc = 0; int bitsIn = 0;
        for (size_t i = 0; i < n; ++i)
        {
            const char c = str[i];
            const char* p = std::strchr(kB64, c);
            if (!p || !c) continue;
            acc = (acc << 6) | uint32_t(p - kB64); bitsIn += 6;
            if (bitsIn >= 8) { bitsIn -= 8; bytes.push_back(uint8_t(acc >> bitsIn)); }
        }
    }
    if (!out) { *outSize = DWORD(bytes.size()); return TRUE; }
    if (*outSize < bytes.size()) { *outSize = DWORD(bytes.size()); SetLastError(ERROR_MORE_DATA); return FALSE; }
    if (!bytes.empty()) std::memcpy(out, bytes.data(), bytes.size());
    *outSize = DWORD(bytes.size());
    return TRUE;
}
extern "C" BOOL CryptStringToBinaryW(LPCWSTR str, DWORD len, DWORD flags, BYTE* out, DWORD* outSize, DWORD* skip, DWORD* usedFlags)
{
    std::string s = Narrow(str, len ? int(len) : -1);
    return CryptStringToBinaryA(s.c_str(), DWORD(s.size()), flags, out, outSize, skip, usedFlags);
}

// ======================================================================================
// Registry: absent
// ======================================================================================
extern "C" LSTATUS RegOpenKeyExW(HKEY, LPCWSTR, DWORD, REGSAM, HKEY* out) { if (out) *out = nullptr; return ERROR_FILE_NOT_FOUND; }
extern "C" LSTATUS RegOpenKeyExA(HKEY, LPCSTR, DWORD, REGSAM, HKEY* out) { if (out) *out = nullptr; return ERROR_FILE_NOT_FOUND; }
extern "C" LSTATUS RegCreateKeyExW(HKEY, LPCWSTR, DWORD, LPWSTR, DWORD, REGSAM, LPSECURITY_ATTRIBUTES, HKEY* out, LPDWORD) { if (out) *out = nullptr; return ERROR_ACCESS_DENIED; }
extern "C" LSTATUS RegQueryValueExW(HKEY, LPCWSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD) { return ERROR_FILE_NOT_FOUND; }
extern "C" LSTATUS RegQueryValueExA(HKEY, LPCSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD) { return ERROR_FILE_NOT_FOUND; }
extern "C" LSTATUS RegSetValueExW(HKEY, LPCWSTR, DWORD, DWORD, const BYTE*, DWORD) { return ERROR_ACCESS_DENIED; }
extern "C" LSTATUS RegGetValueW(HKEY, LPCWSTR, LPCWSTR, DWORD, LPDWORD, PVOID, LPDWORD) { return ERROR_FILE_NOT_FOUND; }
extern "C" LSTATUS RegGetValueA(HKEY, LPCSTR, LPCSTR, DWORD, LPDWORD, PVOID, LPDWORD) { return ERROR_FILE_NOT_FOUND; }
extern "C" LSTATUS RegDeleteValueW(HKEY, LPCWSTR) { return ERROR_FILE_NOT_FOUND; }
extern "C" LSTATUS RegDeleteTreeW(HKEY, LPCWSTR) { return ERROR_FILE_NOT_FOUND; }
extern "C" LSTATUS RegCloseKey(HKEY) { return ERROR_SUCCESS; }

// ======================================================================================
// Debug output
// ======================================================================================
extern "C" BOOL IsDebuggerPresent(void) { return FALSE; }
extern "C" void DebugBreak(void) { raise(SIGTRAP); }
extern "C" void OutputDebugStringA(LPCSTR s)
{
#ifdef __ANDROID__
    __android_log_write(ANDROID_LOG_DEBUG, "TTT2", s ? s : "");
#else
    std::fputs(s ? s : "", stderr);
#endif
}
extern "C" void OutputDebugStringW(LPCWSTR s) { OutputDebugStringA(Narrow(s).c_str()); }
