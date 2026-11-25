// Android entry point. SDL2's SDLActivity loads libmain.so and calls SDL_main on its own
// thread; this sets up the data root and the environment the PC launcher/installer would, then
// runs the unchanged PC main() (compiled as ttt2_main).
//
// Data root (TTT2_ROOT): the launcher may pass it; otherwise the app's external files folder
// (/storage/emulated/0/Android/data/<package>/files), falling back to internal storage.
// Layout under it, as the PC installer's game folder:
//   game/tekken.img, game/data*.bin, ...   (built by the installer from the player's disc + TU3)
//   TTT2Recomp/log.txt                     (the game's log; saves and settings next to it)
#include <SDL.h>
#include <jni.h>
#include <cstdint>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <exception>
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <ucontext.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <android/log.h>
#endif

int ttt2_main(int argc, char** argv);                       // main.cpp
extern "C" void Ttt2InstallSignalHandlers(void);            // win32_memory.cpp
extern "C" void Ttt2SetCommandLine(int argc, char** argv);  // win32_core.cpp

extern "C" void Ttt2SetTouchPad(uint32_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry, uint32_t lt, uint32_t rt);   // win32_sdl.cpp

// The launcher's on-screen controls (com.saavan.ttt2.TouchOverlay): XInput buttons, sticks, triggers.
extern "C" JNIEXPORT void JNICALL Java_com_saavan_ttt2_Native_setTouchPad(JNIEnv*, jclass, jint buttons, jint lx, jint ly, jint rx, jint ry, jint lt, jint rt)
{
    Ttt2SetTouchPad(uint32_t(buttons), lx, ly, rx, ry, uint32_t(lt), uint32_t(rt));
}

// ---------------------------------------------------------------------------------------------
// Crash report: <root>/TTT2Recomp/crash.txt (+ logcat tag TTT2). Installed before the Win32
// layer's SIGSEGV handler (write watch, guard pages, the runtime's vectored handlers), which
// chains to this one for faults nobody handled. Frames are module+offset (the build is stripped):
// llvm-addr2line -e libmain.so <offset> on the unstripped build gives the function.
static char g_crashPath[512];
static struct sigaction g_oldCrash[32];

static bool SafeRead(uintptr_t addr, void* out, size_t n)
{
    iovec local{ out, n }, remote{ reinterpret_cast<void*>(addr), n };
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == ssize_t(n);
}

static void CrashOut(int fd, const char* s)
{
    if (fd >= 0) { ssize_t r = write(fd, s, std::strlen(s)); (void)r; }
#ifdef __ANDROID__
    __android_log_write(ANDROID_LOG_FATAL, "TTT2", s);
#endif
    ssize_t r2 = write(2, s, std::strlen(s)); (void)r2;
}

static void Frame(int fd, int i, uintptr_t pc)
{
    char line[600];
    Dl_info info{};
    if (pc && dladdr(reinterpret_cast<void*>(pc), &info) && info.dli_fname)
    {
        const char* base = std::strrchr(info.dli_fname, '/');
        std::snprintf(line, sizeof line, "  #%02d pc %016llx  %s +0x%llx%s%s\n", i, (unsigned long long)pc, base ? base + 1 : info.dli_fname,
                      (unsigned long long)(pc - reinterpret_cast<uintptr_t>(info.dli_fbase)), info.dli_sname ? "  " : "", info.dli_sname ? info.dli_sname : "");
    }
    else std::snprintf(line, sizeof line, "  #%02d pc %016llx  ?\n", i, (unsigned long long)pc);
    CrashOut(fd, line);
}

static void CrashHandler(int sig, siginfo_t* si, void* uc)
{
    const int fd = g_crashPath[0] ? open(g_crashPath, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644) : -1;
    char line[600];
    std::snprintf(line, sizeof line, "=== TTT2 crash: signal %d (%s) code %d, fault address %p, thread %d ===\n", sig,
                  sig == SIGSEGV ? "SIGSEGV" : sig == SIGBUS ? "SIGBUS" : sig == SIGILL ? "SIGILL" : sig == SIGFPE ? "SIGFPE" : sig == SIGABRT ? "SIGABRT" : "?",
                  si ? si->si_code : 0, si ? si->si_addr : nullptr, gettid());
    CrashOut(fd, line);
#if defined(__aarch64__)
    if (uc)
    {
        const mcontext_t& mc = static_cast<ucontext_t*>(uc)->uc_mcontext;
        std::snprintf(line, sizeof line, "pc %016llx lr %016llx sp %016llx fp %016llx\nx0 %016llx x1 %016llx x2 %016llx x3 %016llx\n",
                      (unsigned long long)mc.pc, (unsigned long long)mc.regs[30], (unsigned long long)mc.sp, (unsigned long long)mc.regs[29],
                      (unsigned long long)mc.regs[0], (unsigned long long)mc.regs[1], (unsigned long long)mc.regs[2], (unsigned long long)mc.regs[3]);
        CrashOut(fd, line);
        CrashOut(fd, "backtrace (frame pointers):\n");
        Frame(fd, 0, mc.pc);
        Frame(fd, 1, mc.regs[30]);
        uintptr_t fp = mc.regs[29];
        for (int i = 2; i < 40 && fp && !(fp & 7); ++i)
        {
            uintptr_t rec[2];
            if (!SafeRead(fp, rec, sizeof rec) || rec[0] <= fp) { if (SafeRead(fp, rec, sizeof rec)) Frame(fd, i, rec[1]); break; }
            Frame(fd, i, rec[1]);
            fp = rec[0];
        }
    }
#endif
    if (fd >= 0) close(fd);
    // Hand on to whatever was there before (Android's debuggerd: tombstone), then die with the signal.
    struct sigaction& old = g_oldCrash[sig & 31];
    if ((old.sa_flags & SA_SIGINFO) && old.sa_sigaction) { old.sa_sigaction(sig, si, uc); }
    else if (old.sa_handler != SIG_DFL && old.sa_handler != SIG_IGN && old.sa_handler) { old.sa_handler(sig); }
    signal(sig, SIG_DFL);
    raise(sig);
}

static void InstallCrashReport(const std::string& root)
{
    std::snprintf(g_crashPath, sizeof g_crashPath, "%s/TTT2Recomp/crash.txt", root.c_str());
    unlink(g_crashPath);
    static char altStack[64 * 1024];
    stack_t ss{}; ss.ss_sp = altStack; ss.ss_size = sizeof altStack;
    sigaltstack(&ss, nullptr);
    for (int sig : { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT })
    {
        struct sigaction sa{};
        sa.sa_sigaction = CrashHandler;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&sa.sa_mask);
        sigaction(sig, &sa, &g_oldCrash[sig & 31]);
    }
    std::set_terminate([] {
        std::string what = "unknown";
        if (std::exception_ptr e = std::current_exception())
        {
            try { std::rethrow_exception(e); }
            catch (const std::exception& x) { what = x.what(); }
            catch (...) { what = "non-std exception"; }
        }
        else what = "std::terminate without an exception";
        std::fprintf(stderr, "[android] FATAL: uncaught C++ exception: %s\n", what.c_str());
        if (FILE* f = std::fopen(g_crashPath, "w")) { std::fprintf(f, "uncaught C++ exception: %s\n", what.c_str()); std::fclose(f); }
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_FATAL, "TTT2", "uncaught C++ exception: %s", what.c_str());
#endif
        std::abort();
    });
}

static void MakeDirs(const std::string& path)
{
    for (size_t i = 1; i <= path.size(); ++i)
        if (i == path.size() || path[i] == '/') mkdir(path.substr(0, i).c_str(), 0755);
}

extern "C" int SDL_main(int argc, char* argv[])
{
    std::string root;
    if (const char* r = std::getenv("TTT2_ROOT"); r && *r) root = r;
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], "--root") == 0) root = argv[i + 1];
#ifdef __ANDROID__
    if (root.empty() && SDL_AndroidGetExternalStorageState() & SDL_ANDROID_EXTERNAL_STORAGE_WRITE)
        if (const char* e = SDL_AndroidGetExternalStoragePath()) root = e;
    if (root.empty()) if (const char* in = SDL_AndroidGetInternalStoragePath()) root = in;
#endif
    if (root.empty()) root = "/data/local/tmp/ttt2";
    while (root.size() > 1 && root.back() == '/') root.pop_back();
    MakeDirs(root + "/TTT2Recomp");
    MakeDirs(root + "/tmp");
    if (chdir(root.c_str()) != 0) std::fprintf(stderr, "[android] chdir %s failed (%d)\n", root.c_str(), errno);

    auto setDefault = [](const char* name, const std::string& value) { setenv(name, value.c_str(), 0); };
    setenv("TTT2_ROOT", root.c_str(), 1);
    setDefault("LOCALAPPDATA", root);
    setDefault("APPDATA", root);
    setDefault("USERPROFILE", root);
    setDefault("TEMP", root + "/tmp");
    setDefault("TMP", root + "/tmp");
    setDefault("TTT2_VFS_ROOT", root + "/game");          // the installed game data (main.cpp InstalledLayout)
    setDefault("TTT2_GPU_HW", "1");                        // the hardware renderer (D3D11 -> DXVK -> Vulkan)
    setDefault("TTT2_FBDUMP", "0");
    setDefault("TTT2_SAVE_ROOT", root + "/TTT2Recomp/saves");
    // DXVK (D3D11 -> Vulkan): its log and pipeline state cache next to the game's log
    setDefault("DXVK_LOG_PATH", root + "/TTT2Recomp");
    setDefault("DXVK_LOG_LEVEL", "warn");
    setDefault("DXVK_STATE_CACHE_PATH", root + "/TTT2Recomp");
    setDefault("DXVK_HUD", "0");
    // The log is written directly, not through the PC build's pipe + writer thread: a crash right
    // after start must still leave every line in log.txt.
    setDefault("TTT2_LOG_ASYNC", "0");

#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "TTT2", "root %s; game log: %s/TTT2Recomp/log.txt", root.c_str(), root.c_str());
#endif
    InstallCrashReport(root);          // first: the Win32 layer's handler chains to it
    Ttt2InstallSignalHandlers();
    std::fprintf(stderr, "[android] SDL_main: root %s, starting the game\n", root.c_str());

    // The installed layout is detected with no arguments (argc == 1), as the PC game started
    // from its shortcut. Extra launcher arguments (other than --root) pass through.
    std::vector<char*> args;
    static char self[] = "Tekken Tag Tournament 2.exe";
    args.push_back(self);
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--root") == 0) { ++i; continue; }
        args.push_back(argv[i]);
    }
    args.push_back(nullptr);
    Ttt2SetCommandLine(int(args.size() - 1), args.data());
    const int rc = ttt2_main(int(args.size() - 1), args.data());
    std::fprintf(stderr, "[android] game main returned %d\n", rc);
    std::fflush(stderr);
    return rc;
}
