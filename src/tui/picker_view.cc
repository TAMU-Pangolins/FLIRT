// flirt-tui's channel picker, drawn: "<module> · N channels", the grid of the
// channels 0..max in columns of 8 ("[x]" cyan or "[ ]" dim, the number bold
// in its plot colour; a channel without traces dim), the channels without
// traces, the buttons All, None, Done, and how many are picked.

#include <algorithm>

#include "text.h"
#include "view_parts.h"

namespace flirt::tui
{

using namespace ftxui;

Element picker_inner(const Model &m, int inner, int height, Hits *hits)
{
    const auto &p = m.picker;
    const int n = p.max + 1;
    const int rows = std::max(1, std::min(8, n));
    const int columns = (n + rows - 1) / rows;
    const int numberWidth = std::max(2, width(std::to_string(std::max(0, p.max))));
    const int cellWidth = 4 + numberWidth;
    // As many columns as fit (" " then the cells, 3 cells apart); scrolled
    // to keep the cursor's column in view.
    const int fit = std::max(1, (inner - 1 + 3) / (cellWidth + 3));
    const int cursorColumn = p.cursor / rows;
    const int first = std::clamp(cursorColumn - fit + 1, 0, std::max(0, columns - fit));

    Elements lines = { text(" " + mid_short(printable(p.module), std::max(4, inner - 16)) + " · "
                            + std::to_string(n) + " channels") | dim, text("") };
    for (int r = 0; r < rows; ++r)
    {
        Elements parts = { text(" ") };
        for (int c = first; c < columns && c < first + fit; ++c)
        {
            const int ch = c * rows + r;
            if (c > first)
                parts.push_back(text("   "));
            if (ch > p.max)
            {
                parts.push_back(text(std::string(static_cast<size_t>(cellWidth), ' ')));
                continue;
            }
            auto number = std::to_string(ch);
            number = std::string(static_cast<size_t>(numberWidth - width(number)), ' ') + number;
            Element cell;
            if (!p.withTraces.count(ch))
                cell = text("[ ] " + number) | dim;
            else
            {
                const bool on = p.checked.count(ch) > 0;
                cell = hbox({ on ? text("[x]") | color(Teal) : text("[ ]") | dim, text(" "),
                              text(number) | bold | color(channel_color(ch)) });
            }
            if (ch == p.cursor && p.button < 0)
                cell = cell | bgcolor(Grey);
            parts.push_back(hit(cell, hits, { Target::Kind::PickerChannel, ch }));
        }
        lines.push_back(hbox(parts));
    }
    lines.push_back(text(""));

    std::vector<int> missing;
    for (int ch = 0; ch <= p.max; ++ch)
        if (!p.withTraces.count(ch))
            missing.push_back(ch);
    if (!missing.empty())
    {
        // "Channel 4: no traces in this module", shorter where it does not fit.
        std::string list;
        for (const int c: missing)
            list += (list.empty() ? "" : ", ") + std::to_string(c);
        const auto named = (missing.size() == 1 ? "Channel " : "Channels ") + list;
        std::string line;
        for (const auto &candidate: { named + ": no traces in this module", named + ": no traces",
                                      std::to_string(missing.size()) + " channels: no traces" })
        {
            line = candidate;
            // One cell kept on each side, as the other lines.
            if (width(line) <= inner - 2)
                break;
        }
        lines.push_back(text(" " + mid_short(line, std::max(4, inner - 2))) | dim);
        lines.push_back(text(""));
    }

    // Cyan only for the button under the cursor (as the browser's).
    Elements buttons = { text(" ") };
    const char *const labels[] = { "All", "None", "Done" };
    for (int i = 0; i < 3; ++i)
    {
        if (i)
            buttons.push_back(text("  "));
        const bool primary = p.button == i;
        buttons.push_back(hit(bar_button(labels[i], primary, true), hits, { Target::Kind::PickerButton, i }));
    }
    lines.push_back(hbox(buttons));
    lines.push_back(text(p.checked.empty() ? " none picked = all channels"
                                           : " " + std::to_string(p.checked.size()) + " picked") | dim);
    return vbox(lines) | size(HEIGHT, EQUAL, height);
}

}
