// Android installer: builds the game folder from the player's own disc image and Title Update 3,
// the same steps as the PC installer (port/src/setup/setup_main.cpp RunInstall) with the same
// readers and checks (xdvdfs.cpp, stfs.cpp, game_data.cpp): the retail Default.xex is verified,
// TU3 is applied to it (tekken.img, checked against the image the port was recompiled from),
// every game archive is copied and the LZX-compressed ones are expanded.
// Not done here (Windows-only in the PC installer): payload files, shortcuts, registry, the
// shader prebuild (the game builds shaders as it meets them and caches them).
//
// Called from Java (com.saavan.ttt2.Native.install) on a worker thread. Inputs can be any path
// the process can open - the launcher passes /proc/self/fd/<n> for files picked with Android's
// file picker. Output: <root>/game/...
#include <windows.h>
#include <jni.h>
#include <dirent.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <filesystem>
#include <fcntl.h>

#include "setup/dlc_install.h"
#include "setup/game_data.h"
#include "setup/stfs.h"
#include "setup/xdvdfs.h"

using namespace ttt2::setup;

namespace {

std::wstring W(const std::string& s)
{
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(size_t(n > 0 ? n - 1 : 0), L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

struct Job
{
    std::function<void(int, const std::string&)> report;
    std::function<bool()> cancelled;
    std::vector<std::string> written;
    int pct = 0;
    std::string error;
    void Set(int p, const std::string& s) { pct = p; report(p, s); }
    bool Fail(const std::string& why) { error = why; report(pct, "Failed: " + why); return false; }
};

void MakeDirs(const std::string& path)
{
    for (size_t i = 1; i <= path.size(); ++i)
        if (i == path.size() || path[i] == '/') mkdir(path.substr(0, i).c_str(), 0755);
}

bool WriteBytes(const std::string& path, const void* data, size_t size)
{
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(data, 1, size, f) == size;
    return std::fclose(f) == 0 && ok;
}

// The TU file, or a folder holding all three updates: the package whose Default.xexp targets 0.0.3.2.
bool OpenTitleUpdate(const std::string& path, StfsPackage& pkg, std::vector<uint8_t>& xexp, std::string& why)
{
    std::vector<std::string> candidates;
    struct stat st{};
    // /proc/self/fd/<n> (Android file picker): fstat the open descriptor, the path itself may be refused.
    int rc;
    if (path.rfind("/proc/self/fd/", 0) == 0) rc = fstat(std::atoi(path.c_str() + 14), &st);
    else rc = stat(path.c_str(), &st);
    if (rc != 0) { why = "The title update file was not found."; return false; }
    if (S_ISDIR(st.st_mode))
    {
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((path + "/*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) candidates.push_back(path + "/" + fd.cFileName); }
            while (FindNextFileA(h, &fd));
            FindClose(h);
        }
    }
    else candidates.push_back(path);

    std::string seen;
    for (const std::string& c : candidates)
    {
        std::string err;
        if (!pkg.Open(W(c).c_str(), err)) continue;
        if (pkg.TitleId() != kTitleId) { pkg.Close(); continue; }
        if (!pkg.ReadFile("Default.xexp", xexp)) { pkg.Close(); continue; }
        const std::string version = XexpTargetVersion(xexp);
        if (version == "0.0.3.2") return true;
        seen = version;
        pkg.Close();
    }
    why = !seen.empty() ? "That is the title update for game version " + seen + ". The port needs Title Update 3 "
                          "(version 0.0.3.2, file name ending in \"O2\")."
                        : "No Tekken Tag Tournament 2 title update was found there. Choose the Title Update 3 "
                          "file (TU_174Q22P_0000008000000.00000000000O2).";
    return false;
}

// r30: an extracted game folder, read like the disc image: the launcher passes each top-level file's name and an
// open descriptor (/proc/self/fd/<n>). Entry offsets are virtual - (file index + 1) << 40 - so the disc code reads it.
class FolderImage {
public:
    ~FolderImage() { for (int fd : fds_) if (fd >= 0) close(fd); }
    void Add(const std::string& name, const std::string& path)
    {
        int fd = -1;
        if (path.rfind("/proc/self/fd/", 0) == 0) fd = dup(std::atoi(path.c_str() + 14));
        if (fd < 0) fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) return;
        struct stat st{};
        if (fstat(fd, &st) != 0 || S_ISDIR(st.st_mode)) { close(fd); return; }
        IsoEntry e;
        e.path = name;
        std::replace(e.path.begin(), e.path.end(), '/', '\\');
        e.offset = uint64_t(fds_.size() + 1) << 40;
        e.size = uint32_t(std::min<uint64_t>(uint64_t(st.st_size), 0xFFFFFFFFull));
        e.dir = false;
        big_.push_back(uint64_t(st.st_size) > 0xFFFFFFFFull);
        fds_.push_back(fd);
        entries_.push_back(e);
        paths_.push_back(path);
    }
    const std::vector<IsoEntry>& Entries() const { return entries_; }
    const IsoEntry* Find(const char* path) const
    {
        for (const IsoEntry& e : entries_) if (strcasecmp(e.path.c_str(), path) == 0) return &e;
        return nullptr;
    }
    bool ReadAt(uint64_t off, void* buf, uint32_t n)
    {
        const size_t i = size_t(off >> 40);
        if (!i || i > fds_.size()) return false;
        const uint64_t at = off & ((1ull << 40) - 1);
        uint8_t* p = static_cast<uint8_t*>(buf);
        uint32_t left = n;
        while (left)
        {
            const ssize_t r = pread(fds_[i - 1], p, left, off_t(at + (n - left)));
            if (r <= 0) return false;
            p += r; left -= uint32_t(r);
        }
        return true;
    }
    bool ReadEntry(const IsoEntry& e, std::vector<uint8_t>& out)
    {
        out.resize(e.size);
        return !e.size || ReadAt(e.offset, out.data(), e.size);
    }
    std::string PathOf(const IsoEntry& e) const { return paths_[size_t(e.offset >> 40) - 1]; }
    bool TooBig() const { for (bool b : big_) if (b) return true; return false; }
private:
    std::vector<int> fds_;
    std::vector<IsoEntry> entries_;
    std::vector<std::string> paths_;
    std::vector<bool> big_;
};

struct Planned { const IsoEntry* entry; uint64_t outBytes; bool expand; };

template <class Img> bool RunInstallFrom(Job& job, Img& img, const std::string& tuPath, const std::string& root);

bool RunInstall(Job& job, const std::string& iso, const std::string& tuPath, const std::string& root)
{
    std::string err;
    job.Set(1, "Reading the disc image...");
    XdvdfsImage img;
    if (!img.Open(W(iso).c_str(), err)) return job.Fail(err);
    return RunInstallFrom(job, img, tuPath, root);
}

// r30: a game folder that is already installed (the PC version's game folder: tekken.img with Title Update 3 applied,
// the archives already unpacked, update\data100.bin): checked and copied as it is.
bool CopyInstalledFolder(Job& job, FolderImage& img, const std::string& root)
{
    const IsoEntry* imgEntry = img.Find("tekken.img");
    std::vector<uint8_t> image;
    if (!imgEntry || !img.ReadEntry(*imgEntry, image)) return job.Fail("The game folder could not be read.");
    if (Sha1Hex(image.data(), image.size()) != kImageSha1)
        return job.Fail("tekken.img in that folder is not the Tekken Tag Tournament 2 Title Update 3 game this port was made for.");
    if (!img.Find("update\\data100.bin") || !img.Find("update\\data100.ofs"))
        return job.Fail("That game folder has no update folder (update/data100.bin): choose the whole game folder.");
    uint64_t total = 0;
    for (const IsoEntry& e : img.Entries()) total += e.size;
    struct statvfs vs{};
    MakeDirs(root);
    if (statvfs(root.c_str(), &vs) == 0 && uint64_t(vs.f_bavail) * vs.f_frsize < total + (256ull << 20))
        return job.Fail("Not enough free space to copy the game folder.");
    const std::string gameDir = root + "/game";
    MakeDirs(gameDir + "/update");
    std::vector<uint8_t> buf(1u << 20);
    uint64_t done = 0;
    for (const IsoEntry& e : img.Entries())
    {
        std::string rel = e.path;
        std::replace(rel.begin(), rel.end(), '\\', '/');
        if (rel.rfind("dlc/", 0) == 0) continue;      // DLC goes through SELECT DLC (it is checked there)
        const std::string out = gameDir + "/" + rel;
        if (rel.find('/') != std::string::npos) MakeDirs(out.substr(0, out.rfind('/')));
        job.written.push_back(out);
        FILE* oh = std::fopen(out.c_str(), "wb");
        if (!oh) return job.Fail("Could not write to the game folder.");
        uint64_t at = 0;
        bool ok = true;
        while (at < e.size && ok)
        {
            const uint32_t n = uint32_t(std::min<uint64_t>(e.size - at, buf.size()));
            ok = img.ReadAt(e.offset + at, buf.data(), n) && std::fwrite(buf.data(), 1, n, oh) == n;
            at += n;
            if ((at & ((32u << 20) - 1)) == 0 || at >= e.size)
            {
                if (job.cancelled()) { std::fclose(oh); return job.Fail("Installation cancelled."); }
                job.Set(5 + int((done + at) * 90 / (total ? total : 1)), "Copying " + rel + "...");
            }
        }
        if (std::fclose(oh) != 0) ok = false;
        if (!ok) return job.Fail("Could not copy " + rel + " (the storage may be full).");
        done += e.size;
    }
    const char marker[] = "installed by the Android launcher (game folder)\n";
    WriteBytes(gameDir + "/.ttt2-installed", marker, sizeof(marker) - 1);
    job.Set(100, "Done.");
    return true;
}

bool RunInstallFolder(Job& job, FolderImage& img, std::string tuPath, const std::string& root)
{
    job.Set(1, "Reading the game folder...");
    if (img.TooBig()) return job.Fail("A file in that folder is larger than 4 GB: choose the extracted game folder, not a disc image folder.");
    if (img.Find("tekken.img")) return CopyInstalledFolder(job, img, root);
    if (!img.Find("Default.xex"))
        return job.Fail("That folder is not a Tekken Tag Tournament 2 game folder (no default.xex or tekken.img). Choose the folder "
                        "with default.xex, nrfo.bin and the data files.");
    // No title update chosen: one inside the folder (TU_... file) is used.
    if (tuPath.empty())
        for (const IsoEntry& e : img.Entries())
            if (strncasecmp(e.path.c_str(), "TU_", 3) == 0) { tuPath = img.PathOf(e); break; }
    if (tuPath.empty()) return job.Fail("Choose Title Update 3 first (SELECT TITLE UPDATE).");
    return RunInstallFrom(job, img, tuPath, root);
}

template <class Img> bool RunInstallFrom(Job& job, Img& img, const std::string& tuPath, const std::string& root)
{
    std::string err, why;

    const IsoEntry* xexEntry = img.Find("Default.xex");
    std::vector<uint8_t> baseXex;
    if (!xexEntry || !img.ReadEntry(*xexEntry, baseXex))
        return job.Fail("That disc image is not Tekken Tag Tournament 2 (no Default.xex).");
    if (Sha1Hex(baseXex.data(), baseXex.size()) != kBaseXexSha1)
        return job.Fail("That disc image is not the Tekken Tag Tournament 2 (USA, Europe) retail disc this port "
                        "was made for, or the image is damaged.");

    job.Set(2, "Reading the title update...");
    StfsPackage tu;
    std::vector<uint8_t> xexp, tuArchive, tuIndex;
    if (!OpenTitleUpdate(tuPath, tu, xexp, why)) return job.Fail(why);
    if (!tu.ReadFile("data100.bin", tuArchive) || !tu.ReadFile("data100.ofs", tuIndex))
        return job.Fail("The title update file is incomplete or damaged.");
    const uint64_t tuExpanded = RecordExpandedSize(tuIndex);
    if (tuArchive.size() < 16 || !IsLzxContainer(tuArchive.data()) || !tuExpanded)
        return job.Fail("The title update file is incomplete or damaged.");

    job.Set(3, "Applying Title Update 3 to the game...");
    std::vector<uint8_t> image;
    if (!BuildGameImage(baseXex, xexp, image, err)) return job.Fail(err);

    const IsoEntry* nrfoEntry = img.Find("nrfo.bin");
    std::vector<uint8_t> nrfo;
    if (!nrfoEntry || !img.ReadEntry(*nrfoEntry, nrfo)) return job.Fail("The disc image is incomplete (no nrfo.bin).");
    std::vector<std::string> archives;
    for (const IsoEntry& e : img.Entries())
        if (!e.dir && e.path.find('\\') == std::string::npos && e.path.size() == 11 &&
            strncasecmp(e.path.c_str(), "data", 4) == 0 && strcasecmp(e.path.c_str() + 7, ".bin") == 0)
            archives.push_back(e.path);
    std::sort(archives.begin(), archives.end(), [](const std::string& a, const std::string& b) { return strcasecmp(a.c_str(), b.c_str()) < 0; });
    if (archives.size() != 42) return job.Fail("The disc image is incomplete (expected 42 game archives).");
    const auto sizes = ArchiveSizes(nrfo, archives);

    std::vector<Planned> plan;
    uint64_t total = image.size() + tuExpanded + tuIndex.size();
    for (const IsoEntry& e : img.Entries())
    {
        if (e.dir || e.path.find('\\') != std::string::npos || strcasecmp(e.path.c_str(), "AvatarAssetPack") == 0) continue;
        if (strncasecmp(e.path.c_str(), "TU_", 3) == 0) continue;     // r30: a title update kept in a game folder
        uint8_t head[4] = {};
        if (e.size >= 4 && !img.ReadAt(e.offset, head, 4)) return job.Fail("The disc image could not be read.");
        Planned p{ &e, e.size, false };
        const auto it = sizes.find(e.path);
        if (e.size >= 16 && IsLzxContainer(head))
        {
            if (it == sizes.end() || !it->second) return job.Fail("The disc image does not match the expected game data.");
            p.outBytes = it->second;
            p.expand = true;
        }
        plan.push_back(p);
        total += p.outBytes;
    }
    struct statvfs vs{};
    MakeDirs(root);
    if (statvfs(root.c_str(), &vs) == 0)
    {
        const uint64_t freeBytes = uint64_t(vs.f_bavail) * vs.f_frsize;
        if (freeBytes < total + (256ull << 20))
        {
            char msg[160];
            std::snprintf(msg, sizeof msg, "Not enough free space: %.1f GB needed, %.1f GB free.", double(total + (256ull << 20)) / 1e9, double(freeBytes) / 1e9);
            return job.Fail(msg);
        }
    }

    job.Set(4, "Creating the game folder...");
    const std::string gameDir = root + "/game";
    MakeDirs(gameDir + "/update");

    uint64_t done = 0, shown = 0;
    auto report = [&](const std::string& what, uint64_t extra) {
        if (job.cancelled()) return false;
        const int pct = 5 + int((done + extra) * 90 / (total ? total : 1));
        if (done + extra - shown > (16u << 20) || extra == 0) { shown = done + extra; job.Set(pct, what); }
        return true;
    };
    std::vector<uint8_t> buf(1u << 20);
    for (const Planned& p : plan)
    {
        const std::string name = p.entry->path, out = gameDir + "/" + name;
        job.written.push_back(out);
        if (p.expand)
        {
            const std::string what = "Unpacking " + name + "...";
            const IsoEntry* e = p.entry;
            if (!ExpandContainer([&](uint64_t off, void* b, uint32_t n) { return img.ReadAt(e->offset + off, b, n); },
                                 e->size, p.outBytes, W(out), [&](uint64_t w) { return report(what, w); }, err))
                return job.Fail(err == "Installation cancelled." ? err : "Could not unpack " + name + ": " + err + ".");
        }
        else
        {
            const std::string what = "Copying " + name + "...";
            FILE* oh = std::fopen(out.c_str(), "wb");
            if (!oh) return job.Fail("Could not write to the game folder.");
            uint64_t left = p.entry->size, at = 0;
            bool ok = true;
            while (left && ok)
            {
                const uint32_t n = uint32_t(std::min<uint64_t>(left, buf.size()));
                ok = img.ReadAt(p.entry->offset + at, buf.data(), n) && std::fwrite(buf.data(), 1, n, oh) == n;
                at += n; left -= n;
                if (ok && !report(what, at)) { std::fclose(oh); std::remove(out.c_str()); return job.Fail("Installation cancelled."); }
            }
            if (std::fclose(oh) != 0) ok = false;
            if (!ok) { std::remove(out.c_str()); return job.Fail("The disc image could not be read, or the storage is full."); }
        }
        done += p.outBytes;
    }

    job.Set(95, "Installing Title Update 3 data...");
    job.written.push_back(gameDir + "/update/data100.bin");
    job.written.push_back(gameDir + "/update/data100.ofs");
    job.written.push_back(gameDir + "/tekken.img");
    if (!ExpandContainer([&](uint64_t off, void* b, uint32_t n) {
                             if (off + n > tuArchive.size()) return false;
                             std::memcpy(b, tuArchive.data() + off, n);
                             return true;
                         }, tuArchive.size(), tuExpanded, W(gameDir + "/update/data100.bin"), {}, err))
        return job.Fail("Could not unpack the title update: " + err + ".");
    if (!WriteBytes(gameDir + "/update/data100.ofs", tuIndex.data(), tuIndex.size()) ||
        !WriteBytes(gameDir + "/tekken.img", image.data(), image.size()))
        return job.Fail("Could not write to the game folder (the storage may be full).");
    const char marker[] = "installed by the Android launcher (disc + TU3)\n";
    WriteBytes(gameDir + "/.ttt2-installed", marker, sizeof(marker) - 1);
    job.Set(100, "Done.");
    return true;
}

} // namespace

// Java: static native String install(String iso, String tu, String root, Native.Listener listener)
// Returns null on success, else the error message. Listener: void progress(int pct, String status);
// boolean cancelled().
extern "C" JNIEXPORT jstring JNICALL Java_com_saavan_ttt2_Native_install(JNIEnv* env, jclass, jstring jiso, jstring jtu, jstring jroot, jobject listener)
{
    auto str = [env](jstring s) { const char* c = env->GetStringUTFChars(s, nullptr); std::string r(c ? c : ""); if (c) env->ReleaseStringUTFChars(s, c); return r; };
    const std::string iso = str(jiso), tu = str(jtu), root = str(jroot);
    jclass cls = env->GetObjectClass(listener);
    jmethodID progress = env->GetMethodID(cls, "progress", "(ILjava/lang/String;)V");
    jmethodID cancelled = env->GetMethodID(cls, "cancelled", "()Z");
    Job job;
    job.report = [&](int pct, const std::string& s) {
        jstring js = env->NewStringUTF(s.c_str());
        env->CallVoidMethod(listener, progress, jint(pct), js);
        env->DeleteLocalRef(js);
        std::fprintf(stderr, "[install] %3d%% %s\n", pct, s.c_str());
    };
    job.cancelled = [&] { return env->CallBooleanMethod(listener, cancelled) == JNI_TRUE; };
    if (RunInstall(job, iso, tu, root)) return nullptr;
    // Take back what this run wrote (newest first)
    for (auto it = job.written.rbegin(); it != job.written.rend(); ++it) std::remove(it->c_str());
    return env->NewStringUTF(job.error.empty() ? "Installation failed." : job.error.c_str());
}

// r23: Java: static native String buildShaders(String root, Native.Listener listener)
// Every shader in the installed game (game/data002.bin, data003.bin, tekken.img) translated and compiled into the
// shader cache ahead of time - the PC installer's --build-shaders step (gpu_hw_d3d11.h PrebuildShaders). Without
// it a match's first start compiles its shaders while it runs (black stage, characters drawn late; one vertex
// shader took 28 s on a phone). Resumable: shaders already in the cache are skipped. Null = done, else the error.
extern int GpuPrebuildShadersProgress(const std::vector<std::filesystem::path>& files, unsigned threads,
                                      bool (*progress)(void* user, size_t done, size_t total), void* user);
extern "C" JNIEXPORT jstring JNICALL Java_com_saavan_ttt2_Native_buildShaders(JNIEnv* env, jclass, jstring jroot, jobject listener)
{
    const char* c = env->GetStringUTFChars(jroot, nullptr);
    std::string root(c ? c : "");
    if (c) env->ReleaseStringUTFChars(jroot, c);
    while (root.size() > 1 && root.back() == '/') root.pop_back();
    setenv("LOCALAPPDATA", root.c_str(), 1);          // the game's cache: <root>/TTT2Recomp/shadercache
    struct Ctx { JNIEnv* env; jobject listener; jmethodID progress, cancelled; } ctx{ env, listener, nullptr, nullptr };
    jclass cls = env->GetObjectClass(listener);
    ctx.progress = env->GetMethodID(cls, "progress", "(ILjava/lang/String;)V");
    ctx.cancelled = env->GetMethodID(cls, "cancelled", "()Z");
    auto cb = [](void* user, size_t done, size_t total) -> bool {
        Ctx& x = *static_cast<Ctx*>(user);
        char text[96];
        std::snprintf(text, sizeof(text), "Building shaders: %zu / %zu", done, total);
        jstring js = x.env->NewStringUTF(text);
        x.env->CallVoidMethod(x.listener, x.progress, jint(total ? done * 100 / total : 0), js);
        x.env->DeleteLocalRef(js);
        return x.env->CallBooleanMethod(x.listener, x.cancelled) != JNI_TRUE;
    };
    const std::string game = root + "/game";
    // All cores but one: the launcher stays responsive.
    const long cores = sysconf(_SC_NPROCESSORS_ONLN);
    const unsigned threads = unsigned(cores > 2 ? cores - 1 : 1);
    std::fprintf(stderr, "[shaders] building from %s on %u threads\n", game.c_str(), threads);
    const int rc = GpuPrebuildShadersProgress({ game + "/data002.bin", game + "/data003.bin", game + "/tekken.img" }, threads, cb, &ctx);
    if (rc == 0) return nullptr;
    char err[96];
    std::snprintf(err, sizeof(err), rc == 6 ? "Stopped - it continues where it left off next time." : "Shader build failed (code %d).", rc);
    return env->NewStringUTF(err);
}

namespace {
struct JniJob
{
    JNIEnv* env; jobject listener; jmethodID progress, cancelled;
    JniJob(JNIEnv* e, jobject l) : env(e), listener(l)
    {
        jclass cls = env->GetObjectClass(listener);
        progress = env->GetMethodID(cls, "progress", "(ILjava/lang/String;)V");
        cancelled = env->GetMethodID(cls, "cancelled", "()Z");
    }
    void Bind(Job& job)
    {
        job.report = [this](int pct, const std::string& s) {
            jstring js = env->NewStringUTF(s.c_str());
            env->CallVoidMethod(listener, progress, jint(pct), js);
            env->DeleteLocalRef(js);
            std::fprintf(stderr, "[install] %3d%% %s\n", pct, s.c_str());
        };
        job.cancelled = [this] { return env->CallBooleanMethod(listener, cancelled) == JNI_TRUE; };
    }
};
std::string Str(JNIEnv* env, jstring s)
{
    if (!s) return std::string();
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string r(c ? c : "");
    if (c) env->ReleaseStringUTFChars(s, c);
    return r;
}
}

// r30: Java: static native String installFolder(String[] names, String[] paths, String tu, String root, Listener l)
// An extracted game folder (default.xex + nrfo.bin + data*.bin, as extracted from the disc) is installed like the disc
// image; an already-installed game folder (tekken.img + update/) is checked and copied. Null = success.
extern "C" JNIEXPORT jstring JNICALL Java_com_saavan_ttt2_Native_installFolder(JNIEnv* env, jclass, jobjectArray jnames,
                                                                               jobjectArray jpaths, jstring jtu, jstring jroot, jobject listener)
{
    FolderImage img;
    const jsize n = env->GetArrayLength(jnames);
    for (jsize i = 0; i < n; ++i)
    {
        jstring a = jstring(env->GetObjectArrayElement(jnames, i)), b = jstring(env->GetObjectArrayElement(jpaths, i));
        img.Add(Str(env, a), Str(env, b));
        env->DeleteLocalRef(a); env->DeleteLocalRef(b);
    }
    JniJob jj(env, listener);
    Job job;
    jj.Bind(job);
    std::fprintf(stderr, "[install] game folder: %zu files\n", img.Entries().size());
    if (RunInstallFolder(job, img, Str(env, jtu), Str(env, jroot))) return nullptr;
    for (auto it = job.written.rbegin(); it != job.written.rend(); ++it) std::remove(it->c_str());
    return env->NewStringUTF(job.error.empty() ? "Installation failed." : job.error.c_str());
}

// r32: what the folder holds instead of game DLC: bit 0 avatar items (content type 0x9000), bit 1 other packages.
static int OtherPackages(const std::string& dir, int depth = 0)
{
    int kinds = 0;
    DIR* d = opendir(dir.c_str());
    if (!d) return 0;
    while (dirent* e = readdir(d))
    {
        const std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        const std::string path = dir + "/" + name;
        struct stat st{};
        if (stat(path.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { if (depth < 4) kinds |= OtherPackages(path, depth + 1); continue; }
        if (!S_ISREG(st.st_mode) || st.st_size < 0x400) continue;
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) continue;
        unsigned char magic[4] = {}, type[4] = {};
        const bool read = std::fread(magic, 1, 4, f) == 4 && std::fseek(f, 0x344, SEEK_SET) == 0 && std::fread(type, 1, 4, f) == 4;
        std::fclose(f);
        if (!read || !(std::memcmp(magic, "LIVE", 4) == 0 || std::memcmp(magic, "PIRS", 4) == 0 || std::memcmp(magic, "CON ", 4) == 0)) continue;
        const uint32_t t = uint32_t(type[0]) << 24 | uint32_t(type[1]) << 16 | uint32_t(type[2]) << 8 | type[3];
        kinds |= t == 0x00009000 ? 1 : t != 0x00000002 ? 2 : 0;
    }
    closedir(d);
    return kinds;
}

// r30: Java: static native String installDlc(String folder, String root, Listener l)
// The player's downloadable content packages (Bonus Tracks / Bonus Movies, content type 2) found in `folder` are checked
// against their own hashes and extracted to <root>/game/dlc, where the game lists them. Returns "OK:<n>" or the error.
extern "C" JNIEXPORT jstring JNICALL Java_com_saavan_ttt2_Native_installDlc(JNIEnv* env, jclass, jstring jdir, jstring jroot, jobject listener)
{
    const std::string dir = Str(env, jdir), root = Str(env, jroot);
    JniJob jj(env, listener);
    Job job;
    jj.Bind(job);
    job.Set(1, "Looking for DLC packages...");
    int damaged = 0;
    const std::vector<DlcPackage> found = FindDlc(W(dir), &damaged);
    if (found.empty())
    {
        if (damaged) return env->NewStringUTF("The DLC files found there are damaged or incomplete.");
        const int kinds = OtherPackages(dir);
        if (kinds & 1)
            return env->NewStringUTF("Those files are Xbox 360 avatar items (folder 00009000), not game DLC. They are clothes for "
                                     "the Xbox dashboard avatar and do nothing in the game itself, so they can't be added. "
                                     "Game DLC is in a folder named 00000002 (Bonus Tracks / Bonus Movies).");
        if (kinds & 2)
            return env->NewStringUTF("Those files are Xbox 360 packages of another kind (not game DLC). Only the game's own DLC, "
                                     "in a folder named 00000002, can be added.");
        return env->NewStringUTF("No Tekken Tag Tournament 2 DLC was found there. Choose the folder that holds the DLC "
                                 "packages (usually .../4E4D0859/00000002).");
    }
    uint64_t total = 0, done = 0;
    for (const DlcPackage& p : found) total += p.bytes;
    struct statvfs vs{};
    if (statvfs(root.c_str(), &vs) == 0 && uint64_t(vs.f_bavail) * vs.f_frsize < total + (64ull << 20))
        return env->NewStringUTF("Not enough free space for the DLC.");
    const std::wstring gameDir = W(root + "/game");
    int ok = 0;
    std::string firstError;
    for (const DlcPackage& p : found)
    {
        std::vector<std::wstring> written;
        std::string err;
        char disp[160] = {};
        for (size_t i = 0; i < p.display.size() && i + 1 < sizeof(disp); ++i) disp[i] = p.display[i] < 128 ? char(p.display[i]) : '?';
        const std::string what = std::string("Installing ") + disp + "...";
        if (InstallDlc(p, gameDir, [&](uint64_t w) {
                if (job.cancelled()) return false;
                job.Set(5 + int((done + w) * 90 / (total ? total : 1)), what);
                return true;
            }, written, err)) ++ok;
        else if (firstError.empty()) firstError = std::string(disp) + ": " + err;
        done += p.bytes;
        if (err == "Installation cancelled.") break;
    }
    std::fprintf(stderr, "[install] DLC: %d of %zu installed%s%s\n", ok, found.size(), firstError.empty() ? "" : " - ", firstError.c_str());
    if (!ok) return env->NewStringUTF(firstError.empty() ? "The DLC could not be installed." : firstError.c_str());
    char r[64];
    std::snprintf(r, sizeof(r), "OK:%d", ok);
    return env->NewStringUTF(r);
}
