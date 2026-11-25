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
#include <sys/stat.h>
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
    setDefault("DXVK_LOG_LEVEL", "info");
    setDefault("DXVK_STATE_CACHE_PATH", root + "/TTT2Recomp");
    setDefault("DXVK_HUD", "0");

#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "TTT2", "root %s; game log: %s/TTT2Recomp/log.txt", root.c_str(), root.c_str());
#endif
    Ttt2InstallSignalHandlers();

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
