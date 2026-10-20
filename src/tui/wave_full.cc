// flirt-tui's full window (`z`): the whole screen one box, the
// plot(s) as in the right box, then the info row, the play bar and the
// controls (the key bar under it is render()'s).

#include <algorithm>

#include "text.h"
#include "view_parts.h"

namespace flirt::tui
{

using namespace ftxui;

namespace
{

std::string without_suffix(const std::string &name, const std::string &suffix)
{
    return name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0
        ? name.substr(0, name.size() - suffix.size()) : name;
}

std::string plural(size_t n, const std::string &word)
{
    return std::to_string(n) + " " + word + (n == 1 ? "" : "s");
}

// The grid's pages: "ch 0-7 (PgDn: 8-15)": the page's channels and the
// next page's (on the last page the previous one's, PgUp); empty with one page.
std::string page_note(const Drawn &d)
{
    if (!d.grid || d.pages < 2 || d.onScreen.empty())
        return {};
    auto span = [&](int page) {
        const int rows = d.layout.rowsPerPage, n = static_cast<int>(d.panels.size());
        const int first = page * rows * 4, last = std::min(n, (page + 1) * rows * 4) - 1;
        return std::to_string(d.panels[static_cast<size_t>(first)]) + "-"
            + std::to_string(d.panels[static_cast<size_t>(last)]);
    };
    const bool lastPage = d.page + 1 >= d.pages;
    return "ch " + span(d.page) + (lastPage ? " (PgUp: " + span(d.page - 1) + ")" : " (PgDn: " + span(d.page + 1) + ")");
}

// " Channels 5 6" and "event 1 · timestamp 4096 · 2 traces" (overlaid; a
// channel beyond the y range with its mark, red), or " Event 120 · 3 traces
// in 3 of 16 channels · y -100 to 400 for every channel and frame · time in ns
// · ch 0-7 (PgDn: 8-15)" (the grid; shorter forms where it does not fit):
// the event shown.
Element info_row(const Model &m, const Drawn &d, int inner, Hits *hits)
{
    const auto &w = m.waves;
    if (w.going)
        return hit(hbox({ text(" Go to event: ") | bold, text(tail_cells(w.goText, inner - 16)), text(" ") | inverted,
                          filler() }), hits, { Target::Kind::EditLine }) | size(WIDTH, EQUAL, inner);
    if (!waves_have_frame(m) || !w.error.empty())
        return text("");
    // The event's own traces (age 0).
    size_t traces = 0;
    std::vector<int> channels;
    const auto all = w.frame.find("traces");
    if (all != w.frame.end() && all->is_array())
        for (const auto &t: *all)
            if (trace_age(t) == 0)
            {
                ++traces;
                channels.push_back(trace_channel(t));
            }
    std::sort(channels.begin(), channels.end());
    channels.erase(std::unique(channels.begin(), channels.end()), channels.end());
    const auto event = std::to_string(w.frame.value("event", int64_t(0)));
    std::vector<Part> left, right;
    if (!d.grid)
    {
        left.push_back(part(" Channels"));
        for (const int c: drawn_channels(d))
        {
            left.push_back(part(" " + std::to_string(c), bold | color(channel_color(c))));
            const auto mark = beyond_mark(d, &c);
            if (!mark.empty())
                left.push_back(part(mark, color(Color::Red)));
        }
        const auto ts = w.frame.value("timestamp", json());
        right.push_back(part("event " + event + (ts.is_number() ? " · timestamp " + ts.dump() : "") + " · "
                             + plural(traces, "trace") + " ", dim));
        return note_row(left, right, &d, inner);
    }
    const bool picked = w.frame.value("channel", json()).is_array() || w.frame.value("channel", json()).is_number();
    const auto of = std::to_string(channels.size()) + " of " + std::to_string(d.panels.size())
        + (picked ? " picked" : "") + " channel" + (d.panels.size() == 1 ? "" : "s");
    const auto range = "y " + std::to_string(d.y.lo) + " to " + std::to_string(d.y.hi);
    const auto pages = page_note(d);
    const auto tail = pages.empty() ? std::string() : " · " + pages;
    const std::vector<std::string> forms = {
        " Event " + event + " · " + plural(traces, "trace") + " in " + of + " · " + range
            + " for every channel and frame · time in ns" + tail,
        " Event " + event + " · " + plural(traces, "trace") + " in " + of + " · " + range + " · time in ns" + tail,
        " Event " + event + " · " + of + " · " + range + " · ns" + tail,
        " Event " + event + " · " + range + tail };
    // The ssh note first (its long form, else its short one): the words from
    // what is left after it; with no note, the longest form that fits.
    std::string line;
    for (const bool brief: { false, true })
    {
        const auto note = ssh_note(d, brief);
        for (const auto &f: forms)
            if (line.empty() && !note.empty() && width(f) + 1 + width(note) + 4 <= inner)
                line = f;
    }
    for (const auto &f: forms)
        if (line.empty() && width(f) + 1 <= inner)
            line = f;
    if (line.empty())
        line = forms.back();
    left.push_back(part(line));
    return note_row(left, right, &d, inner);
}

}

Element full_window(const Model &m, int width_, int height, Hits *hits)
{
    const auto &w = m.waves;
    // Heavy, as the box you are in; thin under the quit dialog.
    const Edge edge = m.dialog == Dialog::Quit ? Edge::FocusThin : Edge::Focus;
    PlotArea a;
    a.full = true;
    a.width = std::max(4, width_ - 2);
    a.rows = std::max(4, height - 3);
    const auto d = drawn(m, a);
    const int inner = a.width;

    // The controls: ‹ prev  ► play  next ›  speed 5/s ... frame P of F  go to;
    // the gaps narrower where the row is short.
    const auto frame = frame_label(m);
    const std::string play = m.playing ? "‖ pause" : "► play";
    // " ‹ prev", 4, play, 4, "next ›", 6, "speed 5/s", "  frame P of F     ", "go to   ".
    const int wide = 1 + 6 + 4 + width(play) + 4 + 6 + 6 + 6 + width(speed_label(m))
        + (frame.empty() ? 0 : 2 + width(frame) + 5) + 8;
    const int gap = wide <= inner ? 4 : 2;
    Elements controls = { text(" "), hit(text("‹ prev") | bold, hits, { Target::Kind::FramePrev }),
                          text(std::string(static_cast<size_t>(gap), ' ')),
                          hit(text(play) | bold | color(Teal), hits, { Target::Kind::Play }),
                          text(std::string(static_cast<size_t>(gap), ' ')),
                          hit(text("next ›") | bold, hits, { Target::Kind::FrameNext }),
                          text(std::string(static_cast<size_t>(gap + 2), ' ')),
                          hit(hbox({ text("speed ") | dim, text(speed_label(m)) }), hits, { Target::Kind::Speed }) };
    const int used = wide - (gap == 4 ? 0 : 6);
    if (waves_reading(m) && !w.going && !m.playing)
        // "Reading..." where it fits, else "…".
        controls.push_back(text(used + 12 <= inner ? "  Reading..." : " …") | dim);
    controls.push_back(filler());
    if (!frame.empty())
        controls.push_back(text("  " + frame + "     ") | dim);
    controls.push_back(hit(text("go to") | color(Teal), hits, { Target::Kind::GoTo }));
    controls.push_back(text("   "));

    // The grid's info row above it, overlaid's under the plot.
    auto plots = plots_element(m, d, a, hits) | size(HEIGHT, EQUAL, a.region());
    auto info = info_row(m, d, inner, hits);
    auto content = d.grid ? vbox({ info, plots, bar_row(m, inner, hits), hbox(controls) | size(WIDTH, EQUAL, inner) })
                          : vbox({ plots, info, bar_row(m, inner, hits), hbox(controls) | size(WIDTH, EQUAL, inner) });

    // " <file> · <module> · <channels> ", the grid's pages ("ch 0-7 of 16", a
    // click: the next page) and "Esc back" on the right.
    std::vector<Part> right;
    if (d.grid && d.pages > 1)
    {
        auto pages = part(" " + page_label(d) + " ", dim);
        pages.e = hit(pages.e, hits, { Target::Kind::Pages });
        right.push_back(pages);
        right.push_back(part(edge_rule(edge), edge_style(edge)));
    }
    auto esc = part(" Esc", bold | color(Teal));
    esc.e = hit(hbox({ esc.e, text(" back ") | dim }), hits, { Target::Kind::Close });
    esc.width += 6;
    right.push_back(esc);
    int rightWidth = 0;
    for (const auto &p: right)
        rightWidth += p.width;

    const int module = shown_module(m);
    const std::string moduleName = module >= 0 && module < static_cast<int>(w.trees.size())
        ? printable(w.trees[static_cast<size_t>(module)].value("name", "")) : "";
    const auto channel = shown_channel(m);
    // The grid with all channels: the module's count ("16 channels").
    std::string channels = d.grid ? plural(d.panels.size(), "channel") : "all channels";
    if (channel.is_number_integer() || (channel.is_array() && channel.size() == 1))
        channels = "channel " + (channel.is_array() ? channel[0] : channel).dump();
    else if (channel.is_array())
    {
        std::vector<int> picked;
        for (const auto &c: channel)
            picked.push_back(c.get<int>());
        const auto list = channel_list(picked, 24, "channels");
        channels = list.find(',') != std::string::npos ? "channels " + list : list;
    }
    const auto tail = " · " + moduleName + " · " + channels + " ";
    const int room = std::max(4, width_ - 3 - 1 - rightWidth - 1 - width(tail) - 1);
    const auto name = mid_short(without_suffix(printable(base_name(w.file)), "_waveforms.root"), room);
    const std::vector<Part> title = { part(" " + name + tail) };

    return frame_box(width_, height - 1, edge, title, right, {}, content);
}

}
