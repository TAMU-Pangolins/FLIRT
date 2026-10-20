// flirt-tui's views in the right box: the lines of the
// details, the log, the help and the command, the file browser; and the quit
// dialog over the screen.

#include <algorithm>
#include <ctime>
#include <map>
#include <set>
#include <strings.h>

#include "text.h"
#include "view.h"
#include "view_parts.h"

namespace flirt::tui
{

using namespace ftxui;

namespace
{

std::string format_mtime(int64_t mtime)
{
    const time_t t = static_cast<time_t>(mtime);
    struct tm local;
    localtime_r(&t, &local);
    char text[32];
    std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M", &local);
    return text;
}

// 1204377 -> "1,204,377".
std::string thousands(uint64_t n)
{
    auto digits = std::to_string(n);
    for (int i = static_cast<int>(digits.size()) - 3; i > 0; i -= 3)
        digits.insert(static_cast<size_t>(i), ",");
    return digits;
}

std::string text_of(const json &j, const char *key)
{
    const auto v = j.is_object() ? j.value(key, json()) : json();
    return v.is_string() ? v.get<std::string>() : std::string();
}

// The label column of the details (10 cells with the margin).
constexpr int Label = 9;

// A labeled value: the label in its column (blue, or its level's colour in
// warning and error: lines_inner), the value wrapped at spaces beside it, its
// other lines under the value. Every fact has its own label; an empty label
// only for the next item of a list whose items start with their own name (the
// events, the modules per event).
void labeled(std::vector<Line> &out, int inner, const std::string &label, const std::string &value,
             const std::string &level = "")
{
    const std::string indent(Label, ' ');
    const bool own = width(label) >= Label;
    if (own)
        out.push_back({ label, level, width(label) });
    const auto lines = wrap(value, std::max(10, inner - Label));
    for (size_t i = 0; i < lines.size(); ++i)
    {
        const bool first = i == 0 && !own && !label.empty();
        out.push_back({ (first ? label + std::string(static_cast<size_t>(Label - width(label)), ' ') : indent) + lines[i],
                        level, first ? Label : 0 });
    }
}

// A count's share of a channel's hits, one decimal: "0.8%", "<0.1%" when
// not zero but too small to show.
std::string percent_text(uint64_t part, uint64_t whole)
{
    const double percent = whole ? 100.0 * double(part) / double(whole) : 0.0;
    if (part && percent < 0.05)
        return "<0.1%";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f%%", percent);
    return buf;
}

// A run converted with --extract-hit-flags: per flag source of its report's
// "hitFlags" (src/flirt/hit_flags.cc), a heading and a table of the channels
// with hits: ch, hits, then each flag's count and share of the hits (SCP:
// pile-up, over/underflow, both; QDC: overflow). The column names blue as
// the labels, the numbers in the default colour. Where the table with the
// shares beside the counts ("205 (0.8%)") is wider than the box, the shares
// get a line of their own under the counts and "over/underflow" two header
// lines.
void hit_flag_lines(const json &hitFlags, int width, std::vector<Line> &out)
{
    const auto sources = hitFlags.value("sources", json::array());
    std::map<std::string, int> perModule;
    for (const auto &s: sources)
        ++perModule[text_of(s, "module")];
    for (const auto &s: sources)
    {
        if (!s.is_object())
            continue;
        const bool pileUp = s.count("pileUp") > 0;
        const auto module = text_of(s, "module"), original = text_of(s, "original");
        // The source's own name only where a module has several (QDC long and short).
        const auto dot = original.rfind('.');
        const auto which = perModule[module] > 1 ? ", " + original.substr(dot == std::string::npos ? 0 : dot + 1)
                                                 : std::string();
        out.push_back({});
        for (const auto &line: wrap(printable(std::string(pileUp ? "Pile-up and over/underflow" : "Overflow") + "   "
                                              + module + " (" + text_of(s, "type") + ")" + which), width))
            out.push_back({ line, "heading" });

        // The columns: header, then per channel the counts and the shares.
        const std::vector<std::pair<std::string, std::string>> flags = pileUp
            ? std::vector<std::pair<std::string, std::string>>{ { "pile-up", "pileUp" },
                                                                { "over/underflow", "overUnderflow" },
                                                                { "both", "both" } }
            : std::vector<std::pair<std::string, std::string>>{ { "overflow", "overflow" } };
        struct Row { std::vector<std::string> counts, shares; };
        std::vector<Row> rows;
        const auto channels = s.value("channels", json::array());
        for (const auto &c: channels.is_array() ? channels : json::array())
        {
            const auto num = [&](const std::string &key) {
                const auto v = c.is_object() ? c.value(key, json()) : json();
                return v.is_number_integer() && v.get<int64_t>() >= 0 ? v.get<uint64_t>() : uint64_t(0);
            };
            const auto hits = num("hits");
            if (!hits)
                continue;
            Row r;
            r.counts = { std::to_string(num("channel")), thousands(hits) };
            r.shares = { "", "" };
            for (const auto &f: flags)
            {
                r.counts.push_back(thousands(num(f.second)));
                r.shares.push_back(percent_text(num(f.second), hits));
            }
            rows.push_back(r);
        }
        std::vector<std::string> header = { "ch", "hits" };
        for (const auto &f: flags)
            header.push_back(f.first);

        // One line per channel, "205 (0.8%)", if that fits.
        const size_t n = header.size();
        auto widths_for = [&](bool together, const std::vector<std::string> &head) {
            std::vector<int> w(n);
            for (size_t i = 0; i < n; ++i)
            {
                w[i] = flirt::tui::width(head[i]);
                for (const auto &r: rows)
                    w[i] = std::max(w[i], together && i >= 2 ? flirt::tui::width(r.counts[i] + " (" + r.shares[i] + ")")
                                                             : std::max(flirt::tui::width(r.counts[i]),
                                                                        flirt::tui::width(r.shares[i])));
            }
            return w;
        };
        auto total = [](const std::vector<int> &w) {
            int t = 0;
            for (const auto x: w)
                t += x + 1;
            return t - 1;
        };
        auto line_of = [&](const std::vector<std::string> &cellsIn, const std::vector<int> &w) {
            std::string line;
            for (size_t i = 0; i < n; ++i)
            {
                const auto pad = std::max(0, w[i] - flirt::tui::width(cellsIn[i]));
                line += (i ? " " : "") + std::string(static_cast<size_t>(pad), ' ') + cellsIn[i];
            }
            return line;
        };
        auto widths = widths_for(true, header);
        if (total(widths) <= width)
        {
            const auto head = line_of(header, widths);
            out.push_back({ head, "", flirt::tui::width(head) });
            for (const auto &r: rows)
            {
                std::vector<std::string> cellsOut = r.counts;
                for (size_t i = 2; i < n; ++i)
                    cellsOut[i] = r.counts[i] + " (" + r.shares[i] + ")";
                out.push_back({ line_of(cellsOut, widths), "" });
            }
            continue;
        }
        // Narrow: "over/underflow" on two header lines, the shares under the counts.
        std::vector<std::string> top = header, bottom(n);
        for (size_t i = 0; i < n; ++i)
            if (header[i] == "over/underflow")
            {
                top[i] = "over/";
                bottom[i] = "underflow";
            }
        std::vector<std::string> widest = top;
        for (size_t i = 0; i < n; ++i)
            if (flirt::tui::width(bottom[i]) > flirt::tui::width(widest[i]))
                widest[i] = bottom[i];
        widths = widths_for(false, widest);
        for (const auto &h: { top, bottom })
        {
            if (std::all_of(h.begin(), h.end(), [](const std::string &x) { return x.empty(); }))
                continue;
            const auto head = line_of(h, widths);
            out.push_back({ head, "", flirt::tui::width(head) });
        }
        for (const auto &r: rows)
        {
            out.push_back({ line_of(r.counts, widths), "" });
            out.push_back({ line_of(r.shares, widths), "" });
        }
    }
}

struct Lines
{
    std::vector<Line> out;
    int width;

    void add(const std::string &text, const std::string &level = "")
    {
        for (auto &line: wrap(text, width))
            out.push_back({ line, level });
    }
    void blank() { out.push_back({}); }
    // A key and what it does: the key in 15 cells, the words wrapped beside it.
    void key(const std::string &k, const std::string &does)
    {
        constexpr int Keys = 15;
        const auto lines = wrap(does, std::max(10, width - Keys));
        for (size_t i = 0; i < lines.size(); ++i)
            out.push_back({ (i ? std::string(Keys, ' ') : k + std::string(static_cast<size_t>(std::max(1, Keys - width_of(k))), ' '))
                            + lines[i], "" });
    }
    static int width_of(const std::string &s) { return flirt::tui::width(s); }
};

std::string lower_first(std::string s)
{
    if (!s.empty() && s[0] >= 'A' && s[0] <= 'Z')
        s[0] = static_cast<char>(s[0] - 'A' + 'a');
    return s;
}

// The browser's buttons, short enough for the right box.
std::string short_label(const std::string &label)
{
    if (label.rfind("Add all ", 0) == 0)
        return "Add all";
    if (label.rfind("Add ", 0) == 0)
        return label.substr(0, label.size() - std::string(" selected").size());
    if (label == "Choose this folder")
        return "Choose";
    return label;
}

std::string plural(size_t n, const std::string &word)
{
    return std::to_string(n) + " " + word + (n == 1 ? "" : "s");
}

}

std::vector<Line> detail_lines(const Model &m, int width)
{
    Lines l{ {}, std::max(10, width) };
    const auto *f = selected_file(m);
    if (!f)
    {
        l.add("Add listfiles to see their details here.", "muted");
        return l.out;
    }
    // The name, its Size, the last job's part
    // (Status, why, its warnings and notes, the files).
    const auto *run = run_for(m, f->path);
    l.add(f->name, "heading");
    labeled(l.out, l.width, "Size", format_size(f->size < 0 ? 0 : f->size));
    if (run)
    {
        auto status = lower_first(run->at("status").value("text", ""));
        const auto seconds = format_seconds(run->value("seconds", json()));
        if (run->value("result", json()) == "converted" && !seconds.empty())
            status += " in " + seconds;
        labeled(l.out, l.width, "Status", status);
        const auto &st = run->at("status");
        if (st.value("detail", json()).is_string())
            labeled(l.out, l.width, "Why", st["detail"].get<std::string>(), st.value("level", ""));
        // A bad end's to-do and its check, after why (src/common/failures.h).
        if (st.value("todo", json()).is_string())
            labeled(l.out, l.width, "To do", st["todo"].get<std::string>());
        if (st.value("check", json()).is_string())
            labeled(l.out, l.width, "Check", st["check"].get<std::string>(), "muted");
        // Only the warnings and notes: the Log tab has every message.
        for (const auto &msg: run->at("messages"))
        {
            const auto text = msg.is_string() ? msg.get<std::string>() : std::string();
            if (text.rfind("warning: ", 0) == 0)
                labeled(l.out, l.width, "Warning", text.substr(9), "warning");
            else if (text.rfind("note: ", 0) == 0)
                labeled(l.out, l.width, "Note", text.substr(6));
        }
        // The kinds among the outputs (src/ui/job.cc, the "result" event).
        std::string kinds;
        for (const auto &[suffix, kind]: { std::pair<std::string, std::string>{ "_raw.root", "raw" },
                                           { "_histos.root", "histos" }, { "_waveforms.root", "waveforms" },
                                           { "_hit_flags.analysis", "flags analysis" },
                                           { ".flirt.json", "report" } })
            for (const auto &p: run->at("outputs"))
            {
                const auto path = p.is_string() ? p.get<std::string>() : std::string();
                if (path.size() >= suffix.size() && path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0)
                {
                    kinds += (kinds.empty() ? "" : ", ") + kind;
                    break;
                }
            }
        if (!kinds.empty())
            labeled(l.out, l.width, "Files", kinds);
        // Converted with --extract-hit-flags: the counts from the report (src/ui/job.cc).
        const auto hitFlags = run->value("hitFlags", json());
        if (hitFlags.is_object())
            hit_flag_lines(hitFlags, l.width, l.out);
    }
    const auto report = run ? run->value("report", json()) : json();
    if (report.is_string())
    {
        const auto path = report.get<std::string>();
        l.blank();
        if (!m.reportShown)
            labeled(l.out, l.width, "Report", "press r to show it", "muted");
        else
        {
            // Its lines after it are the report itself, as the file holds them;
            // its name only where it fits whole in the value column.
            const auto name = base_name(path);
            labeled(l.out, l.width, "Report", (Lines::width_of(name) <= l.width - Label ? name : std::string("below"))
                    + " (r or Esc hides it)", "heading");
            const auto error = m.reportErrors.find(path);
            const auto lines = m.reports.find(path);
            if (error != m.reportErrors.end())
                l.add(error->second, "error");
            else if (lines != m.reports.end())
                for (const auto &line: lines->second)
                    l.add(line);
        }
    }
    return l.out;
}

std::string command_intro(const std::string &host)
{
    return "To convert the same way without flirt-tui, run this in a terminal"
        + (host.empty() ? std::string() : " on " + printable(host));
}

LogStyle log_style(const std::string &line, const LogStyle &before)
{
    // flirt-convert prints a block per run (README "What
    // flirt-convert prints"): a header "<mark> <run>   <outcome>" by its mark,
    // the run's name blue; rows "    <label>   <text>" (warning, note,
    // quality, lost yellow; why and to do in their header's level); wrapped
    // lines as the line they continue; the end's "  <mark> <run>   <reason>"
    // lines and "note"/"quality" rows the same way.
    static const std::vector<std::pair<std::string, std::string>> marks = {
        { "✓ ", "ok" }, { "– ", "ok" }, { "! ", "warning" }, { "✗ ", "error" }, { "● ", "muted" } };
    static const std::set<std::string> labels = { "run", "events", "dither", "files", "lost", "quality", "flags",
                                                  "note", "warning", "why", "to do" };
    LogStyle b;
    b.block = before.block;
    const size_t lead = line.find_first_not_of(' ');
    if (lead == std::string::npos)
        return b;   // a blank line ends a block's wrapped text
    const std::string body = line.substr(lead);
    if (lead == 0 || lead == 2)
        for (const auto &[mark, level]: marks)
            if (body.rfind(mark, 0) == 0)
            {
                const auto end = body.find("   ", mark.size());
                const auto name = body.substr(mark.size(), end == std::string::npos ? std::string::npos
                                                                                     : end - mark.size());
                if (end == std::string::npos || name.empty() || name.find(' ') != std::string::npos)
                    break;  // not "<mark> <run>   ..."
                b.level = b.row = level;
                b.prefix = width(line.substr(0, lead + mark.size() + name.size()));
                if (lead == 0)
                    b.block = level;
                return b;
            }
    // A row: the label in its column (4 cells in a block, 0 for the end's
    // note and quality), its text 10 cells after.
    if (lead == 0 || lead == 4)
        for (const auto &label: labels)
            if (body.size() > 10 && body.rfind(label, 0) == 0
                && body.find_first_not_of(' ', label.size()) == 10 && (lead == 4 || label == "note" || label == "quality"))
            {
                if (label == "warning" || label == "note" || label == "quality" || label == "lost")
                    b.level = "warning";
                else if (label == "why" || label == "to do")
                    b.level = before.block == "error" || before.block == "warning" ? before.block : "";
                b.row = b.level;
                b.value = static_cast<int>(lead) + 10;
                return b;
            }
    // A wrapped line (indented 4 cells or more): as the line it continues.
    if (lead >= 4)
    {
        b.level = b.row = before.row;
        return b;
    }

    // The other lines (the watch's and the stop's own lines, and the
    // "<run>: " lines of older logs): the level of what follows the run's
    // name; everything else default (docs/design.md, "TUI").
    static const std::vector<std::pair<std::string, std::string>> starts = {
        { "converted (status ok)", "ok" }, { "skipped", "ok" },
        { "note:", "warning" }, { "data quality", "warning" }, { "lost packets", "warning" },
        { "warning:", "warning" },
        { "error:", "error" }, { "failed", "error" }, { "refused", "error" }, { "unsupported", "error" },
        { "interrupted", "error" } };
    LogStyle s;
    // "<run>: ": a name without spaces before the first ": " that is not a
    // word flirt-convert starts its own lines with.
    const auto colon = line.find(": ");
    static const std::set<std::string> own = { "error", "warning", "note", "refused", "paused", "resumed", "Done" };
    if (colon != std::string::npos && colon > 0 && line.find(' ') > colon && !own.count(line.substr(0, colon)))
        s.prefix = width(line.substr(0, colon + 2));
    const auto rest = s.prefix ? line.substr(colon + 2) : line;
    // Progress: "20%", "90% (batch: about 5 s left)".
    size_t digits = 0;
    while (digits < rest.size() && rest[digits] >= '0' && rest[digits] <= '9')
        ++digits;
    if (digits && digits < rest.size() && rest[digits] == '%'
        && (digits + 1 == rest.size() || rest.compare(digits + 1, 8, " (batch:") == 0))
    {
        s.level = "muted";
        return s;
    }
    for (const auto &[start, level]: starts)
        if (rest.rfind(start, 0) == 0)
        {
            s.level = level;
            return s;
        }
    // A failed check under "failed checks:" ("  I2-listfile-entry: fail: ...").
    if (rest.find(": fail: ") != std::string::npos)
        s.level = "error";
    return s;
}

std::vector<Line> log_lines(const Model &m, int width)
{
    Lines l{ {}, std::max(10, width) };
    const auto log = m.job.is_object() ? m.job.value("log", json::array()) : json::array();
    if (log.empty())
        l.add("No output from flirt-convert yet. The log shows its last 200 lines.", "muted");
    LogStyle style;
    for (const auto &line: log)
    {
        // Each line coloured by its kind, the run's name blue.
        const auto text = line.get<std::string>();
        style = log_style(text, style);
        const auto first = l.out.size();
        // A block's row (flirt-convert leaves them unwrapped for flirt-tui):
        // its text wrapped under its value column, every line in the row's
        // colour.
        if (style.value > 0 && style.value < Lines::width_of(text))
        {
            const auto c = cells(text);
            std::string head, value;
            for (size_t k = 0; k < c.size(); ++k)
                (static_cast<int>(k) < style.value ? head : value) += c[k];
            const auto parts = wrap(value, std::max(10, l.width - style.value));
            for (size_t i = 0; i < parts.size(); ++i)
                l.out.push_back({ (i ? std::string(static_cast<size_t>(style.value), ' ') : head) + parts[i],
                                  style.level });
            continue;
        }
        l.add(text, style.level);
        if (l.out.size() > first)
            l.out[first].run = std::min(style.prefix, Lines::width_of(l.out[first].text));
    }
    return l.out;
}

std::vector<Line> command_lines(const Model &m, int width)
{
    Lines l{ {}, std::max(10, width) };
    if (!m.job.is_object())
    {
        l.add("No conversion has run yet.", "muted");
        return l.out;
    }
    // "Terminal command" (plainer than "Command" alone);
    // flirt_tui.cc prints the same words at the end.
    l.add(command_intro(m.host) + ":", "heading");
    l.add(m.job.value("commandLine", ""));
    l.blank();
    l.add(m.mouse ? "To copy it, hold Shift while you select it (⌥ Option in iTerm2; ⌘R in macOS Terminal); "
                    "flirt-tui also prints it when it ends."
                  : "flirt-tui prints it again when it ends.", "muted");
    return l.out;
}

std::vector<Line> help_lines(const Model &m, int width)
{
    Lines l{ {}, std::max(10, width) };
    // What FLIRT stands for (its name spelled out) and does.
    l.add("FLIRT " + m.version, "heading");
    l.add("Fast Listfile Interpretation and ROOT Translation");
    l.blank();
    l.add("FLIRT turns mvme's MVLC listfiles (.zip) into the ROOT files that official mvme and mvme_root_client "
          "make from them, without running mvme. This screen picks the runs, runs the conversion (flirt-convert) "
          "and shows how it goes. Original listfiles are only ever "
          "read: FLIRT never changes, moves or deletes them.");
    l.blank();
    if (!m.host.empty() || !m.user.empty())
    {
        l.add("Running" + (m.host.empty() ? std::string() : " on " + printable(m.host))
              + (m.user.empty() ? std::string() : " as " + printable(m.user)) + ".");
        l.blank();
    }
    l.add("Everywhere", "heading");
    l.key("Tab, Shift-Tab", "the next, previous box");
    l.key("1, 2, 3", "box 1, 2 or 3");
    l.key("[, ]", "the tabs of the box you are in");
    l.key("s", "convert, or stop the running conversion");
    l.key("c", "the terminal command for the last conversion");
    l.key("l", "the log (Listfiles)");
    l.key("?", "this help");
    l.key("q, Ctrl-C", "quit (asks first while a conversion runs)");
    l.blank();
    l.add("Listfiles (box 1)", "heading");
    l.key("↑↓", "choose a run; the right box shows its details");
    l.key("PgUp, PgDn", "10 runs up, down");
    l.key("Home, End", "the first, last run");
    l.key("Enter", "into its details");
    l.key("Del, Backspace", "take the run off the list (not while converting)");
    l.key("a", "add listfiles");
    l.key("o", "choose the output folder");
    l.blank();
    l.add("Options (box 2)", "heading");
    l.key("Enter", "change them, in the right box");
    l.blank();
    l.add("Details and Log (box 3)", "heading");
    l.key("↑↓", "scroll; PgUp, PgDn, Home, End too");
    l.key("[, ]", "Details or Log");
    l.key("r", "show or hide the report, if the run has one");
    l.key("Esc", "hide the report; else back to box 1");
    l.blank();
    l.add("The options (box 3)", "heading");
    l.key("↑↓", "move");
    l.key("Space, Enter", "act on the row under the cursor: tick a box, choose, edit a field (Enter or Tab keeps "
                          "it, Esc cancels), or open the folder browser (Choose...)");
    l.key("Flags", "Add pile-up/overflow flags: a column per amplitude, 0 clean, 1 over/underflow, 2 pile-up, "
                   "3 both (QDC: 1 overflow); Details shows the counts");
    {
        // From common/compression.cc's table.
        std::string choices;
        for (const auto &c: compressions())
            choices += (choices.empty() ? "" : "; ") + std::string(c.name) + ": " + c.gives;
        l.key("Compression", "the ROOT files' compression (" + choices + "); the values are the same with each, "
                             "and ROOT reads them all");
    }
    l.key("Esc", "back to box 2");
    l.blank();
    l.add("The file browser", "heading");
    l.key("↑↓", "move");
    l.key("Space", "pick a file");
    l.key("Enter", "open a folder; add a listfile (with the ones picked), choose a file");
    l.key("Backspace", "the folder above");
    l.key("/, ~", "type a path");
    l.key("a", "add all in this folder (listfiles)");
    l.key("n", "a new folder (output folder)");
    l.key("Tab", "the path, the buttons");
    l.key("Esc", "close");
    l.blank();
    if (m.mouse)
    {
        l.add("Mouse", "heading");
        l.key("Click", "goes there: a box, a tab, a row; acts on an option, a tick box, a button, the keys at "
                       "the bottom");
        l.key("Double-click", "as Enter: a run's details, box 2's options; in the browser, open a "
                              "folder, add or choose a file");
        l.key("Wheel", "scroll the box under it");
        l.add("To select text, hold Shift while you drag (⌥ Option in iTerm2; in macOS Terminal, ⌘R turns the "
              "mouse off and on), or start flirt-tui with --no-mouse. Tested in tmux; other terminals not yet "
              "checked.", "muted");
    }
    else
        l.add("The mouse is off (--no-mouse).", "muted");
    l.blank();
    l.add("Settings (the output folder, the analysis, waveforms, flags and the last folders browsed) are remembered "
          "in settings/flirt-tui.json in the FLIRT folder, shared by everyone running this copy (FLIRT_TUI_SETTINGS "
          "names another file); the listfiles and the Advanced options are not.", "muted");
    return l.out;
}

std::string browser_title(const Model &m)
{
    static const std::map<std::string, std::string> titles = {
        {"listfile", "Add listfiles"}, {"analysis", "Analysis file"}, {"output", "Output folder"} };
    const auto found = titles.find(m.browser.mode);
    return found == titles.end() ? "Browser" : found->second;
}

Element browser_inner(const Model &m, int inner, int height, Hits *hits)
{
    const auto &b = m.browser;
    const int cw = std::max(4, inner - 2);
    Elements top;

    // The folder, its last name bold (~ for home); or the path being typed.
    if (b.editing && b.zone == Browser::Zone::Path)
        top.push_back(hit(hbox({ text(" "), text(tail_cells(b.editText, cw - 1)), text(" ") | inverted, filler() }),
                          hits, { Target::Kind::BrowserPath }));
    else
    {
        auto shown = start_short(printable(tilde(m, b.path)), cw);
        const auto slash = shown.rfind('/');
        const auto folder = slash == std::string::npos || slash + 1 == shown.size() ? std::string() : shown.substr(0, slash + 1);
        auto row = hbox({ text(" "), text(folder) | dim, text(shown.substr(folder.size())) | bold, filler() });
        top.push_back(hit(bar(row, b.zone == Browser::Zone::Path ? Bar::Focus : Bar::None), hits,
                          { Target::Kind::BrowserPath }));
    }
    if (!b.error.empty())
        for (const auto &line: wrap(b.error, cw))
            top.push_back(text(" " + line) | color(Color::Red));

    // The entries: " [x] name   size " (with the date where it fits).
    const bool marks = b.mode != "output";
    const bool dates = inner >= 60;
    std::vector<std::string> rights;
    int rightWidth = 0;
    for (const auto &e: b.entries)
    {
        const bool added = b.mode == "listfile" && e.type != "dir" && is_added(m, e.id);
        rights.push_back(e.type == "dir" ? "" : added ? "added" : format_size(e.size));
        rightWidth = std::max(rightWidth, width(rights.back()));
    }
    const int nameWidth = std::max(4, inner - 1 - (marks ? 4 : 0) - 2 - rightWidth - (dates ? 18 : 0) - 1);
    Elements rows;
    for (size_t i = 0; i < b.entries.size(); ++i)
    {
        const auto &e = b.entries[i];
        const bool isDir = e.type == "dir";
        const bool added = b.mode == "listfile" && !isDir && is_added(m, e.id);
        const bool picked = b.picked.count(e.id) > 0;
        const auto rowBar = static_cast<int>(i) != b.cursor ? Bar::None
            : b.zone == Browser::Zone::List && !b.editing ? Bar::Focus : Bar::Grey;
        const bool plain = rowBar == Bar::Focus;
        Elements parts = { text(" ") };
        if (marks)
        {
            std::string mark = "   ";
            if (!isDir && b.mode == "listfile")
                mark = picked || added ? "[x]" : "[ ]";
            else if (!isDir)
                mark = picked ? "(•)" : "( )";
            parts.push_back(plain || added ? text(mark) : picked ? text(mark) | color(Teal) : text(mark) | dim);
            parts.push_back(text(" "));
        }
        // Listfiles without ".zip", as box 1 shows them.
        auto shownName = printable(e.name);
        if (b.mode == "listfile" && !isDir && shownName.size() > 4
            && strcasecmp(shownName.c_str() + shownName.size() - 4, ".zip") == 0)
            shownName.resize(shownName.size() - 4);
        auto name = text(mid_short(shownName + (isDir ? "/" : ""), nameWidth));
        parts.push_back(e.up ? (plain ? name : name | dim) : isDir ? name | bold : name);
        parts.push_back(filler());
        parts.push_back(hbox({ filler(), plain || added ? text(rights[i]) : text(rights[i]) | dim })
                        | size(WIDTH, EQUAL, rightWidth));
        // "../" has no date (its mtime is not read: 0).
        if (dates)
        {
            const auto date = e.up ? std::string(18, ' ') : "  " + format_mtime(e.mtime);
            parts.push_back(plain ? text(date) : text(date) | dim);
        }
        parts.push_back(text(" "));
        auto row = bar(hbox(parts), rowBar);
        if (added && !plain)
            row = row | dim;
        if (static_cast<int>(i) == b.cursor)
            row = row | ftxui::focus;
        rows.push_back(hit(row, hits, { Target::Kind::BrowserRow, static_cast<int>(i) }));
    }
    Element list;
    // Nothing but "../": it, then the words.
    if (rows.size() == 1 && b.entries[0].up)
    {
        const std::string none = b.mode == "output" ? "No folders here." : b.mode == "listfile"
            ? "No listfiles (.zip) or folders here." : "No .analysis files or folders here.";
        Elements lines = { rows[0] };
        for (const auto &line: wrap(none, cw))
            lines.push_back(text(" " + line) | dim);
        list = vbox(lines) | flex;
    }
    else if (rows.empty())
    {
        const std::string none = b.mode == "output" ? "No folders here." : b.mode == "listfile"
            ? "No listfiles (.zip) or folders here." : "No .analysis files or folders here.";
        Elements lines;
        for (const auto &line: wrap(none, cw))
            lines.push_back(text(" " + line) | dim);
        list = vbox(lines) | flex;
    }
    else
        list = vbox(rows) | yframe | flex;

    Elements low;
    if (b.zone == Browser::Zone::NewFolder)
        low.push_back(hit(hbox({ text(" New folder: ") | bold, text(tail_cells(b.editText, cw - 13)),
                                 text(" ") | inverted, filler() }), hits, { Target::Kind::EditLine }));
    low.push_back(text(""));
    Elements buttons = { text(" ") };
    const auto labels = browser_buttons(m);
    for (size_t i = 0; i < labels.size(); ++i)
    {
        const auto &label = labels[i];
        bool enabled = true;
        if (label.rfind("Add all ", 0) == 0)
            enabled = addable_count(m) > 0;
        else if (label.rfind("Add ", 0) == 0 || label == "Choose")
            enabled = !b.picked.empty();
        // Cyan only for the button you are on (Tab): a primary look without
        // the focus read as selected.
        const bool primary = b.zone == Browser::Zone::Buttons && b.button == static_cast<int>(i);
        if (i)
            buttons.push_back(text("  "));
        buttons.push_back(hit(bar_button(short_label(label), primary, enabled), hits,
                              { Target::Kind::BrowserButton, static_cast<int>(i) }));
    }
    low.push_back(hbox(buttons));
    size_t files = 0;
    for (const auto &e: b.entries)
        files += e.type != "dir";
    const std::string count = b.mode == "listfile"
        ? plural(files, "listfile") + " here · " + std::to_string(b.picked.size()) + " picked"
        : b.mode == "output" ? plural(b.entries.size() - (b.hasParent ? 1 : 0), "folder") + " here"
        : plural(files, ".analysis file") + " here";
    low.push_back(text(" " + count) | dim);

    Elements all = top;
    all.push_back(list);
    all.insert(all.end(), low.begin(), low.end());
    return vbox(all) | size(HEIGHT, EQUAL, height);
}

Element quit_dialog(const Model &m, Hits *hits)
{
    auto buttons = hbox({ hit(button("Keep converting", m.quitChoice == 0), hits, { Target::Kind::QuitButton, 0 }),
                          text("  "),
                          hit(button("Stop and quit", m.quitChoice == 1), hits, { Target::Kind::QuitButton, 1 }) });
    // It has the focus: heavy cyan edges, as a box with the focus.
    const std::string message = " A conversion is running. Quitting stops it; nothing is half-published. ";
    const int w = width(message) + 2;
    return frame_box(w, 5, Edge::Focus, { part(" Quit ", bold) }, {}, {},
                     vbox({ text(message), text(""), buttons | hcenter }));
}

}
