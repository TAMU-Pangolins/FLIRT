#ifndef FLIRT_COMMON_ESTIMATE_H
#define FLIRT_COMMON_ESTIMATE_H

// The batch's time estimate, shared by flirt-convert's progress display and
// flirt-tui (docs: README "Progress").

#include <cstdint>
#include <string>
#include <vector>

namespace flirt
{

// A run the batch converts, as the estimate sees it.
struct EstimateRun
{
    enum class Phase { Pending, Running, Converted, Ended };

    Phase phase = Phase::Pending;
    uint64_t bytes = 0;     // input file size
    double fraction = 0;    // Running: the share of the run done, 0 to 1
};

// Seconds left, weighted by input size, or a negative value while no estimate
// is shown; elapsed: seconds since the first conversion started.
double estimate_seconds(const std::vector<EstimateRun> &runs, double elapsed);

// "N s" (under a minute, rounded up to 5 s), "N min" (under an hour), "H h MM min".
std::string format_duration(double seconds);

}

#endif
