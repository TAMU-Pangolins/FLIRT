#ifndef FLIRT_RUN_SUMMARY_H
#define FLIRT_RUN_SUMMARY_H

// Run summary: when the DAQ run started and stopped, from the timestamped
// system sections mesytec-mvlc writes into the listfile.
// mvme itself never computes run times from them (mvlc_stream_worker.cc:298),
// so this is FLIRT's own glue. Plain functions, no ROOT, no Qt.

#include <cstdint>
#include <optional>
#include <string>

namespace flirt
{

// The five timestamped sections (mvlc_constants.h:235-258). Each carries one
// u64 of Unix seconds as two u32 words (mvlc_listfile.cc:133-143). Writers:
// BeginRun, EndRun and UnixTimetick (once a second) in TimetickPlugin
// (mvlc_readout_worker.cc:405-444), Pause and Resume in the readout loop
// (mvlc_readout_worker.cc:1029-1043).
bool is_run_time_subtype(uint8_t subtype);

struct RunTimes
{
    std::optional<int64_t> start, stop;   // Unix seconds
    std::string startFrom, stopFrom;      // "BeginRun", "first UnixTimetick", ...; empty if unknown
    std::optional<int64_t> duration;      // stop - start, if both known and stop >= start
    size_t pauses = 0;                    // Pause sections that started a pause
    std::optional<int64_t> paused;        // sum of Resume - Pause; unknown if a pause never
                                          // resumed and the stop is unknown
    std::optional<int64_t> live;          // duration - paused
};

// Fed the timestamped sections in stream order.
class RunTimeAccumulator
{
    public:
        // Sections of other subtypes, and times before 1970 or past 2^40 s
        // (a damaged section), are ignored.
        void record(uint8_t subtype, int64_t unixSeconds);
        RunTimes result() const;

    private:
        std::optional<int64_t> beginRun_, endRun_, firstTick_, lastTick_, pauseStart_;
        int64_t paused_ = 0;
        size_t pauses_ = 0;
};

// 1 h 02 min 47 s, 22 min 05 s or 43 s.
std::string format_duration(int64_t seconds);

// Local time (localtime_r, TZ honoured) with strftime's format.
std::string format_local_time(int64_t unixSeconds, const char *format);

// ISO 8601 local time with the offset from tm_gmtoff: 2026-03-03T14:10:00-06:00.
std::string iso8601_local(int64_t unixSeconds);

// The run summary line (without the run ID the batch prefixes):
// run <start> <zone> to <stop> <zone> (<duration>[, <paused> paused]), <N> events[ (<rate>/s)]
// The stop's date only if it differs from the start's; "run times unknown"
// without a start or stop.
std::string run_summary_line(const RunTimes &times, uint64_t events);

// flirt-convert's run row, the same times in other words: "Mar 3, 2026
// 14:10:00 → 15:12:47 CST  (1 h 02 min 47 s[, <paused> paused])"; the stop's
// date only if it differs from the start's, the start's zone only if it
// differs from the stop's; "times unknown" without a start or stop.
std::string run_times_text(const RunTimes &times);

}

#endif
