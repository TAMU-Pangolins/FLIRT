#include "view.h"

#include <algorithm>

#include "text.h"
#include "view_parts.h"

namespace flirt::tui
{

using namespace ftxui;

Layout layout(int width, int height)
{
    Layout l;
    l.width = width;
    l.height = height;
    l.left = std::clamp(width * 2 / 5, 42, 72);
    l.right = std::max(4, width - l.left);
    l.top = std::max(11, height - 4);
    l.box2 = 8;
    l.box1 = l.top - l.box2;
    return l;
}

namespace
{

// Entries joined by two spaces after the leading one: the context's, the
// optional ones while they fit in `columns`, then the last ones.
std::string fit_items(const std::vector<std::string> &first, const std::vector<std::string> &optional,
                      const std::vector<std::string> &last, int columns)
{
    auto total = [](const std::vector<std::string> &items) {
        int n = 0;
        for (const auto &s: items)
            n += width(s) + 2;
        return n;
    };
    std::vector<std::string> shown = first;
    int used = 1 + total(first) + total(last) - 2;
    for (const auto &s: optional)
        if (used + width(s) + 2 <= columns)
        {
            shown.push_back(s);
            used += width(s) + 2;
        }
    shown.insert(shown.end(), last.begin(), last.end());
    std::string line;
    for (const auto &s: shown)
        line += (line.empty() ? "" : "  ") + s;
    return line;
}

}

// Which box has which edge (docs/design.md, "TUI", "Layout"): the focus cyan;
// the left box the right box shows magenta while the focus is elsewhere (a view
// in the right box: the box it was opened from); the others dim.
Edge edge_of(const Model &m, int box)
{
    if (box == m.box)
        return m.dialog == Dialog::Quit ? Edge::FocusThin : Edge::Focus;
    int chosen = 0;
    if (is_view(m.dialog))
        chosen = m.back == 1 || m.back == 2 ? m.back : chosen_box(m);
    else if (m.box == 3 || (m.wavesTab && m.box == 2))
        chosen = chosen_box(m);
    return box == chosen ? Edge::Chosen : Edge::Dim;
}

int source_box(const Model &m)
{
    switch (right_view(m))
    {
        case RightView::Browser:
        case RightView::Help:
        case RightView::Command: return 0;
        case RightView::Channels: return m.back == 1 || m.back == 2 ? m.back : chosen_box(m);
        default: return chosen_box(m);
    }
}

std::string key_bar_text(const Model &m, int columns, int rows)
{
    if (m.dialog == Dialog::Quit)
        return "Left/Right choose  Enter confirm  Esc keep converting";
    // While a line is edited the other keys type into it: only these two.
    if (m.editing && m.dialog == Dialog::None)
        return "Enter done  Esc cancel";
    if (m.waves.going && m.dialog == Dialog::None)
        return "Enter go  Esc cancel";
    if (m.waves.ranging && m.dialog == Dialog::None)
        return "Enter done  Esc cancel";
    const auto &b = m.browser;
    if (m.dialog == Dialog::Browser)
    {
        if (b.editing)
            return std::string("Enter ") + (b.zone == Browser::Zone::NewFolder ? "create" : "go") + "  Esc cancel";
        // What Enter does on the row under the cursor (browser_activate).
        const bool onFile = b.cursor >= 0 && b.cursor < static_cast<int>(b.entries.size())
            && b.entries[static_cast<size_t>(b.cursor)].type != "dir";
        std::vector<std::string> first = { "↑↓ move" };
        if (b.mode != "output")
            first.push_back("Space pick");
        first.push_back(!onFile ? "Enter open folder" : b.mode == "listfile" ? "Enter add"
                        : b.mode == "analysis" ? "Enter choose" : "Enter open");
        std::vector<std::string> optional = { "Backspace up" };
        if (b.mode == "listfile")
            optional.push_back("a add all");
        if (b.mode == "output")
            optional.push_back("n new folder");
        optional.push_back("/ path");
        optional.push_back("Tab buttons");
        return fit_items(first, optional, { "Esc close" }, columns);
    }
    if (m.dialog == Dialog::Help || m.dialog == Dialog::Command)
        return fit_items({ "↑↓ scroll" }, {}, { "Esc close", "q quit" }, columns);
    if (m.dialog == Dialog::Channels)
        return fit_items({ "↑↓←→ move", "Space pick", "a all", "n none", "Enter done", "Esc cancel" }, {}, {},
                         columns);
    // The grid on more than one page: PgUp/PgDn change the page.
    const bool pages = m.wavesTab && m.grid
        && drawn(m, plot_area(m, rows < 0 ? m.screenWidth : columns, rows < 0 ? m.screenHeight : rows)).pages > 1;
    if (m.fullPlot)
    {
        std::vector<std::string> optional = { "g go to" };
        if (pages)
        {
            // PgDn always, the other words shorter where they do not fit.
            const auto line = fit_items({ "←→ event", "Space play/pause", "- + speed", "PgDn more rows" }, optional,
                                        { "z back to the boxes", "? help" }, columns);
            if (width(line) + 1 <= columns)
                return line;
            return fit_items({ "←→ event", "Space play", "- + speed", "PgDn more rows" }, optional, { "z back", "? help" },
                             columns);
        }
        return fit_items({ "←→ event", "Space play/pause", "- + speed" }, optional, { "z back to the boxes", "? help" },
                         columns);
    }
    const std::string play = m.playing ? "Space pause" : "Space play";
    // Space plays only a frame shown (toggle_play), z and - + need a file
    // shown (play_key): without them the bar does not offer them.
    const bool canPlay = m.playing || (m.waves.open && waves_have_frame(m) && m.waves.error.empty());
    const bool shown = m.waves.open;
    auto only = [](std::vector<std::string> items, bool keep, const std::vector<std::string> &drop) {
        if (!keep)
            for (const auto &d: drop)
                items.erase(std::remove(items.begin(), items.end(), d), items.end());
        return items;
    };

    const std::string convert = busy(m) ? "s stop" : "s convert";
    std::vector<std::string> first, optional = { "Tab next box", convert };
    if (m.wavesTab)
    {
        if (m.box == 1 && m.waveFiles.empty())
        {
            // No file: the list, the frame keys and Del do nothing; o opens one.
            first = { "o open" };
            optional = { "Tab next box", convert };
        }
        else if (m.box == 1)
        {
            first = { "↑↓ file", "←→ event", play };
            optional = { "z full window", "Tab next box", "g go to", convert, "Del remove", "o open" };
        }
        else if (m.box == 2)
        {
            first = { "↑↓ move", "Space choose", "←→ event" };
            optional = { "z full window", "Tab next box", convert };
        }
        else if (!shown)
        {
            // No file shown: frame_key and play_key do nothing; Esc goes back.
            first = { "Esc back" };
            optional = { "Tab next box", convert };
        }
        else
        {
            first = { "←→ event", play, "- + speed" };
            optional = { "z full window", "g go to", "Esc back", convert };
        }
        first = only(only(first, canPlay, { play }), shown, { "- + speed" });
        optional = only(optional, shown, { "z full window" });
        if (pages)
            first.insert(first.begin(), "PgDn more rows");
        // Only box 1 has tabs here ([ ] in box 2 or the plot do nothing).
        if (m.box == 1)
            optional.push_back("[ ] tabs");
    }
    else if (m.box == 1)
    {
        first = { "a add", "Del remove", "[ ] tabs" };
        optional.push_back("l log");
    }
    else if (m.box == 2)
    {
        first = { "Enter edit", "o output folder" };
        optional.push_back("l log");
    }
    else if (m.chosen == 2)
    {
        // Space and Enter do the same on every row (input.cc activate): one
        // entry naming what they do to the row under the cursor.
        std::string word = "edit";
        switch (m.focus)
        {
            case Item::Output: word = "choose folder"; break;
            case Item::AnalysisArchive: case Item::AnalysisDefault: case Item::AnalysisFile: word = "choose"; break;
            case Item::Compression: word = "next choice"; break;
            case Item::Waveforms: case Item::HitFlags: case Item::Official: case Item::Damaged: word = "tick"; break;
            default: break;
        }
        first = { "↑↓ move", "Space/Enter " + word };
    }
    else
    {
        first = { "↑↓ scroll", "[ ] tabs" };
        const auto *f = selected_file(m);
        const auto *run = f ? run_for(m, f->path) : nullptr;
        // While the report is shown, Esc hides it (a second Esc goes back).
        const bool report = !m.logTab && run && run->value("report", json()).is_string();
        if (report && m.reportShown)
            first.push_back("r/Esc hide report");
        else
        {
            if (report)
                first.push_back("r report");
            first.push_back("Esc back");
        }
    }
    if (m.job.is_object())
        optional.push_back("c terminal command");
    // Box 1's "o output" only where it fits after the others, shown before
    // "l log": the 80-column bar stays as it was.
    if (!m.wavesTab && m.box == 1)
    {
        optional.push_back("o output");
        auto line = fit_items(first, optional, { "? help", "q quit" }, columns);
        const auto o = line.find("  o output");
        const auto l = line.find("  l log");
        if (o != std::string::npos && l != std::string::npos && l < o)
        {
            line.erase(o, 10);
            line.insert(l, "  o output");
        }
        return line;
    }
    auto line = fit_items(first, optional, { "? help", "q quit" }, columns);
    // The details, the Log and the report keep s, the screen's main action:
    // Tab next box gives way first.
    if (!m.wavesTab && m.box == 3 && m.chosen != 2 && line.find("  " + convert) == std::string::npos)
    {
        optional.erase(std::remove(optional.begin(), optional.end(), "Tab next box"), optional.end());
        line = fit_items(first, optional, { "? help", "q quit" }, columns);
    }
    return line;
}

Element render(const Model &m, int width, int height, Hits *hits)
{
    const auto l = layout(width, height);
    // With the quit dialog over the screen, only its buttons and key bar are targets.
    Hits *screenHits = m.dialog == Dialog::Quit ? nullptr : hits;
    if (m.fullPlot)
    {
        // The plot fills the screen (z).
        auto full = vbox({ full_window(m, width, height, screenHits),
                           key_bar_line(key_bar_text(m, width, height), hits) });
        if (m.dialog == Dialog::Quit)
            return dbox({ full, quit_dialog(m, hits) | clear_under | center });
        return full;
    }
    auto box1 = m.wavesTab ? waveforms_box(m, l, edge_of(m, 1), screenHits)
                           : listfiles_box(m, l, edge_of(m, 1), screenHits);
    auto box2 = m.wavesTab ? plot_options_box(m, l, edge_of(m, 2), screenHits)
                           : options_box(m, l, edge_of(m, 2), screenHits);
    auto screen = vbox({ hbox({ vbox({ box1, box2 }), right_box(m, l, edge_of(m, 3), screenHits) }),
                         bottom_box(m, l, screenHits), key_bar_line(key_bar_text(m, width, height), hits) });
    if (m.dialog == Dialog::Quit)
        return dbox({ screen, quit_dialog(m, hits) | clear_under | center });
    return screen;
}

}
