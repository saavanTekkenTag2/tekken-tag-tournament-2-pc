#pragma once
// r22 (Android): which cores a thread runs on. Phones mix fast ("big"/"prime") and slow ("little") cores; the
// scheduler often leaves the game's main thread and the command processor on a little core for a while after a
// stall, and shader / pipeline builds compete with them for the big ones. Here: the game's threads are kept on the
// big cores, the background builds on the little ones. Cores are ranked by cpu_capacity (or cpuinfo_max_freq);
// a phone whose cores are all alike gets no masks (everything stays free).
// TTT2_BIG_CORES=0: no affinity at all.
#ifdef __ANDROID__
#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ttt2_affinity
{
struct Masks { cpu_set_t big, little; int nBig = 0, nLittle = 0; bool valid = false; };

inline long ReadLong(const char* path)
{
    FILE* f = std::fopen(path, "r");
    if (!f) return -1;
    long v = -1;
    if (std::fscanf(f, "%ld", &v) != 1) v = -1;
    std::fclose(f);
    return v;
}

inline const Masks& Get()
{
    static const Masks m = [] {
        Masks r;
        CPU_ZERO(&r.big); CPU_ZERO(&r.little);
        const char* e = std::getenv("TTT2_BIG_CORES");
        if (e && *e == '0') return r;
        long n = sysconf(_SC_NPROCESSORS_CONF);
        if (n <= 1 || n > 64) return r;
        long cap[64], top = 0, low = 0x7FFFFFFF;
        for (long i = 0; i < n; ++i)
        {
            char p[96];
            std::snprintf(p, sizeof(p), "/sys/devices/system/cpu/cpu%ld/cpu_capacity", i);
            cap[i] = ReadLong(p);
            if (cap[i] <= 0)
            {
                std::snprintf(p, sizeof(p), "/sys/devices/system/cpu/cpu%ld/cpufreq/cpuinfo_max_freq", i);
                cap[i] = ReadLong(p);
            }
            if (cap[i] <= 0) return r;
            if (cap[i] > top) top = cap[i];
            if (cap[i] < low) low = cap[i];
        }
        if (top == low) return r;
        // "Big" = within 15% of the fastest core, so 1 prime + 3 performance cores count together.
        for (long i = 0; i < n; ++i)
        {
            if (cap[i] * 100 >= top * 85) { CPU_SET(i, &r.big); ++r.nBig; }
            else { CPU_SET(i, &r.little); ++r.nLittle; }
        }
        // Too few big cores for the game's busy threads (main + command processor + audio): use every core but the
        // slowest group for them.
        if (r.nBig < 2)
        {
            CPU_ZERO(&r.big); r.nBig = 0;
            for (long i = 0; i < n; ++i) if (cap[i] > low) { CPU_SET(i, &r.big); ++r.nBig; }
        }
        r.valid = r.nBig > 0 && r.nLittle > 0;
        std::fprintf(stderr, "[cpu] %ld cores: %d big, %d little%s\n", n, r.nBig, r.nLittle, r.valid ? "" : " (no affinity)");
        return r;
    }();
    return m;
}

// The calling thread: a game thread (big cores).
inline void GameThread()
{
    const Masks& m = Get();
    if (m.valid) sched_setaffinity(0, sizeof(cpu_set_t), &m.big);
}

// The calling thread: background work (little cores, lower priority).
inline void BackgroundThread()
{
    const Masks& m = Get();
    if (m.valid) sched_setaffinity(0, sizeof(cpu_set_t), &m.little);
    setpriority(PRIO_PROCESS, 0, 10);
}
} // namespace ttt2_affinity
#endif
