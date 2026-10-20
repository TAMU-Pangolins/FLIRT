#ifndef FLIRT_RUN_TIMELINE_H
#define FLIRT_RUN_TIMELINE_H

// The rate over time: events and lost ETH packets per minute from the
// timestamped system sections, the median rate, and the flags for pauses,
// low-rate periods and rate steps. FLIRT's own glue; mvme never looks at the
// rate over time. No mvlc dependency beyond the subtype constants; the rules
// are in docs/design.md, "Run information".

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "flirt/run_summary.h"

namespace flirt
{

// The rules' parameters. A beam off shows as minutes whose event rate is
// below 15 % of the run median (0.15), and a rate step compares the median
// events/s in the 12 min before and after.
// FLIRT only warns of a low rate: it cannot tell that the beam was off.
namespace rate_rules
{
constexpr double LowRateShare = 0.15;       // a low minute: rate < 0.15 x the median
constexpr int64_t FullMinuteSeconds = 30;   // live seconds for the median and for steps
constexpr int64_t UsableMinuteSeconds = 10; // live seconds to count in a low-rate period; fewer: skipped
constexpr size_t StepWindow = 12;           // minutes in each median of a step
constexpr size_t StepMinBefore = 6;         // usable minutes needed before a step (no start-up transients)
constexpr size_t StepMinAfter = 3;          // usable minutes needed at or after a step
constexpr double StepMinChange = 0.25;      // |after - before| / max(after, before)
}

struct MinuteBin
{
    int64_t start = 0;          // Unix seconds, a multiple of 60
    int64_t liveSeconds = 0;    // the part of [start, stop] in the minute, not paused
    uint64_t events = 0;
    uint64_t lostPackets = 0;

    double rate() const { return liveSeconds > 0 ? double(events) / double(liveSeconds) : 0.0; }
};

struct RateFlag
{
    enum class Kind { Paused, LowRate, Step };
    Kind kind = Kind::Paused;
    int64_t from = 0;               // a step: its minute
    std::optional<int64_t> to;      // paused: unknown if never resumed; a step: none
    double before = 0, after = 0;   // a step's rates, events/s
};

struct RateAnalysis
{
    std::vector<MinuteBin> minutes;     // empty: the rate is unknown
    std::optional<double> median;       // events/s over the minutes with >= 30 live seconds
    std::vector<RateFlag> flags;        // by start time; on ties paused, low rate, step
};

class RunTimeline
{
    public:
        // At every timestamped section, in stream order, with the parser's
        // running totals: the events (the sum of eventHits) and the lost ETH
        // packets so far. What is counted between one section and the next
        // belongs to the earlier section's second, what comes after the last
        // one to that one's (see finish()). Times outside 1970 .. 2^40 s are
        // ignored, as RunTimeAccumulator ignores them.
        void record(uint8_t subtype, int64_t unixSeconds, uint64_t eventsSoFar, uint64_t lostSoFar);

        // The totals at the end of the stream.
        void finish(uint64_t events, uint64_t lostPackets);

        // Minute bins between times.start and times.stop (RunTimeAccumulator's)
        // and the flags. No bins without a start, a stop or any UnixTimetick.
        RateAnalysis analyze(const RunTimes &times) const;

    private:
        struct Mark { int64_t t; uint64_t events, lost; };
        struct Pause { int64_t from; std::optional<int64_t> to; };

        std::vector<Mark> marks_;
        std::vector<Pause> pauses_;
        bool anyTick_ = false;
        bool finished_ = false;
        uint64_t totalEvents_ = 0, totalLost_ = 0;
};

// The median; the mean of the two middle values for an even count. The
// vector is reordered. 0 for none.
double median_of(std::vector<double> values);

// Rates as the texts show them: an integer from 10 on, else one decimal.
std::string format_rate(double perSecond);

// At most two warning lines, flirt-convert's: the low-rate
// periods ("warning: low event rate 14:40-14:42, 14:55-14:56 (under 15 % of
// the run's median rate, 290/s); check whether the beam was off") and the
// rate changes ("warning: the event rate changed at 14:33 (320 -> 180/s),
// 15:05 (180 -> 300/s)"). Each lists at most 3, then "and N more" and
// "; <allListed>" (e.g. "the report lists them all"). Pauses give none:
// they are facts, not guesses.
std::vector<std::string> rate_warnings(const RateAnalysis &rate, const std::string &allListed);

// The same warnings as flirt-convert's run block words them, without
// "warning: ": "low event rate 14:40-14:42 (under 15 % of the run's median
// rate, 290 per second): check whether the beam was off", "the event rate
// changed at 14:33 (320 → 180 per second), 15:05 (180 → 300)"; if either lists
// "and N more", the last one ends with " (<allListed>)".
std::vector<std::string> rate_warning_texts(const RateAnalysis &rate, const std::string &allListed);

}

#endif
