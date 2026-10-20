// flirt-tui's keys in the Waveforms tab (docs/design.md, "TUI", "Waveform
// view"): the frame keys of boxes 1, 2 and 3 and the full window, box 2's plot
// options (Module, Channels, Show, Y range, Traces), play and speed, the grid's
// pages, the own range typed. Frame keys are clamped to the shown frame count
// and do nothing before the first frame answer; a frame moved by hand pauses
// the play.

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "input_parts.h"
#include "plot_layout.h"
#include "view.h"

namespace flirt::tui
{

using ftxui::Event;

namespace
{

// A field of the frame shown; `otherwise` before the first frame answer.
template <typename T>
T shown(const Waves &w, const char *key, T otherwise)
{
    return w.frame.is_object() ? w.frame.value(key, otherwise) : otherwise;
}

// The frame keys' base: the position asked for last, else the one shown.
int64_t base_position(const Waves &w)
{
    return w.wantPosition >= 0 ? w.wantPosition : shown(w, "position", int64_t(-1));
}

void go_to_position(Model &m, int64_t to)
{
    auto &w = m.waves;
    const int64_t frames = shown(w, "frames", int64_t(0));
    if (frames <= 0)
        return;
    pause(m);
    to = std::max<int64_t>(0, std::min(to, frames - 1));
    if (to != base_position(w))
        request_frame(m, { {"position", to} });
}

// The shown event, for keys that keep it; else the first frame.
json keep_event(const Waves &w)
{
    const auto event = shown(w, "event", json());
    return event.is_number() ? json({ {"event", event} }) : json({ {"position", 0} });
}

void next_module(Model &m, int delta)
{
    auto &w = m.waves;
    const int n = static_cast<int>(w.trees.size());
    if (n < 2)
        return;
    pause(m);
    w.module = ((w.module + delta) % n + n) % n;
    w.channel = nullptr;
    w.page = 0;
    w.ranging = false;
    w.rangeError.clear();
    request_frame(m, keep_event(w));
}

// The next preset, wrapping.
void next_preset(int &index, size_t n)
{
    index = (index + 1) % static_cast<int>(n);
}

// Show or Traces changed: the same position again (the depth may change).
void ask_again(Model &m)
{
    const int64_t position = base_position(m.waves);
    if (position >= 0)
        request_frame(m, { {"position", position} });
}

}

void set_channels(Model &m, const json &filter)
{
    auto &w = m.waves;
    pause(m);
    w.channel = filter;
    w.page = 0;
    request_frame(m, keep_event(w));
}

void set_show(Model &m, bool grid)
{
    if (m.grid == grid)
        return;
    m.grid = grid;
    m.waves.page = 0;
    ask_again(m);
}

void set_full(Model &m, bool full)
{
    if (m.fullPlot == full)
        return;
    // Over ssh the depth asked for depends on the channels on screen (the
    // grid's page there, overlaid in the box): asked again if it changes.
    const int before = request_depth(m);
    m.fullPlot = full;
    if (m.ssh && request_depth(m) != before)
        ask_again(m);
}

bool grid_shown(const Model &m)
{
    return m.grid && drawn(m, plot_area(m, m.screenWidth, m.screenHeight)).grid;
}

// The grid's pages at the last known screen size.
int page_count(const Model &m)
{
    if (!m.grid)
        return 1;
    return drawn(m, plot_area(m, m.screenWidth, m.screenHeight)).pages;
}

void change_page(Model &m, int delta, bool wrap)
{
    auto &w = m.waves;
    const int pages = page_count(m);
    const int at = std::clamp(w.page, 0, pages - 1);
    w.page = wrap ? ((at + delta) % pages + pages) % pages : std::clamp(at + delta, 0, pages - 1);
}

bool wave_range_event(Model &m, const Event &e)
{
    auto &w = m.waves;
    bool done, cancelled;
    if (!edit_line(w.rangeText, e, false, done, cancelled))
        return true;
    if (cancelled)
    {
        w.ranging = false;
        w.rangeError.clear();
    }
    else if (done && e == Event::Return)
        finish_range(m);
    return true;
}

void choose_range(Model &m, bool own)
{
    if (!m.waves.open || !shown_tree(m))
        return;
    if (own)
        start_range(m);
    else
        choose_typical(m);
}

bool wave_going_event(Model &m, const Event &e)
{
    auto &w = m.waves;
    bool done, cancelled;
    if (!edit_line(w.goText, e, true, done, cancelled))
        return true;
    if (cancelled)
        w.going = false;
    else if (done && e == Event::Return)
    {
        w.going = false;
        if (!w.goText.empty())
            request_frame(m, { {"event", std::strtoll(w.goText.c_str(), nullptr, 10)} });
    }
    return true;
}

void step_frame(Model &m, int delta)
{
    go_to_position(m, base_position(m.waves) + delta);
}

void start_going(Model &m)
{
    auto &w = m.waves;
    if (w.open && w.frame.is_object())
    {
        pause(m);
        w.going = true;
        w.goText.clear();
    }
}

bool frame_key(Model &m, const Event &e)
{
    auto &w = m.waves;
    if (!w.open)
        return false;
    const int64_t position = base_position(w);
    if (e == Event::ArrowRight) go_to_position(m, position + 1);
    else if (e == Event::ArrowLeft) go_to_position(m, position - 1);
    else if (e == Event::PageDown && grid_shown(m)) change_page(m, 1, false);
    else if (e == Event::PageUp && grid_shown(m)) change_page(m, -1, false);
    else if (e == Event::PageDown) go_to_position(m, position + 100);
    else if (e == Event::PageUp) go_to_position(m, position - 100);
    else if (e == Event::Home) go_to_position(m, 0);
    else if (e == Event::End) go_to_position(m, shown(w, "frames", int64_t(0)) - 1);
    else if (e == Event::Character('g')) start_going(m);
    else
        return false;
    return true;
}

void bar_to(Model &m, int x, int x0, int width)
{
    const int64_t frames = shown(m.waves, "frames", int64_t(0));
    if (frames <= 0 || width <= 0)
        return;
    const int at = std::clamp(x, x0, x0 + width - 1) - x0;
    const int64_t to = width > 1 ? static_cast<int64_t>(std::llround(static_cast<double>(at) / (width - 1)
                                                                     * static_cast<double>(frames - 1))) : 0;
    pause(m);
    go_to_position(m, to);
}

bool play_key(Model &m, const Event &e)
{
    if (!m.waves.open)
        return false;
    if (e == Event::Character('-'))
        change_speed(m, -1);
    else if (e == Event::Character('+') || e == Event::Character('='))
        change_speed(m, 1);
    else if (e == Event::Character('z'))
        set_full(m, !m.fullPlot);
    else
        return false;
    return true;
}

bool full_window_event(Model &m, const Event &e, Actions &a)
{
    if (e == Event::Escape || e == Event::Character('z'))
        set_full(m, false);
    else if (e == Event::Character('?') || e == Event::Tab || e == Event::TabReverse || e == Event::Character('1')
             || e == Event::Character('2') || e == Event::Character('3') || e == Event::Character('[')
             || e == Event::Character(']'))
    {
        set_full(m, false);
        return dispatch_key(m, e, a);
    }
    else if (e == Event::Character(' '))
        toggle_play(m);
    else if (!play_key(m, e))
        frame_key(m, e);
    return true;
}

void plot_option(Model &m, int row)
{
    auto &w = m.waves;
    if (!w.open)
        return;
    switch (row)
    {
        case 0:
            next_module(m, 1);
            break;
        case 1:
            open_picker(m);
            break;
        case 2:
            set_show(m, !m.grid);
            break;
        case 3:
            // Space toggles: own opens its input line.
            choose_range(m, !own_chosen(m));
            break;
        default:
            next_preset(m.tracesPreset, traces_presets().size());
            ask_again(m);
    }
}

}
