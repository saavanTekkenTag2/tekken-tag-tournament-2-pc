// Android Win32 layer: files, paths, directory search, INI files, file mappings.
//
// Paths: the runtime builds Windows paths ("C:\\...", "<exe folder>\\game\\...", %LOCALAPPDATA%).
// They become host paths under the data root (env TTT2_ROOT, set by the launcher to the app's
// files folder): backslashes turn into slashes, a drive letter maps to the data root, and every
// component is looked up case-insensitively when the exact name does not exist (the game's own
// file names ignore case, the Android file system does not).
#include "win32_internal.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <unordered_map>

#include <dirent.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>

using namespace w32;

// ======================================================================================
// Paths
// ======================================================================================
extern "C" const char* Ttt2AndroidDataRoot(void)
{
    // Read on every call: the Android entry sets TTT2_ROOT after the library is loaded. Returned
    // pointers stay valid (one stored copy per distinct value).
    static std::mutex m;
    static std::vector<std::string*>* seen = new std::vector<std::string*>();
    const char* r = std::getenv("TTT2_ROOT");
    std::string s = (r && *r) ? r : "/data/local/tmp/ttt2";
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    std::lock_guard lk(m);
    for (std::string* p : *seen) if (*p == s) return p->c_str();
    seen->push_back(new std::string(s));
    return seen->back()->c_str();
}

namespace w32 {

static std::string CaseResolve(const std::string& path, bool mustExist)
{
    struct stat st;
    if (path.empty() || lstat(path.c_str(), &st) == 0) return path;
    // Walk the components, fixing case where the exact name is missing.
    std::string out = path[0] == '/' ? "/" : "";
    size_t i = path[0] == '/' ? 1 : 0;
    bool resolved = true;
    while (i <= path.size())
    {
        size_t j = path.find('/', i);
        if (j == std::string::npos) j = path.size();
        const std::string comp = path.substr(i, j - i);
        i = j + 1;
        if (comp.empty() || comp == ".") continue;
        std::string candidate = out + (out.empty() || out.back() == '/' ? "" : "/") + comp;
        if (resolved && lstat(candidate.c_str(), &st) != 0)
        {
            bool found = false;
            if (DIR* d = opendir(out.empty() ? "." : out.c_str()))
            {
                while (dirent* e = readdir(d))
                    if (strcasecmp(e->d_name, comp.c_str()) == 0)
                    {
                        candidate = out + (out.empty() || out.back() == '/' ? "" : "/") + e->d_name;
                        found = true;
                        break;
                    }
                closedir(d);
            }
            if (!found) resolved = false;   // later components are new names: keep as given
        }
        out = candidate;
        if (j == path.size()) break;
    }
    (void)mustExist;
    return out;
}

std::string HostPath(const char* win, bool mustExist)
{
    if (!win) return std::string();
    std::string p(win);
    if (p.rfind("\\\\?\\", 0) == 0 || p.rfind("//?/", 0) == 0) p.erase(0, 4);
    std::replace(p.begin(), p.end(), '\\', '/');
    const std::string root = Ttt2AndroidDataRoot();
    if (p.size() >= 2 && std::isalpha((unsigned char)p[0]) && p[1] == ':')
    {
        // "C:/foo" -> "<root>/foo"; paths that already point into the root keep their tail.
        std::string tail = p.substr(2);
        if (tail.rfind(root, 0) == 0) p = tail;
        else p = root + (tail.empty() || tail[0] != '/' ? "/" : "") + tail;
    }
    // collapse duplicate slashes
    std::string c; c.reserve(p.size());
    for (char ch : p) { if (ch == '/' && !c.empty() && c.back() == '/') continue; c += ch; }
    if (c.size() > 1 && c.back() == '/') c.pop_back();
    return CaseResolve(c, mustExist);
}

File::~File()
{
    if (owns && fd >= 0) close(fd);
    if (deleteOnClose && !path.empty()) { if (isDir) rmdir(path.c_str()); else unlink(path.c_str()); }
}
File* GetFile(HANDLE h)
{
    Object* o = Get(h);
    return o && o->kind == Kind::File ? static_cast<File*>(o) : nullptr;
}

} // namespace w32

extern "C" int Ttt2HostPath(const char* winPath, char* out, size_t outSize, int mustExist)
{
    const std::string p = HostPath(winPath, mustExist != 0);
    if (!out || outSize <= p.size()) return int(p.size() + 1);
    std::memcpy(out, p.c_str(), p.size() + 1);
    return int(p.size());
}

namespace {
uint64_t TsToFt(const timespec& ts) { return UnixNsToFileTime(int64_t(ts.tv_sec) * 1000000000ll + ts.tv_nsec); }
void PutFt(uint64_t v, FILETIME* ft) { if (ft) { ft->dwLowDateTime = DWORD(v); ft->dwHighDateTime = DWORD(v >> 32); } }
DWORD AttrsOf(const struct stat& st, const std::string& name)
{
    DWORD a = 0;
    if (S_ISDIR(st.st_mode)) a |= FILE_ATTRIBUTE_DIRECTORY;
    if (!(st.st_mode & S_IWUSR)) a |= FILE_ATTRIBUTE_READONLY;
    const size_t slash = name.find_last_of('/');
    const char* base = name.c_str() + (slash == std::string::npos ? 0 : slash + 1);
    if (base[0] == '.' && base[1] && !(base[1] == '.' && !base[2])) a |= FILE_ATTRIBUTE_HIDDEN;
    return a ? a : FILE_ATTRIBUTE_NORMAL;
}
// Windows' %LOCALAPPDATA% and friends: the data root, unless already set (the Android entry sets them).
void EnsureEnv()
{
    const std::string root = Ttt2AndroidDataRoot();
    setenv("LOCALAPPDATA", root.c_str(), 0);
    setenv("APPDATA", root.c_str(), 0);
    setenv("USERPROFILE", root.c_str(), 0);
    setenv("TEMP", (root + "/tmp").c_str(), 0);
    setenv("TMP", (root + "/tmp").c_str(), 0);
}

HANDLE OpenHost(const std::string& path, DWORD access, DWORD disposition, DWORD flags)
{
    struct stat st{};
    const bool exists = stat(path.c_str(), &st) == 0;
    if (exists && S_ISDIR(st.st_mode))
    {
        if (disposition == CREATE_NEW) { SetLastError(ERROR_FILE_EXISTS); return INVALID_HANDLE_VALUE; }
        const int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0) { SetErrno(errno); return INVALID_HANDLE_VALUE; }
        auto* f = new File(); f->fd = fd; f->path = path; f->isDir = true;
        f->deleteOnClose = flags & FILE_FLAG_DELETE_ON_CLOSE;
        SetLastError(0);
        return Wrap(f);
    }
    const bool wantWrite = access & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_GENERIC_WRITE);
    const bool wantRead = (access & (GENERIC_READ | GENERIC_ALL | FILE_READ_DATA | FILE_GENERIC_READ)) || !wantWrite;
    int o = O_CLOEXEC;
    o |= wantWrite ? (wantRead ? O_RDWR : O_WRONLY) : O_RDONLY;
    switch (disposition)
    {
    case CREATE_NEW: o |= O_CREAT | O_EXCL; break;
    case CREATE_ALWAYS: o |= O_CREAT | O_TRUNC; break;
    case OPEN_EXISTING: break;
    case OPEN_ALWAYS: o |= O_CREAT; break;
    case TRUNCATE_EXISTING: o |= O_TRUNC; break;
    default: break;
    }
    if ((o & (O_CREAT | O_TRUNC)) && !(o & (O_WRONLY | O_RDWR))) o = (o & ~O_RDONLY) | O_RDWR;
    const int fd = open(path.c_str(), o, 0644);
    if (fd < 0)
    {
        if (errno == EEXIST) SetLastError(ERROR_FILE_EXISTS);
        else if (errno == ENOENT)
        {
            const size_t slash = path.find_last_of('/');
            struct stat ps{};
            SetLastError(slash != std::string::npos && slash > 0 && stat(path.substr(0, slash).c_str(), &ps) != 0 ? ERROR_PATH_NOT_FOUND : ERROR_FILE_NOT_FOUND);
        }
        else SetErrno(errno);
        return INVALID_HANDLE_VALUE;
    }
    auto* f = new File(); f->fd = fd; f->path = path;
    f->append = (access & FILE_APPEND_DATA) && !(access & (GENERIC_WRITE | FILE_WRITE_DATA));
    f->deleteOnClose = flags & FILE_FLAG_DELETE_ON_CLOSE;
    SetLastError(exists && (disposition == CREATE_ALWAYS || disposition == OPEN_ALWAYS) ? ERROR_ALREADY_EXISTS : 0);
    return Wrap(f);
}
} // namespace

// ======================================================================================
// Files
// ======================================================================================
extern "C" HANDLE CreateFileA(LPCSTR name, DWORD access, DWORD, LPSECURITY_ATTRIBUTES, DWORD disposition, DWORD flags, HANDLE)
{
    EnsureEnv();
    if (!name || !*name) { SetLastError(ERROR_PATH_NOT_FOUND); return INVALID_HANDLE_VALUE; }
    if (strcasecmp(name, "NUL") == 0 || strcasecmp(name, "\\\\.\\NUL") == 0) return OpenHost("/dev/null", access, OPEN_EXISTING, 0);
    if (strcasecmp(name, "CONOUT$") == 0) return OpenHost("/dev/stderr", GENERIC_WRITE, OPEN_EXISTING, 0);
    // Files picked with Android's file picker arrive as /proc/self/fd/<n>. Reopening that path
    // goes through the storage FUSE mount, which refuses apps without storage permission, so the
    // already-open descriptor is duplicated instead (reads use pread or their own offset).
    if (std::strncmp(name, "/proc/self/fd/", 14) == 0 && disposition == OPEN_EXISTING)
    {
        char* end = nullptr;
        const long n = std::strtol(name + 14, &end, 10);
        if (end && *end == '\0' && n >= 0)
        {
            const int fd = fcntl(int(n), F_DUPFD_CLOEXEC, 0);
            if (fd < 0) { SetErrno(errno); return INVALID_HANDLE_VALUE; }
            lseek(fd, 0, SEEK_SET);
            auto* f = new File(); f->fd = fd; f->path = name;
            SetLastError(0);
            return Wrap(f);
        }
    }
    return OpenHost(HostPath(name, disposition == OPEN_EXISTING || disposition == TRUNCATE_EXISTING), access, disposition, flags);
}
extern "C" HANDLE CreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition, DWORD flags, HANDLE t)
{
    return CreateFileA(Narrow(name).c_str(), access, share, sa, disposition, flags, t);
}
extern "C" BOOL ReadFile(HANDLE h, LPVOID buffer, DWORD size, LPDWORD read, LPOVERLAPPED ov)
{
    File* f = GetFile(h);
    if (read) *read = 0;
    if (!f) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    size_t done = 0;
    while (done < size)
    {
        const ssize_t r = ov ? pread(f->fd, static_cast<char*>(buffer) + done, size - done, off_t((uint64_t(ov->OffsetHigh) << 32 | ov->Offset) + done))
                             : ::read(f->fd, static_cast<char*>(buffer) + done, size - done);
        if (r < 0) { if (errno == EINTR) continue; SetErrno(errno); return FALSE; }
        if (r == 0) break;
        done += size_t(r);
        if (!ov && f->fd <= 2) break;    // terminals/pipes: return what is there
    }
    if (read) *read = DWORD(done);
    if (ov)
    {
        ov->Internal = 0; ov->InternalHigh = done;
        if (ov->hEvent) SetEvent(ov->hEvent);
        if (done == 0 && size) { SetLastError(ERROR_HANDLE_EOF); return FALSE; }
    }
    return TRUE;
}
extern "C" BOOL WriteFile(HANDLE h, LPCVOID buffer, DWORD size, LPDWORD written, LPOVERLAPPED ov)
{
    File* f = GetFile(h);
    if (written) *written = 0;
    if (!f) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    size_t done = 0;
    while (done < size)
    {
        ssize_t r;
        if (ov && !f->append) r = pwrite(f->fd, static_cast<const char*>(buffer) + done, size - done, off_t((uint64_t(ov->OffsetHigh) << 32 | ov->Offset) + done));
        else
        {
            if (f->append) lseek(f->fd, 0, SEEK_END);
            r = ::write(f->fd, static_cast<const char*>(buffer) + done, size - done);
        }
        if (r < 0) { if (errno == EINTR) continue; SetErrno(errno); return FALSE; }
        done += size_t(r);
    }
    if (written) *written = DWORD(done);
    if (ov) { ov->Internal = 0; ov->InternalHigh = done; if (ov->hEvent) SetEvent(ov->hEvent); }
    return TRUE;
}
extern "C" BOOL ReadFileEx(HANDLE h, LPVOID buffer, DWORD size, LPOVERLAPPED ov, LPOVERLAPPED_COMPLETION_ROUTINE done)
{
    DWORD n = 0;
    const BOOL ok = ReadFile(h, buffer, size, &n, ov);
    if (done) done(ok ? 0 : GetLastError(), n, ov);
    return ok;
}
extern "C" BOOL GetOverlappedResult(HANDLE, LPOVERLAPPED ov, LPDWORD transferred, BOOL)
{
    if (!ov) return FALSE;
    if (transferred) *transferred = DWORD(ov->InternalHigh);
    return ov->Internal == 0;
}
extern "C" BOOL CancelIo(HANDLE) { return TRUE; }
extern "C" BOOL CancelIoEx(HANDLE, LPOVERLAPPED) { return TRUE; }
extern "C" BOOL SetFilePointerEx(HANDLE h, LARGE_INTEGER distance, PLARGE_INTEGER newPos, DWORD method)
{
    File* f = GetFile(h);
    if (!f) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    const int whence = method == FILE_BEGIN ? SEEK_SET : method == FILE_CURRENT ? SEEK_CUR : SEEK_END;
    const off_t r = lseek(f->fd, off_t(distance.QuadPart), whence);
    if (r < 0) { SetLastError(errno == EINVAL ? ERROR_NEGATIVE_SEEK : ErrnoToWin32(errno)); return FALSE; }
    if (newPos) newPos->QuadPart = r;
    return TRUE;
}
extern "C" DWORD SetFilePointer(HANDLE h, LONG distance, PLONG distanceHigh, DWORD method)
{
    LARGE_INTEGER d, n;
    d.QuadPart = distanceHigh ? (LONGLONG(*distanceHigh) << 32) | DWORD(distance) : LONGLONG(distance);
    if (!SetFilePointerEx(h, d, &n, method)) return INVALID_SET_FILE_POINTER;
    if (distanceHigh) *distanceHigh = LONG(n.QuadPart >> 32);
    SetLastError(0);
    return DWORD(n.QuadPart);
}
extern "C" BOOL GetFileSizeEx(HANDLE h, PLARGE_INTEGER size)
{
    File* f = GetFile(h);
    struct stat st{};
    if (!f || fstat(f->fd, &st) != 0) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    size->QuadPart = st.st_size;
    return TRUE;
}
extern "C" DWORD GetFileSize(HANDLE h, LPDWORD high)
{
    LARGE_INTEGER s;
    if (!GetFileSizeEx(h, &s)) return INVALID_FILE_SIZE;
    if (high) *high = DWORD(s.QuadPart >> 32);
    return DWORD(s.QuadPart);
}
extern "C" BOOL SetEndOfFile(HANDLE h)
{
    File* f = GetFile(h);
    if (!f) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    const off_t pos = lseek(f->fd, 0, SEEK_CUR);
    if (pos < 0 || ftruncate(f->fd, pos) != 0) { SetErrno(errno); return FALSE; }
    return TRUE;
}
extern "C" BOOL FlushFileBuffers(HANDLE h)
{
    File* f = GetFile(h);
    if (!f) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    if (fsync(f->fd) != 0 && errno != EINVAL && errno != EROFS) { SetErrno(errno); return FALSE; }
    return TRUE;
}
extern "C" DWORD GetFileType(HANDLE h)
{
    File* f = GetFile(h);
    struct stat st{};
    if (!f || fstat(f->fd, &st) != 0) { SetLastError(ERROR_INVALID_HANDLE); return FILE_TYPE_UNKNOWN; }
    if (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode)) return FILE_TYPE_DISK;
    if (S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode)) return FILE_TYPE_PIPE;
    if (S_ISCHR(st.st_mode)) return FILE_TYPE_CHAR;
    return FILE_TYPE_UNKNOWN;
}
extern "C" BOOL GetFileTime(HANDLE h, LPFILETIME created, LPFILETIME accessed, LPFILETIME written)
{
    File* f = GetFile(h);
    struct stat st{};
    if (!f || fstat(f->fd, &st) != 0) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    PutFt(TsToFt(st.st_ctim), created); PutFt(TsToFt(st.st_atim), accessed); PutFt(TsToFt(st.st_mtim), written);
    return TRUE;
}
extern "C" BOOL SetFileTime(HANDLE h, const FILETIME* created, const FILETIME* accessed, const FILETIME* written)
{
    File* f = GetFile(h);
    if (!f) return FALSE;
    (void)created;
    auto conv = [](const FILETIME* ft) {
        timespec ts{ 0, UTIME_OMIT };
        if (ft) { const uint64_t v = (uint64_t(ft->dwHighDateTime) << 32 | ft->dwLowDateTime) - 116444736000000000ull; ts.tv_sec = time_t(v / 10000000ull); ts.tv_nsec = long(v % 10000000ull) * 100; }
        return ts;
    };
    timespec t[2] = { conv(accessed), conv(written) };
    return futimens(f->fd, t) == 0;
}
extern "C" BOOL GetFileInformationByHandle(HANDLE h, LPBY_HANDLE_FILE_INFORMATION info)
{
    File* f = GetFile(h);
    struct stat st{};
    if (!f || !info || fstat(f->fd, &st) != 0) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    std::memset(info, 0, sizeof(*info));
    info->dwFileAttributes = AttrsOf(st, f->path);
    PutFt(TsToFt(st.st_ctim), &info->ftCreationTime); PutFt(TsToFt(st.st_atim), &info->ftLastAccessTime); PutFt(TsToFt(st.st_mtim), &info->ftLastWriteTime);
    info->dwVolumeSerialNumber = DWORD(st.st_dev);
    info->nFileSizeHigh = DWORD(uint64_t(st.st_size) >> 32); info->nFileSizeLow = DWORD(st.st_size);
    info->nNumberOfLinks = DWORD(st.st_nlink);
    info->nFileIndexHigh = DWORD(uint64_t(st.st_ino) >> 32); info->nFileIndexLow = DWORD(st.st_ino);
    return TRUE;
}
extern "C" BOOL GetFileInformationByHandleEx(HANDLE h, FILE_INFO_BY_HANDLE_CLASS cls, LPVOID info, DWORD size)
{
    File* f = GetFile(h);
    struct stat st{};
    if (!f || !info || fstat(f->fd, &st) != 0) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    if (cls == FileStandardInfo && size >= sizeof(FILE_STANDARD_INFO))
    {
        auto* s = static_cast<FILE_STANDARD_INFO*>(info);
        s->AllocationSize.QuadPart = st.st_blocks * 512; s->EndOfFile.QuadPart = st.st_size;
        s->NumberOfLinks = DWORD(st.st_nlink); s->DeletePending = f->deleteOnClose; s->Directory = S_ISDIR(st.st_mode);
        return TRUE;
    }
    if (cls == FileBasicInfo && size >= sizeof(FILE_BASIC_INFO))
    {
        auto* b = static_cast<FILE_BASIC_INFO*>(info);
        b->CreationTime.QuadPart = LONGLONG(TsToFt(st.st_ctim)); b->LastAccessTime.QuadPart = LONGLONG(TsToFt(st.st_atim));
        b->LastWriteTime.QuadPart = LONGLONG(TsToFt(st.st_mtim)); b->ChangeTime.QuadPart = b->LastWriteTime.QuadPart;
        b->FileAttributes = AttrsOf(st, f->path);
        return TRUE;
    }
    if (cls == FileIdInfo && size >= sizeof(FILE_ID_INFO))
    {
        auto* i = static_cast<FILE_ID_INFO*>(info);
        std::memset(i, 0, sizeof(*i)); i->VolumeSerialNumber = st.st_dev; std::memcpy(i->FileId, &st.st_ino, sizeof(st.st_ino));
        return TRUE;
    }
    SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}
extern "C" BOOL SetFileInformationByHandle(HANDLE h, FILE_INFO_BY_HANDLE_CLASS cls, LPVOID info, DWORD size)
{
    File* f = GetFile(h);
    if (!f || !info) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    switch (cls)
    {
    case FileRenameInfo: case FileRenameInfoEx:
    {
        auto* r = static_cast<FILE_RENAME_INFO*>(info);
        const std::wstring name(r->FileName, r->FileNameLength / sizeof(wchar_t));
        const std::string to = HostPath(name.c_str(), false);
        const bool replace = cls == FileRenameInfoEx ? (r->Flags & FILE_RENAME_FLAG_REPLACE_IF_EXISTS) : r->ReplaceIfExists;
        struct stat st{};
        if (!replace && stat(to.c_str(), &st) == 0) { SetLastError(ERROR_ALREADY_EXISTS); return FALSE; }
        if (rename(f->path.c_str(), to.c_str()) != 0) { SetErrno(errno); return FALSE; }
        f->path = to;
        return TRUE;
    }
    case FileDispositionInfo:
        f->deleteOnClose = static_cast<FILE_DISPOSITION_INFO*>(info)->DeleteFile != 0;
        return TRUE;
    case FileDispositionInfoEx:
    {
        const DWORD fl = static_cast<FILE_DISPOSITION_INFO_EX*>(info)->Flags;
        if ((fl & FILE_DISPOSITION_FLAG_DELETE) && (fl & FILE_DISPOSITION_FLAG_POSIX_SEMANTICS))
        {
            if ((f->isDir ? rmdir(f->path.c_str()) : unlink(f->path.c_str())) != 0) { SetErrno(errno); return FALSE; }
            return TRUE;
        }
        f->deleteOnClose = (fl & FILE_DISPOSITION_FLAG_DELETE) != 0;
        return TRUE;
    }
    case FileEndOfFileInfo: case FileAllocationInfo:
        if (ftruncate(f->fd, off_t(static_cast<FILE_END_OF_FILE_INFO*>(info)->EndOfFile.QuadPart)) != 0) { SetErrno(errno); return FALSE; }
        return TRUE;
    case FileBasicInfo:
        return TRUE;
    default:
        (void)size;
        SetLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
    }
}
extern "C" BOOL LockFileEx(HANDLE h, DWORD flags, DWORD, DWORD, DWORD, LPOVERLAPPED)
{
    File* f = GetFile(h);
    if (!f) return FALSE;
    const int op = ((flags & LOCKFILE_EXCLUSIVE_LOCK) ? LOCK_EX : LOCK_SH) | ((flags & LOCKFILE_FAIL_IMMEDIATELY) ? LOCK_NB : 0);
    if (flock(f->fd, op) != 0) { SetLastError(ERROR_LOCK_VIOLATION); return FALSE; }
    return TRUE;
}
extern "C" BOOL UnlockFileEx(HANDLE h, DWORD, DWORD, DWORD, LPOVERLAPPED)
{
    File* f = GetFile(h);
    return f && flock(f->fd, LOCK_UN) == 0;
}
extern "C" DWORD GetFinalPathNameByHandleA(HANDLE h, LPSTR path, DWORD size, DWORD)
{
    File* f = GetFile(h);
    if (!f) { SetLastError(ERROR_INVALID_HANDLE); return 0; }
    char real[PATH_MAX];
    const std::string p = realpath(f->path.c_str(), real) ? std::string(real) : f->path;
    if (!path || size <= p.size()) return DWORD(p.size() + 1);
    std::memcpy(path, p.c_str(), p.size() + 1);
    return DWORD(p.size());
}
extern "C" DWORD GetFinalPathNameByHandleW(HANDLE h, LPWSTR path, DWORD size, DWORD flags)
{
    char buf[PATH_MAX + 1];
    const DWORD n = GetFinalPathNameByHandleA(h, buf, sizeof(buf), flags);
    if (!n || n >= sizeof(buf)) return 0;
    const std::wstring w = Widen(buf);
    if (!path || size <= w.size()) return DWORD(w.size() + 1);
    std::wmemcpy(path, w.c_str(), w.size() + 1);
    return DWORD(w.size());
}

// ---- attributes / directories / delete / move ----
extern "C" DWORD GetFileAttributesA(LPCSTR name)
{
    const std::string p = HostPath(name, true);
    struct stat st{};
    if (stat(p.c_str(), &st) != 0) { SetErrno(errno); return INVALID_FILE_ATTRIBUTES; }
    return AttrsOf(st, p);
}
extern "C" DWORD GetFileAttributesW(LPCWSTR name) { return GetFileAttributesA(Narrow(name).c_str()); }
extern "C" BOOL GetFileAttributesExA(LPCSTR name, GET_FILEEX_INFO_LEVELS, LPVOID info)
{
    const std::string p = HostPath(name, true);
    struct stat st{};
    if (stat(p.c_str(), &st) != 0) { SetErrno(errno); return FALSE; }
    auto* d = static_cast<WIN32_FILE_ATTRIBUTE_DATA*>(info);
    d->dwFileAttributes = AttrsOf(st, p);
    PutFt(TsToFt(st.st_ctim), &d->ftCreationTime); PutFt(TsToFt(st.st_atim), &d->ftLastAccessTime); PutFt(TsToFt(st.st_mtim), &d->ftLastWriteTime);
    d->nFileSizeHigh = S_ISDIR(st.st_mode) ? 0 : DWORD(uint64_t(st.st_size) >> 32);
    d->nFileSizeLow = S_ISDIR(st.st_mode) ? 0 : DWORD(st.st_size);
    return TRUE;
}
extern "C" BOOL GetFileAttributesExW(LPCWSTR name, GET_FILEEX_INFO_LEVELS l, LPVOID info) { return GetFileAttributesExA(Narrow(name).c_str(), l, info); }
extern "C" BOOL SetFileAttributesA(LPCSTR name, DWORD attrs)
{
    const std::string p = HostPath(name, true);
    struct stat st{};
    if (stat(p.c_str(), &st) != 0) { SetErrno(errno); return FALSE; }
    mode_t m = st.st_mode;
    if (attrs & FILE_ATTRIBUTE_READONLY) m &= ~mode_t(S_IWUSR | S_IWGRP | S_IWOTH); else m |= S_IWUSR;
    return chmod(p.c_str(), m) == 0;
}
extern "C" BOOL SetFileAttributesW(LPCWSTR name, DWORD attrs) { return SetFileAttributesA(Narrow(name).c_str(), attrs); }
extern "C" BOOL CreateDirectoryA(LPCSTR name, LPSECURITY_ATTRIBUTES)
{
    const std::string p = HostPath(name, false);
    if (mkdir(p.c_str(), 0755) != 0) { SetErrno(errno); return FALSE; }
    return TRUE;
}
extern "C" BOOL CreateDirectoryW(LPCWSTR name, LPSECURITY_ATTRIBUTES sa) { return CreateDirectoryA(Narrow(name).c_str(), sa); }
extern "C" BOOL RemoveDirectoryA(LPCSTR name) { if (rmdir(HostPath(name, true).c_str()) != 0) { SetErrno(errno); return FALSE; } return TRUE; }
extern "C" BOOL RemoveDirectoryW(LPCWSTR name) { return RemoveDirectoryA(Narrow(name).c_str()); }
extern "C" BOOL DeleteFileA(LPCSTR name) { if (unlink(HostPath(name, true).c_str()) != 0) { SetErrno(errno); return FALSE; } return TRUE; }
extern "C" BOOL DeleteFileW(LPCWSTR name) { return DeleteFileA(Narrow(name).c_str()); }
static bool CopyHost(const std::string& from, const std::string& to)
{
    std::ifstream in(from, std::ios::binary);
    if (!in) return false;
    std::ofstream out(to, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << in.rdbuf();
    return bool(out);
}
extern "C" BOOL MoveFileExA(LPCSTR from, LPCSTR to, DWORD flags)
{
    const std::string a = HostPath(from, true), b = HostPath(to, false);
    struct stat st{};
    if (!(flags & MOVEFILE_REPLACE_EXISTING) && stat(b.c_str(), &st) == 0) { SetLastError(ERROR_ALREADY_EXISTS); return FALSE; }
    if (rename(a.c_str(), b.c_str()) == 0) { if (flags & MOVEFILE_WRITE_THROUGH) sync(); return TRUE; }
    if (errno == EXDEV && (flags & MOVEFILE_COPY_ALLOWED) && CopyHost(a, b)) { unlink(a.c_str()); return TRUE; }
    SetErrno(errno);
    return FALSE;
}
extern "C" BOOL MoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags) { return MoveFileExA(Narrow(from).c_str(), Narrow(to).c_str(), flags); }
extern "C" BOOL MoveFileA(LPCSTR from, LPCSTR to) { return MoveFileExA(from, to, MOVEFILE_COPY_ALLOWED); }
extern "C" BOOL MoveFileW(LPCWSTR from, LPCWSTR to) { return MoveFileExW(from, to, MOVEFILE_COPY_ALLOWED); }
extern "C" BOOL CopyFileA(LPCSTR from, LPCSTR to, BOOL failIfExists)
{
    const std::string a = HostPath(from, true), b = HostPath(to, false);
    struct stat st{};
    if (failIfExists && stat(b.c_str(), &st) == 0) { SetLastError(ERROR_FILE_EXISTS); return FALSE; }
    if (!CopyHost(a, b)) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    return TRUE;
}
extern "C" BOOL CopyFileW(LPCWSTR from, LPCWSTR to, BOOL failIfExists) { return CopyFileA(Narrow(from).c_str(), Narrow(to).c_str(), failIfExists); }
extern "C" BOOL ReplaceFileW(LPCWSTR replaced, LPCWSTR replacement, LPCWSTR backup, DWORD, LPVOID, LPVOID)
{
    const std::string a = HostPath(replaced, true), b = HostPath(replacement, true);
    if (backup) CopyHost(a, HostPath(backup, false));
    if (rename(b.c_str(), a.c_str()) != 0) { SetErrno(errno); return FALSE; }
    return TRUE;
}

// ---- directory search ----
namespace {
struct Find : Object
{
    Find() : Object(Kind::Find) {}
    std::string dir;
    std::vector<std::string> names;
    size_t at = 0;
    bool dirsOnly = false;
};
bool FillFind(Find* f, WIN32_FIND_DATAW* w, WIN32_FIND_DATAA* a)
{
    while (f->at < f->names.size())
    {
        const std::string& n = f->names[f->at++];
        const std::string full = f->dir + "/" + n;
        struct stat st{};
        if (stat(full.c_str(), &st) != 0) continue;
        if (f->dirsOnly && !S_ISDIR(st.st_mode)) continue;
        const DWORD attrs = AttrsOf(st, full);
        const uint64_t size = S_ISDIR(st.st_mode) ? 0 : uint64_t(st.st_size);
        if (w)
        {
            std::memset(w, 0, sizeof(*w));
            w->dwFileAttributes = attrs;
            PutFt(TsToFt(st.st_ctim), &w->ftCreationTime); PutFt(TsToFt(st.st_atim), &w->ftLastAccessTime); PutFt(TsToFt(st.st_mtim), &w->ftLastWriteTime);
            w->nFileSizeHigh = DWORD(size >> 32); w->nFileSizeLow = DWORD(size);
            const std::wstring wn = Widen(n.c_str());
            wcsncpy_s(w->cFileName, MAX_PATH, wn.c_str(), MAX_PATH - 1);
        }
        if (a)
        {
            std::memset(a, 0, sizeof(*a));
            a->dwFileAttributes = attrs;
            PutFt(TsToFt(st.st_ctim), &a->ftCreationTime); PutFt(TsToFt(st.st_atim), &a->ftLastAccessTime); PutFt(TsToFt(st.st_mtim), &a->ftLastWriteTime);
            a->nFileSizeHigh = DWORD(size >> 32); a->nFileSizeLow = DWORD(size);
            strncpy_s(a->cFileName, MAX_PATH, n.c_str(), MAX_PATH - 1);
        }
        return true;
    }
    SetLastError(ERROR_NO_MORE_FILES);
    return false;
}
HANDLE FindStart(const char* patternWin, FINDEX_SEARCH_OPS op, WIN32_FIND_DATAW* w, WIN32_FIND_DATAA* a)
{
    std::string pat(patternWin ? patternWin : "");
    std::replace(pat.begin(), pat.end(), '\\', '/');
    const size_t slash = pat.find_last_of('/');
    const std::string dirWin = slash == std::string::npos ? std::string(".") : pat.substr(0, slash);
    const std::string mask = slash == std::string::npos ? pat : pat.substr(slash + 1);
    auto* f = new Find();
    f->dir = HostPath(dirWin.empty() ? "/" : dirWin.c_str(), true);
    f->dirsOnly = op == FindExSearchLimitToDirectories;
    DIR* d = opendir(f->dir.c_str());
    if (!d) { SetErrno(errno == ENOENT ? ENOTDIR : errno); delete f; return INVALID_HANDLE_VALUE; }
    const bool all = mask == "*" || mask == "*.*" || mask.empty();
    while (dirent* e = readdir(d))
        if (all || fnmatch(mask.c_str(), e->d_name, FNM_CASEFOLD) == 0) f->names.emplace_back(e->d_name);
    closedir(d);
    std::sort(f->names.begin(), f->names.end());
    if (!FillFind(f, w, a)) { delete f; SetLastError(ERROR_FILE_NOT_FOUND); return INVALID_HANDLE_VALUE; }
    return Wrap(f);
}
Find* GetFind(HANDLE h) { Object* o = Get(h); return o && o->kind == Kind::Find ? static_cast<Find*>(o) : nullptr; }
} // namespace

extern "C" HANDLE FindFirstFileA(LPCSTR pattern, LPWIN32_FIND_DATAA data) { return FindStart(pattern, FindExSearchNameMatch, nullptr, data); }
extern "C" HANDLE FindFirstFileW(LPCWSTR pattern, LPWIN32_FIND_DATAW data) { return FindStart(Narrow(pattern).c_str(), FindExSearchNameMatch, data, nullptr); }
extern "C" HANDLE FindFirstFileExW(LPCWSTR pattern, FINDEX_INFO_LEVELS, LPVOID data, FINDEX_SEARCH_OPS op, LPVOID, DWORD)
{ return FindStart(Narrow(pattern).c_str(), op, static_cast<WIN32_FIND_DATAW*>(data), nullptr); }
extern "C" HANDLE FindFirstFileExA(LPCSTR pattern, FINDEX_INFO_LEVELS, LPVOID data, FINDEX_SEARCH_OPS op, LPVOID, DWORD)
{ return FindStart(pattern, op, nullptr, static_cast<WIN32_FIND_DATAA*>(data)); }
extern "C" HANDLE Ttt2FindFirstFileExUtf8W(LPCSTR pattern, FINDEX_INFO_LEVELS, LPVOID data, FINDEX_SEARCH_OPS op, LPVOID, DWORD)
{ return FindStart(pattern, op, static_cast<WIN32_FIND_DATAW*>(data), nullptr); }
extern "C" BOOL FindNextFileW(HANDLE h, LPWIN32_FIND_DATAW data) { Find* f = GetFind(h); return f && FillFind(f, data, nullptr); }
extern "C" BOOL FindNextFileA(HANDLE h, LPWIN32_FIND_DATAA data) { Find* f = GetFind(h); return f && FillFind(f, nullptr, data); }
extern "C" BOOL FindClose(HANDLE h) { Find* f = GetFind(h); if (!f) return FALSE; Release(f); return TRUE; }

// ---- path helpers ----
extern "C" DWORD GetFullPathNameA(LPCSTR name, DWORD size, LPSTR buffer, LPSTR* filePart)
{
    std::string p = HostPath(name, false);
    if (p.empty() || p[0] != '/')
    {
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd))) p = std::string(cwd) + "/" + p;
    }
    if (!buffer || size <= p.size()) return DWORD(p.size() + 1);
    std::memcpy(buffer, p.c_str(), p.size() + 1);
    if (filePart) { char* s = std::strrchr(buffer, '/'); *filePart = s ? s + 1 : buffer; }
    return DWORD(p.size());
}
extern "C" DWORD GetFullPathNameW(LPCWSTR name, DWORD size, LPWSTR buffer, LPWSTR* filePart)
{
    char b[PATH_MAX + 1];
    const DWORD n = GetFullPathNameA(Narrow(name).c_str(), sizeof(b), b, nullptr);
    if (!n || n >= sizeof(b)) return 0;
    const std::wstring w = Widen(b);
    if (!buffer || size <= w.size()) return DWORD(w.size() + 1);
    std::wmemcpy(buffer, w.c_str(), w.size() + 1);
    if (filePart) { wchar_t* s = std::wcsrchr(buffer, L'/'); *filePart = s ? s + 1 : buffer; }
    return DWORD(w.size());
}
extern "C" DWORD GetCurrentDirectoryA(DWORD size, LPSTR buffer)
{
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) return 0;
    const size_t n = std::strlen(cwd);
    if (!buffer || size <= n) return DWORD(n + 1);
    std::memcpy(buffer, cwd, n + 1);
    return DWORD(n);
}
extern "C" DWORD GetCurrentDirectoryW(DWORD size, LPWSTR buffer)
{
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) return 0;
    const std::wstring w = Widen(cwd);
    if (!buffer || size <= w.size()) return DWORD(w.size() + 1);
    std::wmemcpy(buffer, w.c_str(), w.size() + 1);
    return DWORD(w.size());
}
extern "C" BOOL SetCurrentDirectoryA(LPCSTR dir) { if (chdir(HostPath(dir, true).c_str()) != 0) { SetErrno(errno); return FALSE; } return TRUE; }
extern "C" BOOL SetCurrentDirectoryW(LPCWSTR dir) { return SetCurrentDirectoryA(Narrow(dir).c_str()); }
extern "C" DWORD GetTempPathA(DWORD size, LPSTR buffer)
{
    EnsureEnv();
    const std::string t = std::string(Ttt2AndroidDataRoot()) + "/tmp/";
    if (!buffer || size <= t.size()) return DWORD(t.size() + 1);
    std::memcpy(buffer, t.c_str(), t.size() + 1);
    return DWORD(t.size());
}
extern "C" DWORD GetTempPathW(DWORD size, LPWSTR buffer)
{
    char b[PATH_MAX];
    GetTempPathA(sizeof(b), b);
    const std::wstring w = Widen(b);
    if (!buffer || size <= w.size()) return DWORD(w.size() + 1);
    std::wmemcpy(buffer, w.c_str(), w.size() + 1);
    return DWORD(w.size());
}
extern "C" UINT GetTempFileNameW(LPCWSTR dir, LPCWSTR prefix, UINT unique, LPWSTR name)
{
    static std::atomic<unsigned> counter{ 1 };
    const unsigned u = unique ? unique : (unsigned(getpid()) << 8) + counter++;
    std::string p = Narrow(dir) + "/" + Narrow(prefix).substr(0, 3);
    char num[16]; std::snprintf(num, sizeof(num), "%X.tmp", u & 0xFFFF);
    p += num;
    const std::wstring w = Widen(p.c_str());
    std::wmemcpy(name, w.c_str(), std::min<size_t>(w.size() + 1, MAX_PATH));
    if (!unique) { const std::string h = HostPath(p.c_str(), false); const int fd = open(h.c_str(), O_CREAT | O_WRONLY, 0644); if (fd >= 0) close(fd); }
    return u & 0xFFFF;
}
extern "C" BOOL GetDiskFreeSpaceExA(LPCSTR dir, PULARGE_INTEGER freeToCaller, PULARGE_INTEGER total, PULARGE_INTEGER totalFree)
{
    struct statvfs s{};
    const std::string p = HostPath(dir ? dir : Ttt2AndroidDataRoot(), true);
    if (statvfs(p.c_str(), &s) != 0) { SetErrno(errno); return FALSE; }
    if (freeToCaller) freeToCaller->QuadPart = uint64_t(s.f_bavail) * s.f_frsize;
    if (total) total->QuadPart = uint64_t(s.f_blocks) * s.f_frsize;
    if (totalFree) totalFree->QuadPart = uint64_t(s.f_bfree) * s.f_frsize;
    return TRUE;
}
extern "C" BOOL GetDiskFreeSpaceExW(LPCWSTR dir, PULARGE_INTEGER a, PULARGE_INTEGER b, PULARGE_INTEGER c) { return GetDiskFreeSpaceExA(dir ? Narrow(dir).c_str() : nullptr, a, b, c); }
extern "C" BOOL GetVolumeInformationW(LPCWSTR, LPWSTR name, DWORD nameSize, LPDWORD serial, LPDWORD maxComp, LPDWORD flags, LPWSTR fs, DWORD fsSize)
{
    if (name && nameSize) name[0] = 0;
    if (serial) *serial = 0x7772;
    if (maxComp) *maxComp = 255;
    if (flags) *flags = 0;
    if (fs && fsSize > 4) std::wcscpy(fs, L"ext4");
    return TRUE;
}
extern "C" UINT GetDriveTypeW(LPCWSTR) { return 3; /* DRIVE_FIXED */ }
extern "C" BOOL PathFileExistsA(LPCSTR path) { struct stat st{}; return stat(HostPath(path, true).c_str(), &st) == 0; }
extern "C" BOOL PathFileExistsW(LPCWSTR path) { return PathFileExistsA(Narrow(path).c_str()); }
extern "C" int SHCreateDirectoryExA(HWND, LPCSTR path, const SECURITY_ATTRIBUTES*)
{
    std::string p = HostPath(path, false);
    for (size_t i = 1; i <= p.size(); ++i)
        if (i == p.size() || p[i] == '/') { const std::string sub = p.substr(0, i); mkdir(sub.c_str(), 0755); }
    struct stat st{};
    return stat(p.c_str(), &st) == 0 ? 0 : int(ERROR_PATH_NOT_FOUND);
}
extern "C" int SHCreateDirectoryExW(HWND w, LPCWSTR path, const SECURITY_ATTRIBUTES* sa) { return SHCreateDirectoryExA(w, Narrow(path).c_str(), sa); }
extern "C" HRESULT SHGetKnownFolderPath(REFKNOWNFOLDERID, DWORD, HANDLE, PWSTR* path)
{
    EnsureEnv();
    const std::wstring w = Widen(Ttt2AndroidDataRoot());
    *path = static_cast<PWSTR>(CoTaskMemAlloc((w.size() + 1) * sizeof(wchar_t)));
    std::wmemcpy(*path, w.c_str(), w.size() + 1);
    return S_OK;
}
extern "C" HRESULT SHGetFolderPathW(HWND, int, HANDLE, DWORD, LPWSTR path)
{
    const std::wstring w = Widen(Ttt2AndroidDataRoot());
    wcsncpy_s(path, MAX_PATH, w.c_str(), MAX_PATH - 1);
    return S_OK;
}
extern "C" HRESULT SHGetFolderPathA(HWND, int, HANDLE, DWORD, LPSTR path)
{
    strncpy_s(path, MAX_PATH, Ttt2AndroidDataRoot(), MAX_PATH - 1);
    return S_OK;
}
extern "C" HINSTANCE ShellExecuteW(HWND, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, INT) { return reinterpret_cast<HINSTANCE>(uintptr_t(2)); }
extern "C" HINSTANCE ShellExecuteA(HWND, LPCSTR, LPCSTR, LPCSTR, LPCSTR, INT) { return reinterpret_cast<HINSTANCE>(uintptr_t(2)); }

// ---- standard handles / CRT fds ----
namespace {
std::mutex g_fdMutex;
std::unordered_map<int, File*>& FdHandles() { static auto* m = new std::unordered_map<int, File*>(); return *m; }
HANDLE HandleForFd(int fd)
{
    std::lock_guard lk(g_fdMutex);
    File*& f = FdHandles()[fd];
    if (!f) { f = new File(); f->fd = fd; f->owns = false; f->refs = 1 << 20; Wrap(f); }
    return reinterpret_cast<HANDLE>(f);
}
} // namespace
extern "C" intptr_t _get_osfhandle(int fd)
{
    if (fd < 0) return intptr_t(-1);
    return reinterpret_cast<intptr_t>(HandleForFd(fd));
}
extern "C" int _open_osfhandle(intptr_t handle, int)
{
    File* f = GetFile(reinterpret_cast<HANDLE>(handle));
    if (!f) return -1;
    if (!f->owns) return f->fd;
    f->owns = false;      // the fd now belongs to the CRT
    return f->fd;
}
extern "C" HANDLE GetStdHandle(DWORD which)
{
    switch (which)
    {
    case STD_INPUT_HANDLE: return HandleForFd(0);
    case STD_OUTPUT_HANDLE: return HandleForFd(1);
    case STD_ERROR_HANDLE: return HandleForFd(2);
    default: return INVALID_HANDLE_VALUE;
    }
}
extern "C" BOOL SetStdHandle(DWORD which, HANDLE h)
{
    File* f = GetFile(h);
    const int target = which == STD_INPUT_HANDLE ? 0 : which == STD_OUTPUT_HANDLE ? 1 : which == STD_ERROR_HANDLE ? 2 : -1;
    if (!f || target < 0) return FALSE;
    return dup2(f->fd, target) >= 0;
}
extern "C" BOOL AllocConsole(void) { return FALSE; }
extern "C" BOOL AttachConsole(DWORD) { return FALSE; }
extern "C" BOOL FreeConsole(void) { return TRUE; }
extern "C" HWND GetConsoleWindow(void) { return nullptr; }
extern "C" BOOL SetConsoleOutputCP(UINT) { return TRUE; }
extern "C" BOOL SetConsoleTitleW(LPCWSTR) { return TRUE; }
extern "C" BOOL SetConsoleTitleA(LPCSTR) { return TRUE; }
extern "C" BOOL WriteConsoleW(HANDLE h, const void* buffer, DWORD chars, LPDWORD written, LPVOID)
{
    const std::string s = Narrow(static_cast<const wchar_t*>(buffer), int(chars));
    DWORD n = 0;
    const BOOL ok = WriteFile(h, s.data(), DWORD(s.size()), &n, nullptr);
    if (written) *written = chars;
    return ok;
}
extern "C" BOOL WriteConsoleA(HANDLE h, const void* buffer, DWORD chars, LPDWORD written, LPVOID) { return WriteFile(h, buffer, chars, written, nullptr); }
extern "C" BOOL GetConsoleMode(HANDLE, LPDWORD mode) { if (mode) *mode = 0; return FALSE; }
extern "C" BOOL SetConsoleMode(HANDLE, DWORD) { return FALSE; }
extern "C" BOOL SetConsoleCtrlHandler(PHANDLER_ROUTINE, BOOL) { return TRUE; }
extern "C" BOOL CreatePipe(PHANDLE read, PHANDLE write, LPSECURITY_ATTRIBUTES, DWORD)
{
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) { SetErrno(errno); return FALSE; }
    auto* r = new File(); r->fd = fds[0];
    auto* w = new File(); w->fd = fds[1];
    *read = Wrap(r); *write = Wrap(w);
    return TRUE;
}
extern "C" BOOL PeekNamedPipe(HANDLE h, LPVOID, DWORD, LPDWORD read, LPDWORD avail, LPDWORD left)
{
    File* f = GetFile(h);
    if (!f) return FALSE;
    int n = 0;
    if (ioctl(f->fd, FIONREAD, &n) != 0) { SetLastError(ERROR_BROKEN_PIPE); return FALSE; }
    if (read) *read = 0;
    if (avail) *avail = DWORD(n);
    if (left) *left = 0;
    return TRUE;
}

// ---- CRT wide file helpers ----
extern "C" FILE* _wfopen(const wchar_t* name, const wchar_t* mode)
{
    const std::string m = Narrow(mode);
    std::string mm; for (char c : m) if (c != 'b' && c != 't' && c != ',' && c != ' ') mm += c; else if (c == 'b') mm += 'b';
    const size_t ccs = m.find("ccs"); (void)ccs;
    const bool create = mm.find('w') != std::string::npos || mm.find('a') != std::string::npos;
    return std::fopen(HostPath(Narrow(name).c_str(), !create).c_str(), mm.substr(0, mm.find_first_of("=")).c_str());
}
extern "C" int _wfopen_s(FILE** f, const wchar_t* name, const wchar_t* mode) { *f = _wfopen(name, mode); return *f ? 0 : errno; }
extern "C" FILE* _wfreopen(const wchar_t* name, const wchar_t* mode, FILE* stream)
{
    const std::string m = Narrow(mode);
    std::string mm; for (char c : m) if (std::isalpha((unsigned char)c) || c == '+') { if (c == 'c') break; mm += c; }
    const std::string p = HostPath(Narrow(name).c_str(), false);
    return std::freopen(p.c_str(), mm.empty() ? "w" : mm.c_str(), stream);
}
extern "C" FILE* _fsopen(const char* name, const char* mode, int) { return std::fopen(HostPath(name, mode && mode[0] == 'r').c_str(), mode); }
extern "C" FILE* _wfsopen(const wchar_t* name, const wchar_t* mode, int) { return _wfopen(name, mode); }
extern "C" int _wremove(const wchar_t* name) { return std::remove(HostPath(Narrow(name).c_str(), true).c_str()); }
extern "C" int _wrename(const wchar_t* from, const wchar_t* to) { return std::rename(HostPath(Narrow(from).c_str(), true).c_str(), HostPath(Narrow(to).c_str(), false).c_str()); }
extern "C" int _wmkdir(const wchar_t* name) { return mkdir(HostPath(Narrow(name).c_str(), false).c_str(), 0755); }
extern "C" int _mkdir(const char* name) { return mkdir(HostPath(name, false).c_str(), 0755); }

// ======================================================================================
// INI files (GetPrivateProfileString & co): read/modify/write the whole file each call.
// ======================================================================================
namespace {
std::mutex g_iniMutex;
struct IniLine { std::string raw; std::string section, key; bool isKey = false, isSection = false; };
std::vector<IniLine> LoadIni(const std::string& path)
{
    std::vector<IniLine> out;
    std::ifstream in(path, std::ios::binary);
    std::string line, section;
    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        IniLine l; l.raw = line;
        size_t s = line.find_first_not_of(" \t");
        if (s != std::string::npos && line[s] == '[')
        {
            const size_t e = line.find(']', s);
            section = line.substr(s + 1, e == std::string::npos ? std::string::npos : e - s - 1);
            l.isSection = true; l.section = section;
        }
        else if (s != std::string::npos && line[s] != ';' && line[s] != '#')
        {
            const size_t eq = line.find('=');
            if (eq != std::string::npos)
            {
                std::string k = line.substr(s, eq - s);
                while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
                l.isKey = true; l.key = k; l.section = section;
            }
        }
        out.push_back(l);
    }
    return out;
}
std::string IniValue(const IniLine& l)
{
    std::string v = l.raw.substr(l.raw.find('=') + 1);
    const size_t a = v.find_first_not_of(" \t");
    v = a == std::string::npos ? std::string() : v.substr(a);
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
    if (v.size() >= 2 && ((v.front() == '"' && v.back() == '"') || (v.front() == '\'' && v.back() == '\''))) v = v.substr(1, v.size() - 2);
    return v;
}
std::string IniPath(LPCSTR file)
{
    if (!file) return std::string(Ttt2AndroidDataRoot()) + "/win.ini";
    std::string f(file);
    if (f.find('/') == std::string::npos && f.find('\\') == std::string::npos) f = std::string(Ttt2AndroidDataRoot()) + "/" + f;
    return HostPath(f.c_str(), true);
}
} // namespace

extern "C" DWORD GetPrivateProfileStringA(LPCSTR section, LPCSTR key, LPCSTR def, LPSTR out, DWORD size, LPCSTR file)
{
    if (!out || !size) return 0;
    std::lock_guard lk(g_iniMutex);
    const auto lines = LoadIni(IniPath(file));
    std::string result;
    bool found = false;
    if (!section)
    {
        for (auto& l : lines) if (l.isSection) { result += l.section; result += '\0'; }
        found = true;
    }
    else if (!key)
    {
        for (auto& l : lines) if (l.isKey && strcasecmp(l.section.c_str(), section) == 0) { result += l.key; result += '\0'; }
        found = true;
    }
    else
    {
        for (auto& l : lines)
            if (l.isKey && strcasecmp(l.section.c_str(), section) == 0 && strcasecmp(l.key.c_str(), key) == 0) { result = IniValue(l); found = true; break; }
    }
    if (!found) result = def ? def : "";
    const DWORD n = DWORD(std::min<size_t>(result.size(), size - 1));
    std::memcpy(out, result.data(), n);
    out[n] = 0;
    return n;
}
extern "C" UINT GetPrivateProfileIntA(LPCSTR section, LPCSTR key, INT def, LPCSTR file)
{
    char buf[64];
    if (!GetPrivateProfileStringA(section, key, "", buf, sizeof(buf), file) || !buf[0]) return UINT(def);
    return UINT(std::strtol(buf, nullptr, 0));
}
extern "C" BOOL WritePrivateProfileStringA(LPCSTR section, LPCSTR key, LPCSTR value, LPCSTR file)
{
    if (!section) return FALSE;
    std::lock_guard lk(g_iniMutex);
    const std::string path = IniPath(file);
    auto lines = LoadIni(path);
    std::vector<std::string> out;
    bool inSection = false, sectionSeen = false, written = false;
    for (size_t i = 0; i < lines.size(); ++i)
    {
        const IniLine& l = lines[i];
        if (l.isSection)
        {
            if (inSection && !written && key && value) { out.push_back(std::string(key) + "=" + value); written = true; }
            inSection = strcasecmp(l.section.c_str(), section) == 0;
            if (inSection) sectionSeen = true;
            if (inSection && !key) continue;          // delete the whole section
            out.push_back(l.raw);
            continue;
        }
        if (inSection && !key) continue;
        if (inSection && l.isKey && strcasecmp(l.key.c_str(), key) == 0)
        {
            if (value && !written) { out.push_back(std::string(key) + "=" + value); written = true; }
            continue;
        }
        out.push_back(l.raw);
    }
    if (key && value && !written)
    {
        if (!sectionSeen) out.push_back(std::string("[") + section + "]");
        out.push_back(std::string(key) + "=" + value);
    }
    const std::string tmp = path + ".tmp";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        if (!o) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
        for (auto& s : out) o << s << "\r\n";
    }
    return rename(tmp.c_str(), path.c_str()) == 0;
}
extern "C" DWORD GetPrivateProfileStringW(LPCWSTR section, LPCWSTR key, LPCWSTR def, LPWSTR out, DWORD size, LPCWSTR file)
{
    std::string buf(size_t(size) * 4 + 4, '\0');
    const std::string s = Narrow(section), k = Narrow(key), d = Narrow(def), f = Narrow(file);
    const DWORD n = GetPrivateProfileStringA(section ? s.c_str() : nullptr, key ? k.c_str() : nullptr, def ? d.c_str() : nullptr, buf.data(), DWORD(buf.size()), file ? f.c_str() : nullptr);
    const std::wstring w = Widen(buf.c_str(), int(n));
    const DWORD m = DWORD(std::min<size_t>(w.size(), size ? size - 1 : 0));
    if (size) { std::wmemcpy(out, w.c_str(), m); out[m] = 0; }
    return m;
}
extern "C" UINT GetPrivateProfileIntW(LPCWSTR section, LPCWSTR key, INT def, LPCWSTR file)
{ return GetPrivateProfileIntA(Narrow(section).c_str(), Narrow(key).c_str(), def, file ? Narrow(file).c_str() : nullptr); }
extern "C" BOOL WritePrivateProfileStringW(LPCWSTR section, LPCWSTR key, LPCWSTR value, LPCWSTR file)
{
    const std::string s = Narrow(section), k = Narrow(key), v = Narrow(value), f = Narrow(file);
    return WritePrivateProfileStringA(section ? s.c_str() : nullptr, key ? k.c_str() : nullptr, value ? v.c_str() : nullptr, file ? f.c_str() : nullptr);
}

// ======================================================================================
// File mappings
// ======================================================================================
namespace {
struct Mapping : Object
{
    Mapping() : Object(Kind::Mapping) {}
    ~Mapping() override { if (fd >= 0) close(fd); }
    int fd = -1; uint64_t size = 0; int prot = PROT_READ;
};
std::mutex g_viewsMutex;
std::map<uintptr_t, size_t>& Views() { static auto* m = new std::map<uintptr_t, size_t>(); return *m; }
} // namespace
extern "C" HANDLE CreateFileMappingW(HANDLE file, LPSECURITY_ATTRIBUTES, DWORD protect, DWORD sizeHigh, DWORD sizeLow, LPCWSTR)
{
    auto* m = new Mapping();
    m->prot = (protect & 0xFF) == PAGE_READWRITE || (protect & 0xFF) == PAGE_EXECUTE_READWRITE ? PROT_READ | PROT_WRITE : PROT_READ;
    const uint64_t want = (uint64_t(sizeHigh) << 32) | sizeLow;
    if (File* f = GetFile(file))
    {
        m->fd = dup(f->fd);
        struct stat st{}; fstat(m->fd, &st);
        m->size = want ? want : uint64_t(st.st_size);
        if (want > uint64_t(st.st_size) && (m->prot & PROT_WRITE)) { if (ftruncate(m->fd, off_t(want)) != 0) {} }
        if (m->size == 0) { delete m; SetLastError(ERROR_FILE_INVALID); return nullptr; }
    }
    else
    {
        m->size = want;                 // pagefile-backed: anonymous memory
    }
    return Wrap(m);
}
extern "C" HANDLE CreateFileMappingA(HANDLE file, LPSECURITY_ATTRIBUTES sa, DWORD protect, DWORD sizeHigh, DWORD sizeLow, LPCSTR)
{ return CreateFileMappingW(file, sa, protect, sizeHigh, sizeLow, nullptr); }
extern "C" LPVOID MapViewOfFile(HANDLE mapping, DWORD access, DWORD offHigh, DWORD offLow, SIZE_T size)
{
    Object* o = Get(mapping);
    if (!o || o->kind != Kind::Mapping) { SetLastError(ERROR_INVALID_HANDLE); return nullptr; }
    auto* m = static_cast<Mapping*>(o);
    const uint64_t off = (uint64_t(offHigh) << 32) | offLow;
    const size_t len = size ? size : size_t(m->size - off);
    const int prot = (access & FILE_MAP_WRITE) ? PROT_READ | PROT_WRITE : PROT_READ;
    void* p = m->fd >= 0 ? mmap(nullptr, len, prot, (access & FILE_MAP_COPY) ? MAP_PRIVATE : MAP_SHARED, m->fd, off_t(off))
                         : mmap(nullptr, len, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { SetErrno(errno); return nullptr; }
    std::lock_guard lk(g_viewsMutex);
    Views()[reinterpret_cast<uintptr_t>(p)] = len;
    return p;
}
extern "C" BOOL UnmapViewOfFile(LPCVOID address)
{
    std::lock_guard lk(g_viewsMutex);
    auto it = Views().find(reinterpret_cast<uintptr_t>(address));
    if (it == Views().end()) { SetLastError(ERROR_INVALID_ADDRESS); return FALSE; }
    munmap(const_cast<void*>(address), it->second);
    Views().erase(it);
    return TRUE;
}
extern "C" BOOL FlushViewOfFile(LPCVOID address, SIZE_T size)
{
    return msync(const_cast<void*>(address), size ? size : 4096, MS_SYNC) == 0;
}
