#include "flirt/run_summary.h"

#include <cstdio>
#include <ctime>
#include <mesytec-mvlc/mvlc_constants.h>
#include <stdexcept>

namespace flirt
{

namespace subtype = mesytec::mvlc::system_event::subtype;

bool is_run_time_subtype(uint8_t st)
{
    return st == subtype::BeginRun || st == subtype::EndRun || st == subtype::UnixTimetick
        || st == subtype::Pause || st == subtype::Resume;
}

void RunTimeAccumulator::record(uint8_t st, int64_t t)
{
    // A damaged timestamp is ignored: past 2^40 s (year ~36800) localtime_r
    // could fail, and the run summary must never fail a conversion.
    if (t < 0 || t > (int64_t(1) << 40))
        return;

    if (st == subtype::BeginRun)
    {
        if (!beginRun_)
            beginRun_ = t;
    }
    else if (st == subtype::EndRun)
        endRun_ = t;
    else if (st == subtype::UnixTimetick)
    {
        if (!firstTick_)
            firstTick_ = t;
        lastTick_ = t;
    }
    else if (st == subtype::Pause)
    {
        if (!pauseStart_) // a Pause while paused changes nothing
        {
            pauseStart_ = t;
            ++pauses_;
        }
    }
    else if (st == subtype::Resume)
    {
        if (pauseStart_) // a Resume without a Pause changes nothing
        {
            paused_ += t - *pauseStart_;
            pauseStart_.reset();
        }
    }
}

RunTimes RunTimeAccumulator::result() const
{
    RunTimes r;

    if (beginRun_)
        r.start = beginRun_, r.startFrom = "BeginRun";
    else if (firstTick_)
        r.start = firstTick_, r.startFrom = "first UnixTimetick";

    if (endRun_)
        r.stop = endRun_, r.stopFrom = "EndRun";
    else if (lastTick_)
        r.stop = lastTick_, r.stopFrom = "last UnixTimetick";

    if (r.start && r.stop && *r.stop >= *r.start)
        r.duration = *r.stop - *r.start;

    r.pauses = pauses_;
    if (!pauseStart_)
        r.paused = paused_;
    else if (r.stop) // never resumed: paused until the stop
        r.paused = paused_ + (*r.stop - *pauseStart_);

    if (r.duration && r.paused)
        r.live = *r.duration - *r.paused;

    return r;
}

std::string format_duration(int64_t seconds)
{
    char buf[64];
    const long long h = seconds / 3600, m = seconds / 60 % 60, s = seconds % 60;

    if (h)
        std::snprintf(buf, sizeof(buf), "%lld h %02lld min %02lld s", h, m, s);
    else if (m)
        std::snprintf(buf, sizeof(buf), "%lld min %02lld s", m, s);
    else
        std::snprintf(buf, sizeof(buf), "%lld s", s);
    return buf;
}

namespace
{

struct tm local_tm(int64_t unixSeconds)
{
    tzset(); // localtime_r need not read TZ itself
    const time_t t = static_cast<time_t>(unixSeconds);
    struct tm result = {};
    if (!localtime_r(&t, &result))
        throw std::runtime_error("localtime_r failed for " + std::to_string(unixSeconds));
    return result;
}

}

std::string format_local_time(int64_t unixSeconds, const char *format)
{
    const auto tm = local_tm(unixSeconds);
    char buf[128];
    const auto n = std::strftime(buf, sizeof(buf), format, &tm);
    return std::string(buf, n);
}

std::string iso8601_local(int64_t unixSeconds)
{
    const auto tm = local_tm(unixSeconds);
    const long off = tm.tm_gmtoff;
    const long a = off < 0 ? -off : off;
    char zone[32];
    std::snprintf(zone, sizeof(zone), "%c%02ld:%02ld", off < 0 ? '-' : '+', a / 3600, a / 60 % 60);
    return format_local_time(unixSeconds, "%Y-%m-%dT%H:%M:%S") + zone;
}

std::string run_summary_line(const RunTimes &times, uint64_t events)
{
    std::string line = "run ";

    if (times.start && times.stop)
    {
        line += format_local_time(*times.start, "%Y-%m-%d %H:%M:%S %Z") + " to ";
        const bool sameDate = format_local_time(*times.start, "%Y-%m-%d")
            == format_local_time(*times.stop, "%Y-%m-%d");
        line += format_local_time(*times.stop, sameDate ? "%H:%M:%S %Z" : "%Y-%m-%d %H:%M:%S %Z");
        line += " (" + (times.duration ? format_duration(*times.duration) : std::string("duration unknown"));
        if (times.pauses && times.paused)
            line += ", " + format_duration(*times.paused) + " paused";
        line += ")";
    }
    else
        line += "times unknown";

    line += ", " + std::to_string(events) + " events";
    if (times.live && *times.live > 0)
    {
        char rate[64];
        std::snprintf(rate, sizeof(rate), " (%.1f/s)", static_cast<double>(events) / static_cast<double>(*times.live));
        line += rate;
    }

    return line;
}

std::string run_times_text(const RunTimes &times)
{
    if (!times.start || !times.stop)
        return "times unknown";

    // "Mar 3, 2026": the day without a leading zero.
    auto date = [](int64_t t)
    {
        auto day = format_local_time(t, "%d");
        if (day.size() > 1 && day[0] == '0')
            day.erase(0, 1);
        return format_local_time(t, "%b ") + day + format_local_time(t, ", %Y");
    };
    const int64_t start = *times.start, stop = *times.stop;
    const bool sameDate = format_local_time(start, "%Y-%m-%d") == format_local_time(stop, "%Y-%m-%d");
    const auto startZone = format_local_time(start, "%Z"), stopZone = format_local_time(stop, "%Z");

    std::string text = date(start) + "  " + format_local_time(start, "%H:%M:%S")
        + (startZone != stopZone ? " " + startZone : "") + " → "
        + (sameDate ? "" : date(stop) + "  ") + format_local_time(stop, "%H:%M:%S") + " " + stopZone;
    text += "  (" + (times.duration ? format_duration(*times.duration) : std::string("duration unknown"));
    if (times.pauses && times.paused)
        text += ", " + format_duration(*times.paused) + " paused";
    return text + ")";
}

}
