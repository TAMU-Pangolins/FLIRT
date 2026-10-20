#include "flirt/run_timeline.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mesytec-mvlc/mvlc_constants.h>

namespace flirt
{

namespace subtype = mesytec::mvlc::system_event::subtype;
namespace rr = rate_rules;

namespace
{

// At most about two years of minutes: a damaged section's time must not make
// the bins unbounded; such a run's rate is unknown.
constexpr int64_t MaxMinutes = 2 * 366 * 24 * 60;

int64_t minute_of(int64_t t)
{
    return t >= 0 ? t / 60 * 60 : -((-t + 59) / 60) * 60;
}

// Seconds of [a0, a1) in [b0, b1).
int64_t overlap(int64_t a0, int64_t a1, int64_t b0, int64_t b1)
{
    return std::max<int64_t>(0, std::min(a1, b1) - std::max(a0, b0));
}

}

double median_of(std::vector<double> values)
{
    if (values.empty())
        return 0;
    const size_t n = values.size();
    std::sort(values.begin(), values.end());
    return n % 2 ? values[n / 2] : (values[n / 2 - 1] + values[n / 2]) / 2;
}

void RunTimeline::record(uint8_t st, int64_t t, uint64_t eventsSoFar, uint64_t lostSoFar)
{
    if (!is_run_time_subtype(st) || t < 0 || t > (int64_t(1) << 40))
        return;

    marks_.push_back({ t, eventsSoFar, lostSoFar });

    if (st == subtype::UnixTimetick)
        anyTick_ = true;
    // As RunTimeAccumulator: a Pause while paused and a Resume without a
    // Pause change nothing.
    else if (st == subtype::Pause && (pauses_.empty() || pauses_.back().to))
        pauses_.push_back({ t, std::nullopt });
    else if (st == subtype::Resume && !pauses_.empty() && !pauses_.back().to)
        pauses_.back().to = t;
}

void RunTimeline::finish(uint64_t events, uint64_t lostPackets)
{
    finished_ = true;
    totalEvents_ = events;
    totalLost_ = lostPackets;
}

RateAnalysis RunTimeline::analyze(const RunTimes &times) const
{
    RateAnalysis result;

    const bool binned = times.start && times.stop && *times.stop >= *times.start && anyTick_
        && (minute_of(*times.stop) - minute_of(*times.start)) / 60 < MaxMinutes;

    // Paused intervals come from the pause list, whether the rate is known or not.
    for (const auto &p: pauses_)
    {
        RateFlag flag;
        flag.kind = RateFlag::Kind::Paused;
        flag.from = p.from;
        flag.to = p.to;
        result.flags.push_back(flag);
    }

    if (binned)
    {
        const int64_t start = *times.start, stop = *times.stop;
        const int64_t first = minute_of(start);
        const size_t count = static_cast<size_t>((minute_of(stop) - first) / 60 + 1);
        result.minutes.resize(count);

        for (size_t i = 0; i < count; ++i)
        {
            auto &bin = result.minutes[i];
            bin.start = first + 60 * static_cast<int64_t>(i);
            const int64_t end = bin.start + 60;
            // [start, stop] holds stop - start seconds, as the run's duration.
            int64_t live = overlap(start, stop, bin.start, end);
            for (const auto &p: pauses_)
                live -= overlap(std::max(p.from, start), std::min(p.to.value_or(stop), stop), bin.start, end);
            bin.liveSeconds = std::max<int64_t>(0, live);
        }

        // The counts after each mark, up to the next one (or the end), belong
        // to its second; the counts before the first mark to the first. A
        // second outside the run's minutes counts in the nearest minute.
        uint64_t prevEvents = 0, prevLost = 0;
        for (size_t i = 0; i < marks_.size(); ++i)
        {
            const bool last = i + 1 == marks_.size();
            const uint64_t events = last ? (finished_ ? totalEvents_ : marks_[i].events) : marks_[i + 1].events;
            const uint64_t lost = last ? (finished_ ? totalLost_ : marks_[i].lost) : marks_[i + 1].lost;
            const int64_t index = std::clamp<int64_t>((minute_of(marks_[i].t) - first) / 60, 0,
                                                      static_cast<int64_t>(count) - 1);
            auto &bin = result.minutes[static_cast<size_t>(index)];
            bin.events += events >= prevEvents ? events - prevEvents : 0;
            bin.lostPackets += lost >= prevLost ? lost - prevLost : 0;
            prevEvents = std::max(prevEvents, events);
            prevLost = std::max(prevLost, lost);
        }
    }

    const auto &bins = result.minutes;
    std::vector<double> full;
    for (const auto &bin: bins)
        if (bin.liveSeconds >= rr::FullMinuteSeconds)
            full.push_back(bin.rate());

    std::vector<RateFlag> lowRate, steps;

    if (!full.empty())
    {
        const double median = median_of(full);
        result.median = median;

        // Low rate: maximal runs of consecutive low minutes; minutes with
        // fewer than 10 live seconds neither extend nor break one; a period
        // needs a low minute with at least 30 live seconds (one is enough).
        auto low = [&](const MinuteBin &bin) { return median > 0 && bin.rate() < rr::LowRateShare * median; };
        std::vector<size_t> period;
        auto close = [&]()
        {
            const bool hasFull = std::any_of(period.begin(), period.end(), [&](size_t i)
                                             { return bins[i].liveSeconds >= rr::FullMinuteSeconds; });
            if (hasFull)
            {
                RateFlag flag;
                flag.kind = RateFlag::Kind::LowRate;
                flag.from = bins[period.front()].start;
                flag.to = bins[period.back()].start + 60;
                lowRate.push_back(flag);
            }
            period.clear();
        };
        for (size_t i = 0; i < bins.size(); ++i)
        {
            if (bins[i].liveSeconds < rr::UsableMinuteSeconds)
                continue;
            if (low(bins[i]))
                period.push_back(i);
            else
                close();
        }
        close();

        // Steps, over the usable minutes U: >= 30 live seconds and not low,
        // whether or not the low minute is part of a low-rate period.
        std::vector<size_t> U;
        for (size_t i = 0; i < bins.size(); ++i)
            if (bins[i].liveSeconds >= rr::FullMinuteSeconds && !low(bins[i]))
                U.push_back(i);
        std::vector<double> r;
        for (size_t i: U)
            r.push_back(bins[i].rate());

        struct Candidate { size_t k; double d, before, after; };
        std::vector<std::vector<Candidate>> groups;
        for (size_t k = rr::StepMinBefore; k + rr::StepMinAfter <= r.size(); ++k)
        {
            const size_t b0 = k > rr::StepWindow ? k - rr::StepWindow : 0;
            const size_t a1 = std::min(r.size(), k + rr::StepWindow);
            const double before = median_of(std::vector<double>(r.begin() + b0, r.begin() + k));
            const double after = median_of(std::vector<double>(r.begin() + k, r.begin() + a1));
            const double m = std::max(before, after);
            const double d = m > 0 ? std::abs(after - before) / m : 0;
            if (d < rr::StepMinChange)
                continue;
            if (groups.empty() || groups.back().back().k + 1 != k)
                groups.emplace_back();
            groups.back().push_back({ k, d, before, after });
        }

        // Placement: a k where r[k], r[k+1] and r[k+2] are all past the
        // midpoint of its before and after medians, in the step's direction,
        // and r[k-1] is not; the largest d, the earliest on ties. None: no step.
        for (const auto &group: groups)
        {
            const Candidate *best = nullptr;
            for (const auto &c: group)
            {
                const double mid = (c.before + c.after) / 2;
                auto past = [&](double v) { return c.after > c.before ? v > mid : v < mid; };
                bool ok = !past(r[c.k - 1]);
                for (size_t j = 0; ok && j < rr::StepMinAfter; ++j)
                    ok = past(r[c.k + j]);
                if (ok && (!best || c.d > best->d))
                    best = &c;
            }
            if (best)
            {
                RateFlag flag;
                flag.kind = RateFlag::Kind::Step;
                flag.from = bins[U[best->k]].start;
                flag.before = best->before;
                flag.after = best->after;
                steps.push_back(flag);
            }
        }
    }

    // One time order, by start; on ties paused, low rate, step (stable).
    result.flags.insert(result.flags.end(), lowRate.begin(), lowRate.end());
    result.flags.insert(result.flags.end(), steps.begin(), steps.end());
    std::stable_sort(result.flags.begin(), result.flags.end(),
                     [](const RateFlag &a, const RateFlag &b) { return a.from < b.from; });
    return result;
}

std::string format_rate(double perSecond)
{
    char buf[64];
    if (perSecond >= 10)
        std::snprintf(buf, sizeof(buf), "%.0f", perSecond);
    else
        std::snprintf(buf, sizeof(buf), "%.1f", perSecond);
    return buf;
}

namespace
{

// Items a warning lists (not too much at once; the rest are in the report).
constexpr size_t MaxListed = 3;

// "14:40-14:42": a low-rate period, from its first minute to its last + 60 s.
std::string low_rate_period(const RateFlag &flag)
{
    return format_local_time(flag.from, "%H:%M") + "-" + format_local_time(flag.to.value_or(flag.from), "%H:%M");
}

// "a, b, c and 12 more": the first MaxListed items.
std::string first_items(const std::vector<std::string> &items, const char *separator = ", ")
{
    std::string text;
    for (size_t i = 0; i < items.size() && i < MaxListed; ++i)
        text += (i ? separator : "") + items[i];
    if (items.size() > MaxListed)
        text += " and " + std::to_string(items.size() - MaxListed) + " more";
    return text;
}

// "320 -> 180/s": a step's rates.
std::string step_rates(const RateFlag &flag)
{
    return format_rate(flag.before) + " -> " + format_rate(flag.after) + "/s";
}

}

std::vector<std::string> rate_warnings(const RateAnalysis &rate, const std::string &allListed)
{
    std::vector<std::string> low, steps;
    for (const auto &flag: rate.flags)
    {
        if (flag.kind == RateFlag::Kind::LowRate)
            low.push_back(low_rate_period(flag));
        else if (flag.kind == RateFlag::Kind::Step)
            steps.push_back(format_local_time(flag.from, "%H:%M") + " (" + step_rates(flag) + ")");
    }

    // At most MaxListed items, then "and N more" and where all are.
    auto pointer = [&](const std::vector<std::string> &items)
    {
        return items.size() > MaxListed ? "; " + allListed : std::string();
    };

    std::vector<std::string> lines;
    if (!low.empty())
        lines.push_back("warning: low event rate " + first_items(low) + " (under "
                        + std::to_string(std::lround(rr::LowRateShare * 100)) + " % of the run's median rate, "
                        + format_rate(rate.median.value_or(0)) + "/s); check whether the beam was off" + pointer(low));
    if (!steps.empty())
        lines.push_back("warning: the event rate changed at " + first_items(steps) + pointer(steps));
    return lines;
}

std::vector<std::string> rate_warning_texts(const RateAnalysis &rate, const std::string &allListed)
{
    std::vector<std::string> low, steps;
    for (const auto &flag: rate.flags)
    {
        if (flag.kind == RateFlag::Kind::LowRate)
            low.push_back(low_rate_period(flag));
        else if (flag.kind == RateFlag::Kind::Step)
            steps.push_back(format_local_time(flag.from, "%H:%M") + " (" + format_rate(flag.before) + " → "
                            + format_rate(flag.after) + (steps.empty() ? " per second)" : ")"));
    }

    std::vector<std::string> texts;
    if (!low.empty())
        texts.push_back("low event rate " + first_items(low) + " (under "
                        + std::to_string(std::lround(rr::LowRateShare * 100)) + " % of the run's median rate, "
                        + format_rate(rate.median.value_or(0)) + " per second): check whether the beam was off");
    if (!steps.empty())
        texts.push_back("the event rate changed at " + first_items(steps));
    if (low.size() > MaxListed || steps.size() > MaxListed)
        texts.back() += " (" + allListed + ")";
    return texts;
}

}
