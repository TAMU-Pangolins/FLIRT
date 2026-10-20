// flirt-tui's left boxes and the bottom box (docs/design.md, "TUI",
// "Layout"): listfiles_box, waveforms_box, options_box, plot_options_box and
// bottom_box.

#include <algorithm>
#include <cmath>
#include <map>

#include "text.h"
#include "view_parts.h"

namespace flirt::tui
{

using namespace ftxui;

namespace
{

// The name without its ending (".zip", "_waveforms.root").
std::string without(const std::string &name, const std::string &ending, bool anyCase = false)
{
    if (name.size() <= ending.size())
        return name;
    auto tail = name.substr(name.size() - ending.size());
    if (anyCase)
        std::transform(tail.begin(), tail.end(), tail.begin(), [](unsigned char c) { return std::tolower(c); });
    return tail == ending ? name.substr(0, name.size() - ending.size()) : name;
}

std::string plural(size_t n, const std::string &word)
{
    return std::to_string(n) + " " + word + (n == 1 ? "" : "s");
}

Element level_text(const std::string &s, const std::string &level, bool dimmed)
{
    if (level == "busy")
        return txt(s) | color(Teal);
    if (level.empty())
        return dimmed ? txt(s) | dim : txt(s);
    return dimmed ? txt(s) | dim : styled(txt(s), level);
}

// " <mark> <name>  <right> " rows, the right column as wide as its widest
// value; a list of `rows` rows scrolled (yframe) to `cursor`.
struct Row
{
    std::string mark, markLevel, name, right, rightLevel;
    bool rightDim = true;
    std::string folder;     // "batch/", dim before the name (a name listed twice)
};

Element list_rows(const std::vector<Row> &rows, int inner, int selected, int cursor, Bar selectedBar, Target::Kind kind,
                  Hits *hits)
{
    int rightWidth = 0;
    for (const auto &r: rows)
        rightWidth = std::max(rightWidth, width(r.right));
    const bool marks = !rows.empty() && !rows[0].mark.empty();
    const int nameWidth = std::max(4, inner - (marks ? 3 : 1) - 2 - rightWidth - 1);
    Elements out;
    for (size_t i = 0; i < rows.size(); ++i)
    {
        const auto &r = rows[i];
        const auto b = static_cast<int>(i) == selected ? selectedBar : Bar::None;
        const bool plain = b == Bar::Focus;     // the bar's own colours
        Elements parts = { text(" ") };
        if (marks)
        {
            parts.push_back(plain ? text(r.mark) : level_text(r.mark, r.markLevel, r.markLevel.empty()));
            parts.push_back(text(" "));
        }
        int nameRoom = nameWidth;
        if (!r.folder.empty())
        {
            // The folder at most half the name's room, the name the rest.
            const auto folder = mid_short(r.folder.substr(0, r.folder.size() - 1), std::max(1, nameWidth / 2 - 1)) + "/";
            parts.push_back(plain ? text(folder) : text(folder) | dim);
            nameRoom = std::max(4, nameWidth - width(folder));
        }
        parts.push_back(text(mid_short(r.name, nameRoom)));
        parts.push_back(filler());
        parts.push_back(hbox({ filler(), plain ? txt(r.right) : level_text(r.right, r.rightLevel, r.rightDim) })
                        | size(WIDTH, EQUAL, rightWidth));
        parts.push_back(text(" "));
        auto row = bar(hbox(parts) | size(WIDTH, EQUAL, inner), b);
        if (static_cast<int>(i) == cursor)
            row = row | ftxui::focus;
        out.push_back(hit(row, hits, { kind, static_cast<int>(i) }));
    }
    return vbox(out);
}

Element hint(const std::string &s, int inner)
{
    Elements lines;
    for (const auto &line: wrap(s, inner - 2))
        lines.push_back(text(" " + line) | dim);
    return vbox(lines);
}

Bar selected_bar(Edge edge)
{
    return edge == Edge::Focus ? Bar::Focus : Bar::Grey;
}

std::vector<Target> tab_targets()
{
    return { { Target::Kind::Tab, 0 }, { Target::Kind::Tab, 1 } };
}

// " Output    ~/data/rootfiles": a dim 11-cell label (not on a bar), the value.
Element kv(const std::string &label, Element value, int inner, bool onBar = false)
{
    auto name = text(" " + label + std::string(static_cast<size_t>(std::max(1, 10 - width(label))), ' '));
    return hbox({ onBar ? name : name | dim, value, filler() }) | size(WIDTH, EQUAL, inner);
}

}

RowMark row_mark(const FileEntry &f, const json *run)
{
    const auto size = format_size(f.size < 0 ? 0 : f.size);
    if (!run)
        return { "○", size, "", true };
    const auto result = run->value("result", json());
    const auto &st = run->value("status", json::object());
    if (!result.is_string())
    {
        if (st.value("level", "") != "busy")
            return { "○", size, "", true };     // waiting
        const auto percent = run->value("percent", json());
        const auto phase = run->value("phase", json());
        const std::string right = percent.is_number() ? std::to_string(percent.get<int>()) + "%"
            : phase == "finishing" ? "writing" : phase == "starting" ? "starting" : "converting";
        return { "●", right, "busy", false };
    }
    const auto r = result.get<std::string>();
    const auto seconds = format_seconds(run->value("seconds", json()));
    if (r == "converted")
        return { "✓", seconds, st.value("level", "") == "ok" ? "ok" : "warning", true };
    if (r == "skipped")
        return { "–", "skipped", "", true };
    if (r == "interrupted")
        return { "!", "stopped", "warning", false };
    if (r == "outputs-exist")
        return { "!", "exists", "warning", false };
    if (r == "unfinished")
        return { "!", "unfinished", "warning", false };
    if (r == "failed")
        return { "✗", "failed", "error", false };
    if (r == "unsupported")
        return { "✗", "unsupported", "error", false };
    return { "○", size, "", true };             // not started
}

Element listfiles_box(const Model &m, const Layout &l, Edge edge, Hits *hits)
{
    const int inner = l.left - 2;
    std::vector<Row> rows;
    for (const auto &f: m.files)
    {
        const auto mark = row_mark(f, run_for(m, f.path));
        rows.push_back({ mark.mark, mark.level, without(printable(f.name), ".zip", true), mark.right,
                         mark.level == "busy" ? "busy" : mark.rightDim ? "" : mark.level, mark.rightDim, "" });
    }
    Element content = m.files.empty() ? hint("No listfiles yet: press a or click + add", inner)
        : hit(list_rows(rows, inner, m.fileRow, m.fileCursor, selected_bar(edge), Target::Kind::FileRow, hits)
              | yframe | flex, hits, { Target::Kind::FileList });
    auto title = box_title("[1]", { "Listfiles", "Waveforms" }, 0, edge, hits, tab_targets());
    auto add = part(" + add ", color(Teal));
    add.e = hit(add.e, hits, { Target::Kind::Add });
    return hit(frame_box(l.left, l.box1, edge, title, { add }, { part(" " + plural(m.files.size(), "run") + " ", dim) },
                         content), hits, { Target::Kind::Box, 1 });
}

Element waveforms_box(const Model &m, const Layout &l, Edge edge, Hits *hits)
{
    const int inner = l.left - 2;
    std::vector<Row> rows;
    std::map<std::string, int> names;
    for (const auto &f: m.waveFiles)
        ++names[base_name(f.path)];
    for (const auto &f: m.waveFiles)
    {
        rows.push_back({ "", "", without(printable(base_name(f.path)), "_waveforms.root"),
                         f.size < 0 ? "" : format_size(f.size), "", true, "" });
        // Two files of the same name: each row starts with its folder's name.
        const auto slash = f.path.rfind('/');
        if (names[base_name(f.path)] > 1 && slash != std::string::npos && slash > 0)
            rows.back().folder = printable(base_name(f.path.substr(0, slash))) + "/";
    }
    Element content = m.waveFiles.empty()
        ? hint("No waveforms files yet: convert runs, or press o or click + open", inner)
        : hit(list_rows(rows, inner, m.waveRow, m.waveCursor, selected_bar(edge), Target::Kind::WaveRow, hits)
              | yframe | flex, hits, { Target::Kind::WaveList });
    auto title = box_title("[1]", { "Listfiles", "Waveforms" }, 1, edge, hits, tab_targets());
    auto open = part(" + open ", color(Teal));
    open.e = hit(open.e, hits, { Target::Kind::Add });
    return hit(frame_box(l.left, l.box1, edge, title, { open },
                         { part(" " + plural(m.waveFiles.size(), "file") + " ", dim) }, content),
               hits, { Target::Kind::Box, 1 });
}

Element options_box(const Model &m, const Layout &l, Edge edge, Hits *hits)
{
    const int inner = l.left - 2;
    const auto &o = m.opts;
    const int valueWidth = std::max(4, inner - 12);
    auto output = m.outputDir.empty() ? text("not chosen") | dim : text(mid_short(tilde(m, m.outputDir), valueWidth));
    const std::string analysis = o.analysis == "archive" ? "from the listfile"
        : o.analysis == "default-filters" ? "default filters"
        : o.analysisPath.empty() ? "no file chosen" : base_name(o.analysisPath);
    const int changed = o.official + o.acceptDamaged + (o.jobs != "1") + (o.threads != "1")
        + (o.experimentName != "Experiment") + !o.experimentTitle.empty() + (o.ditherOffset != "0")
        + (o.compression != default_compression().name);
    Elements rows = {
        kv("Output", output, inner),
        kv("Analysis", text(mid_short(analysis, valueWidth)), inner),
        kv("Writes", text(mid_short(o.waveforms ? (o.hitFlags ? "waveforms + flags" : "waveforms file")
                                                : (o.hitFlags ? "flags, no waveforms" : "no waveforms file"),
                                    valueWidth)), inner),
        kv("Advanced", text(changed ? std::to_string(changed) + " changed" : "defaults"), inner) };
    auto title = box_title("[2]", { "Options" }, 0, edge);
    return hit(frame_box(l.left, l.box2, edge, title, {}, {},
                         hit(vbox(rows) | flex, hits, { Target::Kind::Summary })),
               hits, { Target::Kind::Box, 2 });
}

Element plot_options_box(const Model &m, const Layout &l, Edge edge, Hits *hits)
{
    // Module, Channels, Show, Y range, Traces.
    const int inner = l.left - 2;
    const int valueWidth = std::max(4, inner - 12);
    const auto &w = m.waves;
    const int module = shown_module(m);
    const auto channel = shown_channel(m);
    auto onBar = [&](int row) { return m.plotRow == row && edge == Edge::Focus; };     // the bar's own colours
    Element moduleValue = text("");
    if (module >= 0 && module < static_cast<int>(w.trees.size()))
    {
        const auto count = " (" + std::to_string(module + 1) + " of " + std::to_string(w.trees.size()) + ")";
        moduleValue = text(mid_short(printable(w.trees[static_cast<size_t>(module)].value("name", "")),
                                     std::max(4, inner - 12 - width(count))) + count);
    }
    else if (w.open)
        moduleValue = text("reading...");

    // Channels: "all (5, 6)" (the channels drawn, when it fits), else "all";
    // picked: "5, 6", else "8 picked".
    const auto area = plot_area(m, l.width, l.height);
    std::string channels = "all";
    if (channel.is_array() || channel.is_number_integer())
    {
        std::vector<int> picked;
        for (const auto &c: channel.is_array() ? channel : json::array({ channel }))
            picked.push_back(c.get<int>());
        channels = channel_list(picked, valueWidth, "picked");
    }
    else
    {
        std::string list;
        for (const int c: frame_channels(m, area))
            list += (list.empty() ? "" : ", ") + std::to_string(c);
        if (!list.empty() && width("all (" + list + ")") <= valueWidth)
            channels = "all (" + list + ")";
    }

    auto mark = [&](int row, bool on, const std::string &yes, const std::string &no) {
        auto e = text(on ? yes : no);
        return onBar(row) ? e : on ? e | color(Teal) : e | dim;
    };
    auto gridChoice = hit(hbox({ mark(2, m.grid, "(•)", "( )"), text(" grid") }), hits, { Target::Kind::ShowChoice, 0 });
    auto overlaid = hit(hbox({ mark(2, !m.grid, "(•)", "( )"), text(" overlaid") }), hits,
                        { Target::Kind::ShowChoice, 1 });

    // Y range: "(•) typical -100 to 400  ( ) own 0 to 8000", shorter where it
    // does not fit: the other one's numbers left out, then the chosen one's word.
    const bool own = own_chosen(m);
    const auto *tree = shown_tree(m);
    const auto typical = typical_range(tree ? *tree : json());
    std::string ownNumbers;
    if (tree)
    {
        const auto found = w.own.find(tree->value("name", ""));
        if (found != w.own.end() && found->second.hi > found->second.lo)
            ownNumbers = std::to_string(found->second.lo) + " to " + std::to_string(found->second.hi);
    }
    const auto typicalNumbers = std::to_string(typical.lo) + " to " + std::to_string(typical.hi);
    struct Form
    {
        std::string typical, own;
    };
    auto with = [](const std::string &word, const std::string &numbers) {
        return numbers.empty() ? word : word.empty() ? numbers : word + " " + numbers;
    };
    const std::vector<Form> forms = {
        { with("typical", w.open && tree ? typicalNumbers : ""), with("own", ownNumbers) },
        { with("typical", own ? "" : typicalNumbers), with("own", own ? ownNumbers : "") },
        { own ? "typical" : typicalNumbers, own ? ownNumbers : "own" } };
    Form form = forms.back();
    for (const auto &f: forms)
        if (width("(•) " + f.typical + "  ( ) " + f.own) <= valueWidth)
        {
            form = f;
            break;
        }
    Element range;
    if (w.ranging)
        range = hit(hbox({ text("own: ") | bold, text(tail_cells(w.rangeText, valueWidth - 6)), text(" ") | inverted }),
                    hits, { Target::Kind::EditLine });
    else
        range = hbox({ hit(hbox({ mark(3, !own, "(•)", "( )"), text(" " + form.typical) }), hits,
                           { Target::Kind::RangeChoice, 0 }),
                       text("  "),
                       hit(hbox({ mark(3, own, "(•)", "( )"), text(" " + form.own) }), hits,
                           { Target::Kind::RangeChoice, 1 }) });
    const int traces = traces_setting(m);
    const std::string tracesText = traces == 1 ? "1 per channel" : "last " + std::to_string(traces) + " per channel";

    Elements rows = { kv("Module", moduleValue, inner, onBar(0)),
                      kv("Channels", text(channels), inner, onBar(1)),
                      kv("Show", hbox({ gridChoice, text("  "), overlaid }), inner, onBar(2)),
                      kv("Y range", range, inner, onBar(3)),
                      kv("Traces", text(tracesText), inner, onBar(4)) };
    for (size_t i = 0; i < rows.size(); ++i)
    {
        const auto b = static_cast<int>(i) == m.plotRow ? selected_bar(edge) : Bar::None;
        rows[i] = hit(bar(rows[i], b), hits, { Target::Kind::PlotOption, static_cast<int>(i) });
    }
    // The last row: why an own range was refused (red), or how the typical
    // range was chosen when it could not be measured.
    if (!w.rangeError.empty())
        rows.push_back(text(" " + mid_short(w.rangeError, inner - 2)) | color(Color::Red));
    else if (w.open && !own && !typical.note.empty() && tree)
        rows.push_back(text(" " + mid_short(typical.note, inner - 2)) | dim);
    auto title = box_title("[2]", { "Plot options" }, 0, edge);
    return hit(frame_box(l.left, l.box2, edge, title, {}, {}, vbox(rows) | flex), hits, { Target::Kind::Box, 2 });
}

namespace
{

// "━━━━───": done cyan, the rest dim.
Element progress_bar(double progress, int cells)
{
    cells = std::max(0, cells);
    const int done = std::clamp(static_cast<int>(std::floor(progress * cells + 0.5)), 0, cells);
    std::string a, b;
    for (int i = 0; i < done; ++i)
        a += "━";
    for (int i = done; i < cells; ++i)
        b += "─";
    return hbox({ text(a) | color(Teal), text(b) | dim });
}

std::string percent_text(double progress)
{
    auto s = std::to_string(static_cast<int>(std::floor(100 * progress + 0.5))) + "%";
    return std::string(static_cast<size_t>(std::max(0, 5 - width(s))), ' ') + s;
}

}

Element bottom_box(const Model &m, const Layout &l, Hits *hits)
{
    const int inner = l.width - 2;
    const auto reason = convert_reason(m);
    const bool running = m.job.is_object() && m.job.value("state", "") == "running";
    const bool stopping = m.job.is_object() && m.job.value("state", "") == "stopping";
    auto convert = [&] {
        return hit(bar_button(convert_label(m), true, reason.empty()), hits, { Target::Kind::Convert });
    };
    // The text beside Convert fits the room left after " Convert N runs   "
    // and one cell of margin, so the button is never cut (FTXUI shrinks every
    // child of a too-wide hbox); a path in it keeps its start and end.
    const int besideRoom = std::max(4, inner - 1 - (width(convert_label(m)) + 2) - 3 - 1);
    auto beside = [&](const std::string &s) { return text(mid_short(printable(s), besideRoom)); };
    // Why Convert is disabled, or why it did not start.
    auto why = [&]() -> Element {
        if (!m.goError.empty())
            return beside(m.goError) | color(Color::Red);
        return beside(reason) | dim;
    };

    std::vector<Part> title, right;
    Element row;
    if (!m.job.is_object())
    {
        int64_t bytes = 0;
        for (const auto &f: m.files)
            bytes += std::max<int64_t>(0, f.size);
        title = { part(" Ready ", bold) };
        right = { part(" " + plural(m.files.size(), "run") + (m.files.empty() ? "" : " · " + format_size(bytes)) + " ",
                       dim) };
        Elements parts = { text(" "), convert(), text("   ") };
        if (reason.empty() && m.goError.empty())
        {
            const int room = std::max(4, inner - 1 - width(convert_label(m)) - 2 - 3 - 5 - 1);
            parts.push_back(text("into ") | dim);
            parts.push_back(text(start_short(printable(tilde(m, m.outputDir)), room)));
        }
        else
            parts.push_back(why());
        row = hbox(parts);
    }
    else
    {
        // job.cc's headline: the title up to its first " · ", the rest on the right.
        const auto headline = m.job.value("headline", json::object());
        const auto text_ = printable(headline.value("text", ""));
        const auto level = headline.value("level", "");
        const std::string separator = " · ";
        const auto dot = text_.find(separator);
        const Decorator style = [level](Element e) { return styled(e | bold, level); };
        title = { part(" " + text_.substr(0, dot) + " ", style) };
        // The right part ("about 50 s left") only while it runs: the elapsed
        // time is on the line under it, during and after.
        if (dot != std::string::npos && (running || stopping))
            right = { part(" " + text_.substr(dot + separator.size()) + " ", dim) };
        const double progress = m.job.value("progress", 0.0);
        const auto percent = percent_text(progress);
        const auto elapsed = printable(m.job.value("elapsedText", ""));
        if (running || stopping)
        {
            const std::string stop = stopping ? "Stopping..." : "[ Stop ]";
            const int cells = inner - 1 - 5 - 2 - width(elapsed) - 3 - width(stop) - 1;
            auto stopPart = stopping ? text(stop) : hit(text(stop) | color(Color::Red) | bold, hits,
                                                        { Target::Kind::Convert });
            row = hbox({ text(" "), progress_bar(progress, cells), text(percent), text("  " + elapsed) | dim, filler(),
                         stopPart, text(" ") });
        }
        else
        {
            const auto usage = m.job.value("usageError", json());
            Elements parts = { text(" "), convert(), text("   ") };
            if (!reason.empty() || !m.goError.empty())
                parts.push_back(why());
            else if (usage.is_string())
                parts.push_back(beside(usage.get<std::string>()) | color(Color::Red));
            else
            {
                const int cells = inner - 1 - (width(convert_label(m)) + 2) - 3 - 5 - 2 - width(elapsed) - 1;
                parts.push_back(progress_bar(progress, cells));
                parts.push_back(text(percent));
                parts.push_back(text("  " + elapsed) | dim);
            }
            row = hbox(parts);
        }
    }
    return frame_box(l.width, 3, Edge::Dim, title, right, {}, row | size(WIDTH, EQUAL, inner));
}

}
