// Shared internals of the Android Win32 layer.
#pragma once
#include <windows.h>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>
#include <pthread.h>
#include <sys/types.h>

namespace w32 {

enum class Kind : int { Event, Semaphore, Mutex, Thread, Process, File, Find, Mapping, Timer, Snapshot, Pipe, Other };

// Every real HANDLE points at one of these (pseudo handles are small negative values).
struct Object
{
    explicit Object(Kind k) : kind(k) {}
    virtual ~Object() = default;
    const Kind kind;
    std::atomic<int> refs{ 1 };
    std::mutex m;
    std::condition_variable cv;
    // Waitable state (events, semaphores, mutexes, threads, timers)
    virtual bool Signaled() { return false; }     // called with m held
    virtual void Acquire() {}                     // called with m held when a wait succeeds
};

Object* Get(HANDLE h);                // nullptr for invalid/pseudo
HANDLE Wrap(Object* o);               // takes ownership of one reference
void Release(Object* o);
uint32_t ErrnoToWin32(int e);
void SetErrno(int e);                 // SetLastError(ErrnoToWin32(e))

// Text
std::string Narrow(const wchar_t* w, int len = -1);          // UTF-32 wchar -> UTF-8
std::wstring Widen(const char* s, int len = -1);              // UTF-8 -> UTF-32 wchar
std::string HostPath(const char* win, bool mustExist);        // Win32 path -> host path
inline std::string HostPath(const wchar_t* win, bool mustExist) { return HostPath(Narrow(win).c_str(), mustExist); }

// Files (win32_file.cpp)
struct File : Object
{
    File() : Object(Kind::File) {}
    ~File() override;
    int fd = -1;
    bool owns = true;
    bool append = false;
    bool deleteOnClose = false;
    bool isDir = false;
    std::string path;
};
File* GetFile(HANDLE h);

// Threads (win32_core.cpp)
pid_t CurrentTid();

// Time
uint64_t MonotonicNs();
uint64_t UnixNsToFileTime(int64_t ns);

} // namespace w32
