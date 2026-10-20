#include "common/cores.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <sys/sysctl.h>
#else
#include <sched.h>
#endif

namespace flirt
{

int read_limit(int cores, double busy)
{
    // The epsilon: cores x idle fraction is not exact (10 x 0.3 = 2.999...).
    const double free = std::floor(static_cast<double>(cores) - busy + 1e-9);
    const int half = cores / 2;
    const int limit = free < half ? static_cast<int>(std::max(free, 0.0)) : half;
    return std::max(1, limit);
}

int usable_cores()
{
#ifdef __APPLE__
    int n = 0;
    size_t size = sizeof(n);
    if (sysctlbyname("hw.logicalcpu", &n, &size, nullptr, 0) == 0 && n > 0)
        return n;
#else
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0)
        return std::max(1, CPU_COUNT(&set));
#endif
    const long online = sysconf(_SC_NPROCESSORS_ONLN);
    return online > 0 ? static_cast<int>(online) : 1;
}

CpuSample cpu_sample()
{
    CpuSample s;
#ifdef __APPLE__
    host_cpu_load_info_data_t info;
    mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
    if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO, reinterpret_cast<host_info_t>(&info), &count)
        != KERN_SUCCESS)
        return s;
    for (int state = 0; state < CPU_STATE_MAX; ++state)
        s.total += info.cpu_ticks[state];
    s.idle = info.cpu_ticks[CPU_STATE_IDLE];
#else
    // "cpu  user nice system idle iowait irq softirq steal guest guest_nice";
    // guest time is already in user and nice.
    std::ifstream in("/proc/stat");
    std::string line, name;
    if (!std::getline(in, line))
        return s;
    std::istringstream fields(line);
    fields >> name;
    if (name != "cpu")
        return s;
    uint64_t v[8] = {};
    for (auto &x: v)
        if (!(fields >> x))
            return s;
    for (const auto x: v)
        s.total += x;
    s.idle = v[3] + v[4];
#endif
    s.ok = true;
    return s;
}

double idle_fraction(const CpuSample &before, const CpuSample &after)
{
    if (!before.ok || !after.ok || after.total <= before.total || after.idle < before.idle)
        return -1;
    const double idle = static_cast<double>(after.idle - before.idle) / static_cast<double>(after.total - before.total);
    return std::min(1.0, idle);
}

double CpuMeter::idle_now()
{
    using clock = std::chrono::steady_clock;
    if (!last_.ok || clock::now() - at_ > maxAge_)
    {
        last_ = cpu_sample();
        at_ = clock::now();
        std::this_thread::sleep_for(interval_);
    }
    else if (clock::now() - at_ < minInterval_ && fraction_ >= 0)
        return fraction_;
    const auto now = cpu_sample();
    const double fraction = idle_fraction(last_, now);
    if (fraction >= 0)
    {
        last_ = now;
        at_ = clock::now();
        fraction_ = fraction;
    }
    return fraction;
}

int CpuMeter::read_limit_now(int own)
{
    const int cores = usable_cores();
    const double idle = idle_now();
    if (idle >= 0)
        return read_limit(cores, std::max(0.0, cores * (1 - idle) - own));
    // Not measured: the 1-minute load average, which counts this program's
    // reads too.
    double load[1] = { 0 };
    if (getloadavg(load, 1) != 1)
        load[0] = 0;
    return read_limit(cores, std::max(0.0, load[0] - own));
}

}
