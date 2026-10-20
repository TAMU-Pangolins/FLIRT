#include "estimate.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace flirt
{

double estimate_seconds(const std::vector<EstimateRun> &runs, double elapsed)
{
    using Phase = EstimateRun::Phase;
    double done = 0, remaining = 0;

    for (const auto &run: runs)
    {
        const double bytes = static_cast<double>(run.bytes);
        if (run.phase == Phase::Converted)
            done += bytes;
        else if (run.phase == Phase::Pending)
            remaining += bytes;
        else if (run.phase == Phase::Running)
        {
            done += run.fraction * bytes;
            remaining += (1 - run.fraction) * bytes;
        }
    }

    if (elapsed < 5 || done <= 0 || done < 0.01 * (done + remaining))
        return -1;
    return remaining / (done / elapsed);
}

std::string format_duration(double seconds)
{
    // Decided on the rounded values, so 57 s reads "1 min", not "60 s".
    const long rounded = std::max(5L, static_cast<long>(std::ceil(seconds / 5)) * 5);
    if (rounded < 60)
        return std::to_string(rounded) + " s";
    const long minutes = std::max(1L, std::lround(seconds / 60));
    if (minutes < 60)
        return std::to_string(minutes) + " min";
    char text[32];
    std::snprintf(text, sizeof(text), "%ld h %02ld min", minutes / 60, minutes % 60);
    return text;
}

}
