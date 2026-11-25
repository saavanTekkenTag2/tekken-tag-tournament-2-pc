// Windows paths through the C library. The PC runtime opens some files with the C/C++ library
// directly (std::ifstream, fopen, std::filesystem) using Windows paths built from
// GetModuleFileName - "C:\game\tekken.img" - which bypass the Win32 layer's path mapping.
// libmain.so is linked with --wrap for these functions (CMakeLists.txt), so every call in the
// library, the static libc++ included, comes here first. Paths with a drive letter ("C:...",
// also after a '/', as std::filesystem::absolute makes them) or with backslashes go through the
// same mapping as CreateFile (drive -> data root, '\' -> '/', case-insensitive lookup); other
// paths pass through untouched.
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

extern "C" int Ttt2HostPath(const char* winPath, char* out, size_t outSize, int mustExist);   // win32_file.cpp

namespace {
struct Mapped
{
    char buf[1024];
    const char* p;
    Mapped(const char* in, bool mustExist)
    {
        p = in;
        if (!in) return;
        const char* drive = nullptr;
        if (std::isalpha((unsigned char)in[0]) && in[1] == ':') drive = in;
        else
            for (const char* s = in; *s; ++s)
                if (*s == '/' && std::isalpha((unsigned char)s[1]) && s[2] == ':' && (s[3] == '\\' || s[3] == '/' || !s[3])) { drive = s + 1; break; }
        if (!drive && !std::strchr(in, '\\')) return;
        const int n = Ttt2HostPath(drive ? drive : in, buf, sizeof buf, mustExist ? 1 : 0);
        if (n > 0 && size_t(n) < sizeof buf) p = buf;
    }
};
bool Creates(int flags) { return (flags & O_CREAT) != 0; }
bool ReadOnly(const char* mode) { return mode && mode[0] == 'r'; }
}

extern "C" {
FILE* __real_fopen(const char*, const char*);
FILE* __real_freopen(const char*, const char*, FILE*);
int __real_open(const char*, int, ...);
int __real___open_2(const char*, int);
int __real_stat(const char*, struct stat*);
int __real_lstat(const char*, struct stat*);
int __real_access(const char*, int);
int __real_mkdir(const char*, mode_t);
DIR* __real_opendir(const char*);
int __real_remove(const char*);
int __real_unlink(const char*);
int __real_rmdir(const char*);
int __real_rename(const char*, const char*);
int __real_truncate(const char*, off_t);
int __real_statvfs(const char*, struct statvfs*);
char* __real_realpath(const char*, char*);

FILE* __wrap_fopen(const char* path, const char* mode) { Mapped m(path, ReadOnly(mode)); return __real_fopen(m.p, mode); }
FILE* __wrap_freopen(const char* path, const char* mode, FILE* f) { Mapped m(path, ReadOnly(mode)); return __real_freopen(path ? m.p : nullptr, mode, f); }
int __wrap_open(const char* path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) { va_list ap; va_start(ap, flags); mode = mode_t(va_arg(ap, int)); va_end(ap); }
    Mapped m(path, !Creates(flags));
    return __real_open(m.p, flags, mode);
}
int __wrap___open_2(const char* path, int flags) { Mapped m(path, !Creates(flags)); return __real___open_2(m.p, flags); }
int __wrap_stat(const char* path, struct stat* st) { Mapped m(path, true); return __real_stat(m.p, st); }
int __wrap_lstat(const char* path, struct stat* st) { Mapped m(path, true); return __real_lstat(m.p, st); }
int __wrap_access(const char* path, int mode) { Mapped m(path, true); return __real_access(m.p, mode); }
int __wrap_mkdir(const char* path, mode_t mode) { Mapped m(path, false); return __real_mkdir(m.p, mode); }
DIR* __wrap_opendir(const char* path) { Mapped m(path, true); return __real_opendir(m.p); }
int __wrap_remove(const char* path) { Mapped m(path, true); return __real_remove(m.p); }
int __wrap_unlink(const char* path) { Mapped m(path, true); return __real_unlink(m.p); }
int __wrap_rmdir(const char* path) { Mapped m(path, true); return __real_rmdir(m.p); }
int __wrap_rename(const char* from, const char* to) { Mapped a(from, true), b(to, false); return __real_rename(a.p, b.p); }
int __wrap_truncate(const char* path, off_t len) { Mapped m(path, true); return __real_truncate(m.p, len); }
int __wrap_statvfs(const char* path, struct statvfs* st) { Mapped m(path, true); return __real_statvfs(m.p, st); }
char* __wrap_realpath(const char* path, char* out) { Mapped m(path, true); return __real_realpath(m.p, out); }
}
