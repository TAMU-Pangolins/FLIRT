// flirt-tui's play (docs/design.md, "TUI", "Waveform view"): the frames one
// after another at the speed chosen, driven by play_tick (flirt_tui.cc calls it
// from a ticker thread every 10 ms while playing, and right after each answer
// is applied).

#include <algorithm>
#include <climits>

#include "model.h"
#include "plot_layout.h"

namespace flirt::tui
{

namespace
{

// The clock is started by the next tick, without a step.
constexpr int64_t ClockUnset = INT64_MIN;

}

int speed(const Model &m)
{
    const auto &presets = speed_presets(m.ssh);
    return presets[static_cast<size_t>(std::clamp(m.speedPreset, 0, static_cast<int>(presets.size()) - 1))];
}

void change_speed(Model &m, int delta, bool wrap)
{
    const int n = static_cast<int>(speed_presets(m.ssh).size());
    const int at = std::clamp(m.speedPreset, 0, n - 1);
    m.speedPreset = wrap ? ((at + delta) % n + n) % n : std::clamp(at + delta, 0, n - 1);
}

void pause(Model &m)
{
    m.playing = false;
    m.stepNow = false;
}

void toggle_play(Model &m)
{
    if (m.playing)
    {
        pause(m);
        return;
    }
    const auto &w = m.waves;
    if (!w.open || !w.frame.is_object() || w.frame.value("position", int64_t(-1)) < 0 || !w.error.empty())
        return;
    m.playing = true;
    const int64_t frames = w.frame.value("frames", int64_t(0));
    const int64_t position = w.wantPosition >= 0 ? w.wantPosition : w.frame["position"].get<int64_t>();
    if (position >= frames - 1 && frames > 1)
    {
        // At the last frame: from the first again, shown for one step's time.
        request_frame(m, { {"position", 0} });
        m.stepNow = false;
        m.lastStepMs = ClockUnset;
    }
    else
        m.stepNow = true;
}

bool play_tick(Model &m, int64_t nowMs)
{
    if (!m.playing)
        return false;
    const auto &w = m.waves;
    // The previous step's answer first: frames are never skipped.
    if (!w.open || waves_reading(m) || w.going)
        return false;
    const int64_t position = w.frame.is_object() ? w.frame.value("position", int64_t(-1)) : -1;
    const int64_t frames = w.frame.is_object() ? w.frame.value("frames", int64_t(0)) : 0;
    if (position < 0 || position >= frames - 1)
    {
        pause(m);
        return true;
    }
    if (m.lastStepMs == ClockUnset)
    {
        m.lastStepMs = nowMs;
        return false;
    }
    const int64_t interval = 1000 / speed(m);
    if (!m.stepNow && nowMs - m.lastStepMs < interval)
        return false;
    // On schedule: the step's time is when it was due, so the ticks' own
    // lateness (10 ms apart) does not add up; after a slow read the
    // clock restarts now (one step per answer, no catching up).
    m.lastStepMs = !m.stepNow && nowMs - m.lastStepMs < 2 * interval ? m.lastStepMs + interval : nowMs;
    m.stepNow = false;
    request_frame(m, { {"position", position + 1} });
    return false;
}

}
