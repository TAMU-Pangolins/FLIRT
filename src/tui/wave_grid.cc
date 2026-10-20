// flirt-tui's waveform grid (docs/design.md, "TUI", "Waveform view"): one
// panel per
// channel in rows of 4, each row in its own colour, every panel with its axes
// on the one y range; the y numbers left of each row, the times under the
// page's last row. The numbers come from plot_layout.cc.

#include <algorithm>
#include <map>

#include "text.h"
#include "view_parts.h"

namespace flirt::tui
{

using namespace ftxui;

namespace
{

std::string spaces(int n)
{
    return std::string(static_cast<size_t>(std::max(0, n)), ' ');
}

bool has_trace(const Drawn &d, int channel)
{
    for (size_t i = 0; i < d.traces.size(); ++i)
        if (trace_channel(d.trace(i)) == channel)
            return true;
    return false;
}

// A panel's title, 1 + pw cells: " ch 5" (bold in the row's colour with
// traces, else dim), its mark ("▲2400", red) at the right.
Element panel_title(const Drawn &d, int channel, int pw, Color colour)
{
    const auto mark = beyond_mark(d, &channel);
    const int markWidth = width(mark);
    auto name = "ch " + std::to_string(channel);
    if (markWidth && width(name) + 1 + markWidth > pw)
        name = name.substr(0, static_cast<size_t>(std::max(0, pw - 1 - markWidth)));
    auto title = text(name);
    title = has_trace(d, channel) ? title | bold | color(colour) : title | dim;
    return hbox({ text(" "), title, text(spaces(pw - width(name) - markWidth)), text(mark) | color(Color::Red) })
        | size(WIDTH, EQUAL, 1 + pw);
}

}

std::string page_label(const Drawn &d, bool brief)
{
    if (d.onScreen.empty())
        return {};
    const auto span = std::to_string(d.onScreen.front()) + "-" + std::to_string(d.onScreen.back()) + " of "
        + std::to_string(d.panels.size());
    return brief ? span : "ch " + span;
}

Color row_color(int row)
{
    // Four colours, one per row of four channels (in the terminal's own
    // shades), again from the fifth row.
    static const Color colours[4] = { Color::BlueLight, Color::Yellow, Color::Green, Color::Magenta };
    return colours[(row % 4 + 4) % 4];
}

Element grid_element(const Drawn &d, const PlotArea &a)
{
    const auto &g = d.layout;
    const int L = g.labelWidth, pw = g.pw, ph = g.ph;
    std::map<int, int> tickAt;      // text row -> value
    for (const auto &t: y_ticks(d.y, ph))
        tickAt[t.row] = t.value;
    std::string axis = "└";
    std::vector<bool> tickColumn(static_cast<size_t>(pw), false);
    for (const auto &t: time_ticks(d.samples, pw))
        if (t.column >= 0 && t.column < pw)
            tickColumn[static_cast<size_t>(t.column)] = true;
    for (int i = 0; i < pw; ++i)
        axis += tickColumn[static_cast<size_t>(i)] ? "┴" : "─";
    PlotRanges r;
    r.xMax = d.samples - 1;
    r.yMin = d.y.lo;
    r.yMax = d.y.hi;
    const auto gap = text(spaces(g.gap));
    const int n = static_cast<int>(d.panels.size());

    Elements lines;
    for (int k = 0; k < g.rowsPerPage; ++k)
    {
        const int row = d.firstRow + k;
        if (row * 4 >= n)
            break;
        const auto colour = row_color(row);
        Elements title = { text(spaces(1 + L)) }, plots = { text(" ") }, axes = { text(spaces(1 + L)) };
        Elements labels;
        for (int y = 0; y < ph; ++y)
        {
            const auto at = tickAt.find(y);
            labels.push_back(text(at == tickAt.end() ? spaces(L) : spaces(L - 1 - width(std::to_string(at->second)))
                                  + std::to_string(at->second) + " ") | dim);
        }
        plots.push_back(vbox(labels) | size(WIDTH, EQUAL, L));
        for (int c = 0; c < 4; ++c)
        {
            if (c)
            {
                title.push_back(gap);
                plots.push_back(gap);
                axes.push_back(gap);
            }
            const int i = row * 4 + c;
            if (i >= n)
            {
                title.push_back(text(spaces(1 + pw)));
                plots.push_back(text(spaces(1 + pw)));
                axes.push_back(text(spaces(1 + pw)));
                continue;
            }
            const int channel = d.panels[static_cast<size_t>(i)];
            title.push_back(panel_title(d, channel, pw, colour));
            Elements axisColumn;
            for (int y = 0; y < ph; ++y)
                axisColumn.push_back(text(tickAt.count(y) ? "┤" : "│") | dim);
            plots.push_back(vbox(axisColumn));
            plots.push_back(trace_canvas(d, &channel, r, pw, ph, &colour, !has_trace(d, channel))
                            | size(WIDTH, EQUAL, pw) | size(HEIGHT, EQUAL, ph));
            axes.push_back(text(axis) | dim);
        }
        lines.push_back(hbox(title));
        lines.push_back(hbox(plots) | size(HEIGHT, EQUAL, ph));
        lines.push_back(hbox(axes));
    }
    // The times under the page's last row, under every column.
    Elements times = { text(spaces(1 + L)) };
    const auto numbers = time_labels(d.samples, pw);
    for (int c = 0; c < 4; ++c)
    {
        if (c)
            times.push_back(gap);
        times.push_back(text(numbers) | dim);
    }
    lines.push_back(hbox(times));
    lines.push_back(filler());
    return vbox(lines) | size(WIDTH, EQUAL, a.width);
}

}
