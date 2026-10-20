#include "batch_text.h"

#include <cmath>
#include <cstdio>
#include <map>

#include "common/failures.h"
#include "flirt/run_summary.h"

namespace flirt
{

using json = nlohmann::json;

namespace
{

// The value column: "    <label>" padded to it.
constexpr size_t Indent = 4, LabelWidth = 10;

bool starts_with(const std::string &s, const std::string &prefix)
{
    return s.compare(0, prefix.size(), prefix) == 0;
}

// Characters, one per UTF-8 sequence.
size_t columns(const std::string &text)
{
    size_t n = 0;
    for (unsigned char c: text)
        if ((c & 0xC0) != 0x80)
            ++n;
    return n;
}

std::string number(uint64_t n)
{
    return separate_thousands(std::to_string(n));
}

std::string counted(uint64_t n, const std::string &one, const std::string &many)
{
    return number(n) + " " + (n == 1 ? one : many);
}

// The text in lines of at most `width` columns, broken at spaces (the spaces
// at a break dropped); the first line has `first` columns less, the others
// `others` less. A word longer than a line stands alone.
std::vector<std::string> wrap(const std::string &text, size_t first, size_t others, size_t width)
{
    std::vector<std::string> lines(1);
    size_t i = 0;
    std::string gap;
    while (i < text.size())
    {
        const auto end = text.find(' ', i);
        const auto word = text.substr(i, end == std::string::npos ? std::string::npos : end - i);
        const size_t room = (lines.size() == 1 ? first : others) < width
            ? width - (lines.size() == 1 ? first : others) : 1;
        if (!lines.back().empty() && columns(lines.back()) + columns(gap) + columns(word) > room)
        {
            lines.emplace_back();
            gap.clear();
        }
        lines.back() += (lines.back().empty() ? "" : gap) + word;
        if (end == std::string::npos)
            break;
        // The spaces after the word, kept inside a line ("_raw.root   _histos.root").
        size_t next = end;
        while (next < text.size() && text[next] == ' ')
            ++next;
        gap = text.substr(end, next - end);
        i = next;
    }
    return lines;
}

std::vector<std::string> rows_at(size_t indent, const std::string &label, const std::string &text,
                                 const Painter &paint, size_t width)
{
    const size_t column = indent + LabelWidth;
    const auto parts = wrap(text, column, column, width);
    std::vector<std::string> lines;
    const Tone tone = label == "warning" ? Tone::Warning : Tone::Label;
    const std::string pad = label.size() < LabelWidth ? std::string(LabelWidth - label.size(), ' ') : " ";
    for (size_t i = 0; i < parts.size(); ++i)
        lines.push_back(i == 0 ? std::string(indent, ' ') + paint(label, tone) + pad + parts[i]
                               : std::string(column, ' ') + parts[i]);
    return lines;
}

// The report's value at a JSON pointer, or null.
json at(const json &report, const char *pointer)
{
    try
    {
        return report.at(json::json_pointer(pointer));
    }
    catch (const std::exception &)
    {
        return json();
    }
}

uint64_t count_at(const json &report, const char *pointer)
{
    const auto value = at(report, pointer);
    return value.is_number_unsigned() || value.is_number_integer() ? value.get<uint64_t>() : 0;
}

}

std::string Painter::operator()(const std::string &text, Tone tone) const
{
    if (!on_ || text.empty())
        return text;
    const char *code = "";
    switch (tone)
    {
        case Tone::Plain: return text;
        case Tone::Ok: code = "\033[32m"; break;
        case Tone::Skip: code = "\033[34m"; break;
        case Tone::Warning: code = "\033[33m"; break;
        case Tone::Error: code = "\033[31m"; break;
        case Tone::Busy: case Tone::Label: code = "\033[36m"; break;
        case Tone::Dim: code = "\033[2m"; break;
    }
    return code + text + "\033[0m";
}

std::string Painter::bold(const std::string &text) const
{
    return on_ ? "\033[1m" + text + "\033[0m" : text;
}

std::string elapsed_text(double seconds)
{
    if (seconds < 59.95)
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f s", seconds < 0 ? 0.0 : seconds);
        return buf;
    }
    return format_duration(static_cast<int64_t>(std::llround(seconds)));
}

void add_warning(RunBlock &block, const std::string &text)
{
    block.rows.push_back({ "warning", text, {} });
    ++block.warnings;
}

RunBlock converted_block(const std::string &runId, double seconds, const json &report)
{
    RunBlock b;
    b.runId = runId;
    b.mark = "✓";
    b.tone = Tone::Ok;
    b.outcome = "converted";
    b.rest = " in " + elapsed_text(seconds);
    auto row = [&](const std::string &label, const std::string &text) { b.rows.push_back({ label, text, {} }); };
    auto note = [&](const std::string &text, const std::string &shared = {})
    {
        b.rows.push_back({ "note", text, shared.empty() ? text : shared });
    };
    std::vector<std::string> warnings;

    // The tree entries: one per EventData message, so after multi-event
    // splitting (flirt_convert.cc "wrote ... events").
    const uint64_t written = count_at(report, "/replay/eventDataMessages");
    row("events", number(written));

    // The files this run wrote, by their endings.
    std::string files = "_raw.root   _histos.root";
    if (at(report, "/output/waveforms").is_string())
        files += "   _waveforms.root";
    row("files", files + "   .flirt.json");

    // The waveform config check (flirt_convert.cc waveform_config_check()).
    for (const auto &m: at(report, "/waveformConfig").is_array() ? at(report, "/waveformConfig") : json::array())
    {
        const auto result = m.value("result", std::string());
        const auto text = "waveforms: " + m.value("module", std::string()) + ": " + m.value("message", std::string());
        if (result == "warn")
            warnings.push_back(text);
        else if (result == "note")
            note(text);
    }
    // --accept-damaged-input: each failed check in plain words (common/failures.h).
    const auto checks = at(report, "/checks");
    if (checks.is_object())
        for (auto it = checks.begin(); it != checks.end(); ++it)
        {
            const auto result = it->value("result", std::string());
            if (result != "fail" && result != "unavailable")
                continue;
            const auto f = explain_failure("failed", it.key() + ": " + result + ": " + it->value("detail", std::string()),
                                           false, FailureFor::Cli);
            warnings.push_back("published despite a failed check (--accept-damaged-input): " + f.headline
                               + (f.why.empty() ? "" : ": " + f.why) + " (check " + it.key() + ")");
        }

    for (const auto &w: warnings)
        add_warning(b, w);
    if (b.warnings)
        b.attention = counted(b.warnings, "warning", "warnings");
    return b;
}

std::vector<std::string> render_row(const std::string &label, const std::string &text, const Painter &paint,
                                    size_t width)
{
    return rows_at(Indent, label, text, paint, width);
}

std::vector<std::string> render_block(const RunBlock &block, bool notes, const Painter &paint, size_t width)
{
    std::string plain = block.mark + " " + block.runId + "   " + block.outcome + block.rest;
    std::string warned;
    if (block.warnings)
        warned = counted(block.warnings, "warning", "warnings");
    if (!warned.empty())
        plain += " · " + warned;

    // The header, wrapped under the run ID; only its first line is coloured.
    std::vector<std::string> lines;
    const auto parts = wrap(plain, 0, Indent, width);
    for (size_t i = 0; i < parts.size(); ++i)
    {
        if (i > 0)
        {
            lines.push_back(std::string(Indent, ' ') + parts[i]);
            continue;
        }
        const auto head = block.mark + " " + block.runId + "   " + block.outcome;
        if (!paint.on() || !starts_with(parts[0], head))
        {
            lines.push_back(parts[0]);
            continue;
        }
        std::string tail = parts[0].substr(head.size());
        if (!warned.empty() && tail.size() >= warned.size() && tail.compare(tail.size() - warned.size(), warned.size(), warned) == 0)
            tail = tail.substr(0, tail.size() - warned.size()) + paint(warned, Tone::Warning);
        lines.push_back(paint(block.mark, block.tone) + " " + paint.bold(block.runId) + "   "
                        + paint(block.outcome, block.tone) + tail);
    }
    for (const auto &row: block.rows)
    {
        if (row.label == "note" && !notes)
            continue;
        for (auto &line: render_row(row.label, row.text, paint, width))
            lines.push_back(std::move(line));
    }
    return lines;
}

std::vector<std::string> render_end(const std::vector<const RunBlock *> &blocks, const std::string &counts,
                                    size_t runsGiven, bool notesInBlocks, const Painter &paint, size_t width)
{
    std::vector<const RunBlock *> attention;
    for (auto b: blocks)
        if (!b->attention.empty())
            attention.push_back(b);

    std::vector<std::string> lines = { "", "Done: " + counts
                                           + (attention.empty() && runsGiven > 1 ? " Nothing needs attention." : "") };
    if (!attention.empty())
    {
        lines.push_back("Needs attention:");
        size_t idWidth = 0;
        for (auto b: attention)
            idWidth = std::max(idWidth, columns(b->runId));
        for (auto b: attention)
        {
            const size_t column = 2 + 2 + idWidth + 3;
            const auto parts = wrap(b->attention, column, column, width);
            for (size_t i = 0; i < parts.size(); ++i)
                lines.push_back(i == 0 ? "  " + paint(b->mark, b->tone) + " " + b->runId
                                             + std::string(idWidth - columns(b->runId) + 3, ' ') + parts[i]
                                       : std::string(column, ' ') + parts[i]);
        }
    }

    // A note several runs have, said once; with notes not said in the
    // blocks, also a note only one run has, named.
    if (!notesInBlocks)
    {
        std::vector<std::string> order;
        std::map<std::string, std::vector<const RunBlock *>> runs;
        std::map<std::string, std::string> shared;
        for (auto b: blocks)
            for (const auto &row: b->rows)
                if (row.label == "note")
                {
                    auto &list = runs[row.text];
                    if (list.empty())
                    {
                        order.push_back(row.text);
                        shared[row.text] = row.shared.empty() ? row.text : row.shared;
                    }
                    if (list.empty() || list.back() != b)
                        list.push_back(b);
                }
        for (const auto &text: order)
        {
            const auto &list = runs[text];
            std::string names;
            for (auto b: list)
                names += (names.empty() ? "" : ", ") + b->runId;
            const auto said = list.size() == runsGiven && runsGiven > 1
                ? (runsGiven == 2 ? std::string("For both runs: ")
                                  : "For all " + std::to_string(runsGiven) + " runs: ") + shared[text]
                : "For " + std::to_string(list.size()) + " of " + std::to_string(runsGiven) + " runs (" + names + "): "
                    + (list.size() > 1 ? shared[text] : text);
            for (auto &line: rows_at(0, "note", said, paint, width))
                lines.push_back(std::move(line));
        }
    }
    return lines;
}

}
