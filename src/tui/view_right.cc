// flirt-tui's right box (docs/design.md, "TUI", "Layout"): a run's Details
// and the Log, the options form, the plot, and the views that open over them
// (the browser, the help, the command).

#include <algorithm>
#include <climits>

#include "text.h"
#include "view_parts.h"

namespace flirt::tui
{

using namespace ftxui;

RightView right_view(const Model &m)
{
    if (m.dialog == Dialog::Browser)
        return RightView::Browser;
    if (m.dialog == Dialog::Help)
        return RightView::Help;
    if (m.dialog == Dialog::Command)
        return RightView::Command;
    if (m.dialog == Dialog::Channels)
        return RightView::Channels;
    if (m.wavesTab)
        return RightView::Plot;
    if (m.chosen == 2)
        return RightView::Form;
    return m.logTab ? RightView::Log : RightView::Details;
}

std::vector<Line> right_lines(const Model &m, int width)
{
    switch (right_view(m))
    {
        case RightView::Details: return detail_lines(m, width);
        case RightView::Log: return log_lines(m, width);
        case RightView::Help: return help_lines(m, width);
        case RightView::Command: return command_lines(m, width);
        default: return {};
    }
}

int *right_scroll(Model &m)
{
    switch (right_view(m))
    {
        case RightView::Details: return &m.detailScroll;
        case RightView::Log: return &m.logScroll;
        case RightView::Help: return &m.helpScroll;
        case RightView::Command: return &m.commandScroll;
        default: return nullptr;
    }
}

namespace
{

// Lines from `scroll` on (clamped), each with a one-cell margin.
Element lines_inner(const std::vector<Line> &lines, int scroll, int rows)
{
    scroll = std::max(0, std::min(scroll, static_cast<int>(lines.size()) - rows));
    Elements shown;
    for (int i = scroll; i < static_cast<int>(lines.size()) && i < scroll + rows; ++i)
    {
        const auto &line = lines[static_cast<size_t>(i)];
        if (line.label > 0)
        {
            const auto c = cells(line.text);
            std::string label, rest;
            for (size_t k = 0; k < c.size(); ++k)
                (static_cast<int>(k) < line.label ? label : rest) += c[k];
            // The label blue; on a warning or an error line its level's colour.
            auto name = line.level == "warning" || line.level == "error" ? styled(text(label), line.level)
                                                                         : text(label) | color(LabelBlue);
            shown.push_back(hbox({ text(" "), name, styled(text(rest), line.level) }));
        }
        else if (line.run > 0)
        {
            // The Log's "<run>: " blue as the Details' labels (grey was hard
            // to notice), the rest in its line's level.
            const auto c = cells(line.text);
            std::string prefix, rest;
            for (size_t k = 0; k < c.size(); ++k)
                (static_cast<int>(k) < line.run ? prefix : rest) += c[k];
            shown.push_back(hbox({ text(" "), text(prefix) | color(LabelBlue), styled(text(rest), line.level) }));
        }
        else
            shown.push_back(hbox({ text(" "), styled(text(line.text), line.level) }));
    }
    return vbox(shown);
}

// ---------- the options form ----------

// A text field: "[ value ]" on grey (a child's colours win over its row's
// bar, so not on the bar), the edit line while it is edited.
Element field(const Model &m, Item item, int fieldWidth, bool onBar)
{
    Element value;
    if (focused(m, item) && m.editing)
    {
        const auto shown = tail_cells(m.editText, fieldWidth - 1);
        value = hbox({ text(shown), text(" ") | inverted, filler() }) | size(WIDTH, EQUAL, fieldWidth);
    }
    else
    {
        const auto &v = field_value(m, item);
        value = text(width(v) > fieldWidth ? mid_short(v, fieldWidth) : printable(v)) | size(WIDTH, EQUAL, fieldWidth);
    }
    auto e = hbox({ text("[ "), value, text("]") });
    return onBar ? e : e | bgcolor(Grey);
}

Element form_inner(const Model &m, int inner, int rows, Edge edge, Hits *hits)
{
    const auto &o = m.opts;
    const bool lock = busy(m);
    const int cw = inner - 2;
    Elements lines;
    // An item's line: the whole line its target, the focused one a bar.
    auto item = [&](Item it, Element e) {
        const bool at = focused(m, it);
        auto row = bar(e | size(WIDTH, EQUAL, inner), at ? (edge == Edge::Focus ? Bar::Focus : Bar::Grey) : Bar::None);
        if (at)
            row = row | ftxui::focus;
        lines.push_back(hit(row, hits, { Target::Kind::Item, 0, it }));
    };
    auto plainAt = [&](Item it) { return focused(m, it) && edge == Edge::Focus; };
    auto mark = [&](Item it, const std::string &on, const std::string &off, bool value) {
        auto e = text(value ? on : off);
        return plainAt(it) ? e : value ? e | color(Teal) : e | dim;
    };
    auto heading = [&](const std::string &s) { lines.push_back(text(" " + s) | bold); };

    auto choose = text("[ Choose... ]");
    item(Item::Output, hbox({ text(" Output folder") | bold, filler(), plainAt(Item::Output) ? choose : choose | bgcolor(Grey),
                              text(" ") }));
    lines.push_back(m.outputDir.empty() ? text("   Choose where the ROOT files go") | dim
                                        : text("   " + start_short(printable(tilde(m, m.outputDir)), cw - 2)));
    heading("Analysis");
    auto radio = [&](Item it, const std::string &label, bool on) {
        item(it, hbox({ text("   "), mark(it, "(•)", "( )", on), text(" " + label), filler() }));
    };
    radio(Item::AnalysisArchive, "From the listfile", o.analysis == "archive");
    radio(Item::AnalysisDefault, "Default filters", o.analysis == "default-filters");
    radio(Item::AnalysisFile, o.analysis == "file" && !o.analysisPath.empty()
          ? mid_short(base_name(o.analysisPath), cw - 6) : "Another file...", o.analysis == "file");
    lines.push_back(text(""));
    auto check = [&](Item it, const std::string &indent, const std::string &label, bool on) {
        item(it, hbox({ text(indent), mark(it, "[x]", "[ ]", on), text(" " + label), filler() }));
    };
    check(Item::Waveforms, " ", "Write waveforms file", o.waveforms);
    check(Item::HitFlags, " ", "Add pile-up/overflow flags", o.hitFlags);
    if (o.hitFlags)
        for (const auto &line: wrap("A flag column for each amplitude. The whole numbers stay as mvme's; the random "
                                    "part differs from files made without.", cw - 2))
            lines.push_back(text("   " + line));
    heading("Advanced");
    check(Item::Official, "   ", "Official waveform branches", o.official);
    check(Item::Damaged, "   ", "Accept damaged input", o.acceptDamaged);
    if (o.acceptDamaged)
        for (const auto &line: wrap("The ROOT files may then lack data or contain wrong data; the report lists the "
                                    "failed checks.", cw - 2))
            lines.push_back(text("   " + line) | color(Color::Yellow));
    // The compression: every name of compressions() on one row, the chosen
    // one teal (plain on the focus bar, bold there), each name a target.
    {
        Elements names = { text("   Compression  ") };
        const auto &all = compressions();
        for (size_t i = 0; i < all.size(); ++i)
        {
            if (i)
                names.push_back(text(" · "));
            auto name = text(all[i].name);
            if (o.compression == all[i].name)
                name = plainAt(Item::Compression) ? name | bold : name | bold | color(Teal);
            names.push_back(hit(name, hits, { Target::Kind::CompressionChoice, static_cast<int>(i) }));
        }
        names.push_back(filler());
        item(Item::Compression, hbox(names));
    }
    auto textField = [&](Item it, const std::string &label, int fieldWidth) {
        item(it, hbox({ text("   " + label), field(m, it, fieldWidth, plainAt(it)), filler() }));
    };
    const int textWidth = std::clamp(inner - 24, 8, 40);
    textField(Item::Jobs, "Conversions at once    ", 3);
    textField(Item::Threads, "Threads per conversion ", 3);
    textField(Item::Name, "Experiment name  ", textWidth);
    textField(Item::Title, "Experiment title ", textWidth);
    // The dither offset, or while the search runs its progress in place of
    // the field; under it, in the field's column (it belongs to the field),
    // "[ From a file… ]", while searching "[ Stop search ]"; then the
    // search's note or why it found none.
    const auto &f = m.find;
    const std::string ditherLabel = "   Dither offset    ";
    if (f.running)
        item(Item::Dither, hbox({ text(ditherLabel), text("searching… " + std::to_string(f.percent) + "%"),
                                  filler() }));
    else
        textField(Item::Dither, "Dither offset    ", 10);
    {
        auto find = text(f.running ? "[ Stop search ]" : "[ From a file… ]");
        if (!plainAt(Item::FindDither))
            find = f.running ? find | color(Teal) : find | bgcolor(Grey);
        if (f.running)
            find = hit(find, hits, { Target::Kind::FindStop });
        item(Item::FindDither, hbox({ text(std::string(static_cast<size_t>(width(ditherLabel)), ' ')), find, filler() }));
    }
    if (!f.error.empty())
        for (const auto &line: wrap(f.error, cw - 2))
            lines.push_back(text("   " + printable(line)) | color(Color::Red));
    else if (!f.note.empty())
        for (const auto &line: wrap(f.note, cw - 2))
            lines.push_back(text("   " + line) | color(Color::Yellow));
    auto form = vbox(lines) | yframe | size(HEIGHT, EQUAL, rows);
    return lock ? form | dim : form;
}

// "Esc close" on a view's edge.
std::vector<Part> close_label(Hits *hits)
{
    auto esc = part(" Esc", bold | color(Teal));
    esc.e = hit(hbox({ esc.e, text(" close ") | dim }), hits, { Target::Kind::Close });
    esc.width += 7;
    return { esc };
}

std::vector<Part> single_title(const std::string &name, Edge edge)
{
    return box_title("[3]", { name }, 0, edge);
}

int width_of(const std::vector<Part> &parts)
{
    int n = 0;
    for (const auto &p: parts)
        n += p.width;
    return n;
}

// "[3] Listfiles → Details ─ Log": after the number, the left box box 3
// shows, by its active tab's name in that box's edge colour, and a dim "→";
// left out when the title, one rule cell and `right` do not fit in a box `w`
// wide with it.
std::vector<Part> with_source(const Model &m, std::vector<Part> title, int w, const std::vector<Part> &right)
{
    const int box = source_box(m);
    if (!box || title.size() < 2)
        return title;
    const std::string name = box == 1 ? (m.wavesTab ? "Waveforms" : "Listfiles")
                                      : (m.wavesTab ? "Plot options" : "Options");
    const std::vector<Part> source = { part(name, edge_style(edge_of(m, box))), part(" → ", dim) };
    if (3 + width_of(title) + width_of(source) + (right.empty() ? 0 : 1 + width_of(right) + 1) > w)
        return title;
    title.insert(title.begin() + 2, source.begin(), source.end());
    return title;
}

}

Element right_box(const Model &m, const Layout &l, Edge edge, Hits *hits)
{
    const int inner = l.right - 2, rows = l.rightRows(), cw = l.rightText();
    std::vector<Part> title, right, bottom;
    Element content;
    switch (right_view(m))
    {
        case RightView::Details:
        case RightView::Log:
            title = with_source(m, box_title("[3]", { "Details", "Log" }, m.logTab ? 1 : 0, edge, hits,
                                             { { Target::Kind::Tab, 2 }, { Target::Kind::Tab, 3 } }), l.right, {});
            content = lines_inner(right_lines(m, cw), m.logTab ? m.logScroll : m.detailScroll, rows);
            break;
        case RightView::Form:
            title = with_source(m, single_title("Details", edge), l.right, {});
            content = form_inner(m, inner, rows, edge, hits);
            break;
        case RightView::Plot:
        {
            PlotArea a;
            a.width = inner;
            a.rows = rows;
            const auto d = drawn(m, a);
            // The button last (as z): " z full window " (the key bar's
            // words), or " full " where the box is too narrow for it. It
            // depends on the box's width only, never on
            // the frame: the long one where its room still holds a
            // two-channel legend of two-digit channels (" ch 13 ch 14") and
            // the pages' short form (" 25-31 of 31 ", shown if shorter than
            // the room, as below).
            // Before it, the grid's pages "ch 0-7 of 16" (the next page), or
            // overlaid the legend: each channel drawn, bold in its colour, as
            // many as fit.
            title = single_title("Plot", edge);
            auto full = part(" z full window ", color(Teal));
            if (l.right - 3 - width_of(title) - 1 - (full.width + 1) <= width(" 25-31 of 31 "))
                full = part(" full ", color(Teal));
            full.e = hit(full.e, hits, { Target::Kind::Full });
            int room = l.right - 3 - width_of(title) - 1 - (full.width + 1);
            if (d.grid && d.pages > 1)
            {
                // At least one "─" before it (else the short form, else none).
                auto pages = part(" " + page_label(d) + " ", dim);
                if (pages.width >= room)
                    pages = part(" " + page_label(d, true) + " ", dim);
                pages.e = hit(pages.e, hits, { Target::Kind::Pages });
                if (pages.width < room)
                    right.push_back(pages);
            }
            else if (!d.grid)
            {
                // Overlaid: the legend, a channel beyond the y range with its
                // mark (red, "▲2400"); the grid names its channels in its panels.
                Part legend{ nullptr, 0 };
                Elements chs;
                for (const int c: frame_channels(m, a))
                {
                    const auto label = " ch " + std::to_string(c);
                    const auto mark = beyond_mark(d, &c);
                    if (width(label) + width(mark) + 2 > room)
                        break;
                    chs.push_back(text(label) | bold | color(channel_color(c)));
                    if (!mark.empty())
                        chs.push_back(text(mark) | color(Color::Red));
                    legend.width += width(label) + width(mark);
                    room -= width(label) + width(mark);
                }
                if (!chs.empty())
                {
                    chs.push_back(text(" ") | edge_style(edge));
                    legend.e = hbox(chs);
                    legend.width += 1;
                    right.push_back(legend);
                }
            }
            // The button only with a file shown (z and its click need one).
            if (!right.empty() && m.waves.open)
                right.push_back(part(edge_rule(edge), edge_style(edge)));
            if (m.waves.open)
                right.push_back(full);
            // The source only where it fits beside them: the legend and the pages win.
            title = with_source(m, title, l.right, right);
            const auto frame = frame_label(m);
            if (!frame.empty())
                bottom = { part(" " + frame + " ", dim) };
            content = plot_inner(m, inner, rows, hits);
            break;
        }
        case RightView::Channels:
            right = close_label(hits);
            title = with_source(m, single_title("Channels", edge), l.right, right);
            content = picker_inner(m, inner, rows, hits);
            break;
        case RightView::Browser:
            title = single_title(browser_title(m), edge);
            right = close_label(hits);
            content = browser_inner(m, inner, rows, hits);
            break;
        case RightView::Help:
        case RightView::Command:
            title = single_title(m.dialog == Dialog::Help ? "Help" : "Terminal command", edge);
            right = close_label(hits);
            content = lines_inner(right_lines(m, cw), m.dialog == Dialog::Help ? m.helpScroll : m.commandScroll, rows);
            break;
    }
    return hit(frame_box(l.right, l.top, edge, title, right, bottom, content), hits, { Target::Kind::Box, 3 });
}

}
