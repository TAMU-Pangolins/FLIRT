// flirt-tui's channel picker, its keys and buttons (docs/design.md, "TUI",
// "Waveform view"): a view in the
// right box opened from box 2's Channels row. ↑↓←→ move over the grid (in
// columns of 8 channels) and down onto the buttons, Space picks or presses,
// `a` all, `n` none, Enter is Done (none picked = all channels, automatic),
// Esc cancels.

#include <algorithm>

#include "input_parts.h"
#include "view.h"

namespace flirt::tui
{

using ftxui::Event;

namespace
{

// The grid's rows: 8, fewer when there are fewer channels.
int grid_rows(const Picker &p)
{
    return std::max(1, std::min(8, p.max + 1));
}

void done(Model &m)
{
    // None checked: all, automatic (null); else exactly the sorted set, even
    // every channel with traces (a plot per channel, "no trace" where empty).
    auto &p = m.picker;
    json filter;
    if (!p.checked.empty())
    {
        filter = json::array();
        for (const int c: p.checked)
            filter.push_back(c);
    }
    m.dialog = Dialog::None;
    set_channels(m, filter);
}

}

void open_picker(Model &m)
{
    auto &w = m.waves;
    const auto channels = w.frame.is_object() ? w.frame.value("channels", json::array()) : json::array();
    if (!channels.is_array() || channels.empty() || w.module < 0 || w.module >= static_cast<int>(w.trees.size()))
        return;
    Picker p;
    p.module = w.trees[static_cast<size_t>(w.module)].value("name", "");
    for (const auto &c: channels)
        if (c.is_number_integer() && c.get<int>() >= 0)
        {
            p.withTraces.insert(c.get<int>());
            p.max = std::max(p.max, c.get<int>());
        }
    if (p.max < 0)
        return;
    // From the current filter: all = every channel with traces checked.
    if (w.channel.is_array())
    {
        for (const auto &c: w.channel)
            if (c.is_number_integer() && p.withTraces.count(c.get<int>()))
                p.checked.insert(c.get<int>());
    }
    else if (w.channel.is_number_integer() && p.withTraces.count(w.channel.get<int>()))
        p.checked.insert(w.channel.get<int>());
    // All (null): none checked ("none picked = all channels").
    p.cursor = p.checked.empty() ? *p.withTraces.begin() : *p.checked.begin();
    m.picker = p;
    m.dialog = Dialog::Channels;
}

void picker_toggle(Model &m, int channel)
{
    auto &p = m.picker;
    if (!p.withTraces.count(channel))
        return;     // no traces in this module: cannot be picked
    if (!p.checked.erase(channel))
        p.checked.insert(channel);
}

void picker_button(Model &m, int button)
{
    auto &p = m.picker;
    if (button == 0)
        p.checked = p.withTraces;
    else if (button == 1)
        p.checked.clear();
    else
        done(m);
}

bool picker_event(Model &m, const Event &e)
{
    auto &p = m.picker;
    const int rows = grid_rows(p);
    const int row = p.cursor % rows;
    if (e == Event::Escape)
        m.dialog = Dialog::None;    // cancelled: nothing changes
    else if (e == Event::Return)
        done(m);
    else if (e == Event::Character('a'))
        picker_button(m, 0);
    else if (e == Event::Character('n'))
        picker_button(m, 1);
    else if (e == Event::Character(' '))
    {
        if (p.button >= 0)
            picker_button(m, p.button);
        else
            picker_toggle(m, p.cursor);
    }
    else if (p.button >= 0)
    {
        if (e == Event::ArrowUp) p.button = -1;
        else if (e == Event::ArrowLeft) p.button = std::max(0, p.button - 1);
        else if (e == Event::ArrowRight) p.button = std::min(2, p.button + 1);
    }
    else if (e == Event::ArrowUp)
    {
        if (row > 0)
            --p.cursor;
    }
    else if (e == Event::ArrowDown)
    {
        if (row < rows - 1 && p.cursor < p.max)
            ++p.cursor;
        else
            p.button = 0;
    }
    else if (e == Event::ArrowLeft)
    {
        if (p.cursor - rows >= 0)
            p.cursor -= rows;
    }
    else if (e == Event::ArrowRight)
    {
        // The next column's row, or its last channel.
        if (p.cursor / rows < p.max / rows)
            p.cursor = std::min(p.max, p.cursor + rows);
    }
    return true;
}

}
