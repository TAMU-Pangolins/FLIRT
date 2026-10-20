// flirt-tui's waveform view, drawn (docs/design.md, "TUI", "Waveform view"):
// the traces as braille polylines, the last N events per channel (older ones
// dim, as mvme fades them), the play bar and the controls, on the y range
// chosen (typical or own) and the time of the module's usual trace length, in
// the grid (wave_grid.cc) or overlaid in one plot, one colour per channel.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ftxui/dom/canvas.hpp>

#include "common/failures.h"
#include "text.h"
#include "view.h"
#include "view_parts.h"

namespace flirt::tui
{

using namespace ftxui;

namespace
{

std::string value_label(double v)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%.0f", v);
    return std::string(text) == "-0" ? "0" : text;
}

std::string spaces(int n)
{
    return std::string(static_cast<size_t>(std::max(0, n)), ' ');
}

// A line from (x0, y0) to (x1, y1) cut to [0, w] x [0, h] (Liang-Barsky);
// false if nothing of it is inside.
bool clip(double &x0, double &y0, double &x1, double &y1, double w, double h)
{
    double t0 = 0, t1 = 1;
    const double dx = x1 - x0, dy = y1 - y0;
    const double p[4] = { -dx, dx, -dy, dy };
    const double q[4] = { x0, w - x0, y0, h - y0 };
    for (int i = 0; i < 4; ++i)
    {
        if (p[i] == 0)
        {
            if (q[i] < 0)
                return false;
            continue;
        }
        const double t = q[i] / p[i];
        if (p[i] < 0)
            t0 = std::max(t0, t);
        else
            t1 = std::min(t1, t);
        if (t0 > t1)
            return false;
    }
    x1 = x0 + t1 * dx;
    y1 = y0 + t1 * dy;
    x0 = x0 + t0 * dx;
    y0 = y0 + t0 * dy;
    return true;
}

}

// The drawn traces (of one channel, or all), oldest first: the current
// event's in its colour (the channel's, or `colour`), older ones in the same
// colour dim, as mvme fades a channel's older traces from alpha 1.0 (newest)
// to 0.1 (external/mvme/src/analysis/waveform_sink_1d_widget.cc:658-669);
// values beyond the y range along its edge, samples past the time axis cut. `zero`: a dim line at 0, if 0 is in the range.
Element trace_canvas(const Drawn &d, const int *channel, const PlotRanges &r, int columns, int rows,
                     const Color *colour, bool zero)
{
    Canvas c(std::max(1, columns) * 2, std::max(1, rows) * 4);
    const double w = c.width() - 1, h = c.height() - 1;
    const double ySpan = r.yMax - r.yMin > 0 ? r.yMax - r.yMin : 1;
    auto px = [&](double x) { return x / r.xMax * w; };
    // A value beyond the range runs along its edge (cut at the edge, its
    // real peak in the title).
    auto py = [&](double y) { return (r.yMax - std::clamp(y, r.yMin, r.yMax)) / ySpan * h; };
    if (zero && r.yMin <= 0 && r.yMax >= 0)
    {
        const int y0 = static_cast<int>(std::lround(py(0)));
        c.DrawPointLine(0, y0, static_cast<int>(w), y0, [](Pixel &p) { p.dim = true; });
    }
    for (size_t k = 0; k < d.traces.size(); ++k)
    {
        const auto &trace = d.trace(k);
        const int ch = trace_channel(trace);
        if (channel && ch != *channel)
            continue;
        const auto shade = colour ? *colour : channel_color(ch);
        const bool older = trace_age(trace) > 0;
        const Canvas::Stylizer style = [shade, older](Pixel &p) {
            p.foreground_color = shade;
            p.dim = older;
        };
        const auto &samples = trace_samples(trace);
        for (size_t i = 0; i < samples.size(); ++i)
        {
            double x0 = px(static_cast<double>(i ? i - 1 : 0)), y0 = py(samples[i ? i - 1 : 0].get<double>());
            double x1 = px(static_cast<double>(i)), y1 = py(samples[i].get<double>());
            if (!clip(x0, y0, x1, y1, w, h))
                continue;
            c.DrawPointLine(static_cast<int>(std::lround(x0)), static_cast<int>(std::lround(y0)),
                            static_cast<int>(std::lround(x1)), static_cast<int>(std::lround(y1)), style);
        }
    }
    return canvas(std::move(c));
}

namespace
{

// A message where the plot goes: at its top, or centred.
Element in_plot(Element e, int columns, int rows, bool centred = false)
{
    if (centred)
        return vbox({ filler(), hbox({ filler(), e, filler() }), filler() }) | size(WIDTH, EQUAL, columns)
            | size(HEIGHT, EQUAL, rows);
    return vbox({ e, filler() }) | size(WIDTH, EQUAL, columns) | size(HEIGHT, EQUAL, rows);
}

// The y labels (the range's top and bottom) and the plot, `cw` x `rows`.
struct Labelled
{
    Element e;
    int labelWidth = 0;
};

Labelled labelled_plot(const Drawn &d, const int *channel, const PlotRanges &r, int cw, int rows)
{
    const auto top = value_label(r.yMax), bottom = value_label(r.yMin);
    Labelled out;
    out.labelWidth = std::max(width(top), width(bottom));
    const int columns = std::max(1, cw - out.labelWidth - 1);
    Elements labels;
    for (int i = 0; i < rows; ++i)
        labels.push_back(hbox({ filler(), text(i == 0 ? top : i == rows - 1 ? bottom : "") }));
    out.e = hbox({ vbox(labels) | size(WIDTH, EQUAL, out.labelWidth), text(" "),
                   trace_canvas(d, channel, r, columns, rows, nullptr, false) | size(WIDTH, EQUAL, columns)
                       | size(HEIGHT, EQUAL, rows) })
        | size(HEIGHT, EQUAL, rows);
    return out;
}

// "0 ns ... 5000.0 ns" under a labelled plot, `width` cells with the margins.
Element x_label_row(int labelWidth, const PlotRanges &r, const Drawn *note, int width)
{
    return note_row({ part(spaces(labelWidth + 2) + "0 ns") }, { part(ns_label(r.xMax) + " ") }, note, width);
}

}

Color channel_color(int channel)
{
    // 8 ANSI colours, neither black nor white nor the default; the user's
    // palette decides their shades.
    static const Color colours[8] = { Color::Red, Color::Green, Color::Yellow, Color::Blue, Color::Magenta,
                                      Color::Cyan, Color::RedLight, Color::BlueLight };
    return colours[(channel % 8 + 8) % 8];
}

int shown_module(const Model &m)
{
    // The frame drawn's, else the one asked for.
    const auto &w = m.waves;
    if (w.frame.is_object())
        for (size_t i = 0; i < w.trees.size(); ++i)
            if (w.trees[i].value("name", "") == w.frame.value("tree", ""))
                return static_cast<int>(i);
    return w.module;
}

json shown_channel(const Model &m)
{
    const auto &w = m.waves;
    return w.frame.is_object() ? w.frame.value("channel", json()) : w.channel;
}

bool waves_have_frame(const Model &m)
{
    const auto &w = m.waves;
    return w.frame.is_object() && w.frame.value("position", int64_t(-1)) >= 0;
}

std::vector<int> frame_channels(const Model &m, const PlotArea &a)
{
    if (!waves_have_frame(m) || !m.waves.error.empty())
        return {};
    return drawn_channels(drawn(m, a));
}

std::string frame_label(const Model &m)
{
    const auto &w = m.waves;
    if (!waves_have_frame(m))
        return {};
    // Thousands separated (event numbers stay plain: they are what go to
    // takes).
    return separate_thousands("frame " + std::to_string(w.frame["position"].get<int64_t>() + 1) + " of "
                              + std::to_string(w.frame.value("frames", int64_t(0))));
}

std::string ns_label(double samples)
{
    const double ns = samples * SamplePeriodNs;
    char text[32];
    if (std::fabs(ns - std::round(ns)) < 1e-9)
        std::snprintf(text, sizeof(text), "%.0f ns", ns);
    else
        std::snprintf(text, sizeof(text), "%.1f ns", ns);
    return text;
}

std::string speed_label(const Model &m)
{
    return std::to_string(speed(m)) + "/s";
}

std::string channel_list(const std::vector<int> &channels, int room, const std::string &words)
{
    std::string list;
    for (const int c: channels)
        list += (list.empty() ? "" : ", ") + std::to_string(c);
    if (width(list) <= room)
        return list;
    return std::to_string(channels.size()) + " " + words;
}

std::string ssh_note(const Drawn &d, bool brief)
{
    if (!d.lowered)
        return {};
    if (d.depth >= 2)
        return brief ? "ssh: last " + std::to_string(d.depth) : "over ssh: last " + std::to_string(d.depth) + " events";
    return brief ? "ssh: 1 event" : "over ssh: this event only";
}

int note_room(const Drawn &d, int room)
{
    // The long form if it fits the row alone, else the short one, else none;
    // with the two spaces on each side note_row keeps.
    for (const bool brief: { false, true })
    {
        const auto n = ssh_note(d, brief);
        if (!n.empty() && width(n) + 4 <= room)
            return width(n) + 4;
    }
    return 0;
}

Element note_row(const std::vector<Part> &left, const std::vector<Part> &right, const Drawn *d, int width_)
{
    int leftWidth = 0, rightWidth = 0;
    Elements out;
    for (const auto &p: left)
    {
        out.push_back(p.e);
        leftWidth += p.width;
    }
    for (const auto &p: right)
        rightWidth += p.width;
    std::string note;
    if (d)
        for (const bool brief: { false, true })
        {
            const auto n = ssh_note(*d, brief);
            if (!n.empty() && leftWidth + 2 + width(n) + 2 + rightWidth <= width_)
            {
                note = n;
                break;
            }
        }
    if (!note.empty())
    {
        // Centred, moved in to keep two spaces from each side's text.
        const int w = width(note);
        const int start = std::clamp((width_ - w) / 2, leftWidth + 2, width_ - rightWidth - 2 - w);
        out.push_back(text(spaces(start - leftWidth)));
        out.push_back(text(note) | color(Color::Yellow) | dim);
        out.push_back(text(spaces(width_ - rightWidth - start - w)));
    }
    else
        out.push_back(filler());
    for (const auto &p: right)
        out.push_back(p.e);
    return hbox(out) | size(WIDTH, EQUAL, width_);
}

Element plots_element(const Model &m, const Drawn &d, const PlotArea &a, Hits *)
{
    const auto &w = m.waves;
    const int region = a.region(), cw = std::max(4, a.width - 2);
    auto message = [&](Element e) { return hbox({ text(" "), in_plot(e, cw, region), text(" ") }); };
    const auto channel = shown_channel(m);
    if (!w.open)
        return message(text("No waveforms file chosen.") | dim);
    if (!w.error.empty())
        return message(paragraph(printable(w.error)) | color(Color::Red));
    if (w.frame.is_object() && !waves_have_frame(m))
        return message(text(w.frame.value("frames", 0) == 0 && channel.is_null() ? "No traces in this module."
                            : channel.is_array() && channel.size() > 1 ? "No traces of these channels."
                            : "No traces of this channel.") | dim);
    if (!waves_have_frame(m))
        return message(text(w.trees.empty() && !waves_reading(m) ? "" : "Reading...") | dim);
    if (d.grid)
        return grid_element(d, a) | size(HEIGHT, EQUAL, region);
    // Overlaid: one plot on the y range, the time of the usual trace length,
    // its labels; Show grid where no row fits: one line before it saying where
    // the grid shows.
    PlotRanges r;
    r.xMax = d.samples - 1;
    r.yMin = d.y.lo;
    r.yMax = d.y.hi;
    Elements parts;
    int rows = std::max(1, region - 1);
    if (d.gridChosen)
    {
        const std::string line = a.full ? " The grid needs a bigger window"
            : width(" The grid shows in the full window (z)") <= a.width ? " The grid shows in the full window (z)"
            : " Grid: z full window";
        parts.push_back(text(line) | dim);
        rows = std::max(1, rows - 1);
    }
    const auto p = labelled_plot(d, nullptr, r, cw, rows);
    parts.push_back(hbox({ text(" "), p.e, text(" ") }));
    parts.push_back(x_label_row(p.labelWidth, r, a.full ? nullptr : &d, a.width));
    return vbox(parts) | size(HEIGHT, EQUAL, region);
}

Element bar_row(const Model &m, int width_, Hits *hits)
{
    // " ●────────": the line dim, ● bold cyan at the frame's place.
    const int cells = std::max(1, width_ - 2);
    const auto &w = m.waves;
    std::string before, after;
    Elements bar;
    if (waves_have_frame(m))
    {
        const int64_t frames = w.frame.value("frames", int64_t(0));
        const int64_t position = w.wantPosition >= 0 ? w.wantPosition : w.frame["position"].get<int64_t>();
        const int at = frames <= 1 ? 0 : static_cast<int>(std::lround(static_cast<double>(position) / (frames - 1)
                                                                       * (cells - 1)));
        for (int i = 0; i < cells; ++i)
            (i < at ? before : after) += i == at ? "" : "─";
        bar = { text(before) | dim, text("●") | bold | color(Teal), text(after) | dim };
    }
    else
    {
        for (int i = 0; i < cells; ++i)
            before += "─";
        bar = { text(before) | dim };
    }
    return hbox({ text(" "), hit(hbox(bar), hits, { Target::Kind::PlayBar }), text(" ") }) | size(WIDTH, EQUAL, width_);
}

// The plot's inside: the plot(s), the play bar,
// then ‹ ► › the speed and the event on the left, "go to" on the right.
Element plot_inner(const Model &m, int inner, int height, Hits *hits)
{
    const auto &w = m.waves;
    PlotArea a;
    a.width = inner;
    a.rows = height;
    const auto d = drawn(m, a);

    Elements controls = { text(" "), hit(text("‹") | bold, hits, { Target::Kind::FramePrev }), text("  "),
                          hit(text(m.playing ? "‖" : "►") | bold | color(Teal), hits, { Target::Kind::Play }),
                          text("  "), hit(text("›") | bold, hits, { Target::Kind::FrameNext }), text("  ") };
    if (w.going)
    {
        // The event being typed in place of the speed and the event: " ‹  ►  ›  " (10 cells),
        // the prompt, the text and its cursor, " go to ".
        const int room = inner - 10 - 1 - 7;
        const std::string prompt = room - 13 >= 8 ? "Go to event: " : "Go to: ";
        controls.push_back(hit(hbox({ text(prompt) | bold, text(tail_cells(w.goText, room - width(prompt))),
                                      text(" ") | inverted }), hits, { Target::Kind::EditLine }));
    }
    else
    {
        controls.push_back(hit(text(speed_label(m)) | dim, hits, { Target::Kind::Speed }));
        controls.push_back(text("  "));
    }
    std::string event;
    if (!w.going && waves_have_frame(m))
        event = "event " + std::to_string(w.frame.value("event", int64_t(0)));
    controls.push_back(text(event));
    int used = 10 + width(speed_label(m)) + 2 + width(event) + 7;
    if (waves_reading(m) && !w.going && !m.playing)
    {
        // "Reading..." where it fits (before " go to "), else "…".
        controls.push_back(text(used + 12 <= inner ? "  Reading..." : " …") | dim);
        used += used + 12 <= inner ? 12 : 2;
    }
    controls.push_back(filler());
    // The grid: the ssh note (it has no label row of its own), then its time
    // unit (its numbers are bare), where they fit.
    if (d.grid && !w.going)
    {
        for (const bool brief: { false, true })
        {
            const auto note = ssh_note(d, brief);
            if (!note.empty() && used + width(note) + 2 <= inner)
            {
                controls.push_back(text(note + "  ") | color(Color::Yellow) | dim);
                used += width(note) + 2;
                break;
            }
        }
        if (used + 12 <= inner)
            controls.push_back(text("time in ns  ") | dim);
    }
    controls.push_back(hit(text("go to") | color(Teal), hits, { Target::Kind::GoTo }));
    controls.push_back(text(" "));
    return vbox({ plots_element(m, d, a, hits) | size(HEIGHT, EQUAL, a.region()), bar_row(m, inner, hits),
                  hbox(controls) | size(WIDTH, EQUAL, inner) });
}

}
