#ifndef FLIRT_COMMON_CORES_H
#define FLIRT_COMMON_CORES_H

// How many listfiles flirt-info and flirt-tui read at once (docs/design.md,
// "Reading several listfiles at once"): a fraction of the cores, never all of
// them, fewer when the cores are busy.

#include <chrono>
#include <cstdint>

namespace flirt
{

// min(floor(cores / 2), floor(cores - busy)), never below 1; busy: the cores
// in use (by other programs), e.g. cores x (1 - the measured idle fraction).
int read_limit(int cores, double busy);

// The CPUs this process may use: Linux sched_getaffinity, macOS hw.logicalcpu.
int usable_cores();

// The machine's CPU time so far, all CPUs together (Linux: /proc/stat's cpu
// line, idle + iowait as idle; macOS: host_statistics(HOST_CPU_LOAD_INFO),
// CPU_STATE_IDLE as idle). ok false if it cannot be read.
struct CpuSample
{
    bool ok = false;
    uint64_t idle = 0, total = 0;
};
CpuSample cpu_sample();

// The idle fraction between two samples (0..1); negative if they cannot
// tell (not read, or no time between them).
double idle_fraction(const CpuSample &before, const CpuSample &after);

// How busy the cores are, measured: the idle fraction from the last sample
// to now. Without a last sample, or one older than `maxAge`, it takes a
// fresh one over `interval` (sleeping). Asked again sooner than
// `minInterval` after the last sample, it answers the last fraction.
class CpuMeter
{
    public:
        CpuMeter(std::chrono::milliseconds interval, std::chrono::milliseconds maxAge)
            : interval_(interval), maxAge_(maxAge)
        { }

        // The limit now; `own`: this program's reads running, each counted
        // as one core of its own, not as another program's work. If the CPU
        // time cannot be read: the 1-minute load average instead.
        int read_limit_now(int own);

    private:
        double idle_now();

        const std::chrono::milliseconds interval_, maxAge_;
        static constexpr std::chrono::milliseconds minInterval_{ 50 };
        CpuSample last_;
        std::chrono::steady_clock::time_point at_;
        double fraction_ = -1;
};

}

#endif
