#include "flirt/settings_changes.h"

#include <algorithm>
#include <regex>

// Patterns, derived from `git show <tag>:<path>` for v1.16.2, v1.18.0 and
// v1.21.0 of external/mvme (docs/design.md, "Settings changes"):
// - mvme lines "HH:MM:SS: <message>": MVMEContext::logMessage
//   (mvme_context.cc:1915-1921). logError stores the message without the
//   prefix (mvme_context.cc:1924-1940). spdlog lines "[date time] [logger]
//   [level] ..." come through a qt_sink (mvme.cpp:169-197).
// - The window: mvme's own "Entering readout loop" and "Leaving readout loop"
//   (mvlc_readout_worker.cc:583, :617; 1.16.2 :579, :613); mesytec-mvlc's
//   spdlog "Entering readout loop" is ignored.
// - Scripts: ScriptConfigRunner logs `Running script "<title>"`, then either
//   "  " + format_result per command, or (AggregateResults, the Trigger I/O
//   editor, mvme.cpp:1987) "  Executed N commands, no errors" or
//   "  Error: <e>" (vme_config_scripts.cc:251-298).
// - format_result (vme_script_exec.cc:460-611; 1.16.2 :442) with
//   to_string(cmd, true) (vme_script.cc:2154-2360): the write forms end in
//   ", write ok"; a failed one starts with `Error from "` or `Warning from "`
//   and still ends in ", write ok", so only the forms below count.
// - VME Debug: "VME Debug: write 0x%08x -> 0x%04x, write ok" or
//   "..., vmeError=..." (vme_debug_widget.cc:313-321). Its "Run script"
//   (:367-389) logs no results.

namespace flirt
{

namespace
{

struct Line
{
    bool mvme = false;      // has mvme's "HH:MM:SS: " prefix
    bool spdlog = false;    // an spdlog line (no mvme prefix)
    std::string time;
    size_t indent = 0;      // spaces after the prefix
    std::string text;       // after the prefix and the indentation
};

bool is_digit(char c) { return c >= '0' && c <= '9'; }

Line classify(const std::string &raw)
{
    Line l;
    std::string body = raw;
    if (raw.size() >= 10 && is_digit(raw[0]) && is_digit(raw[1]) && raw[2] == ':'
        && is_digit(raw[3]) && is_digit(raw[4]) && raw[5] == ':' && is_digit(raw[6])
        && is_digit(raw[7]) && raw[8] == ':' && raw[9] == ' ')
    {
        l.mvme = true;
        l.time = raw.substr(0, 8);
        body = raw.substr(10);
    }
    else
        l.spdlog = !raw.empty() && raw[0] == '[';

    l.indent = body.find_first_not_of(' ');
    if (l.indent == std::string::npos)
        l.indent = body.size();
    l.text = body.substr(l.indent);
    return l;
}

const std::regex &write_re()
{
    static const std::regex re(
        R"(^(?:write|writeabs) (?:[a-z0-9]+ )?\(amod=0x[0-9a-f]{2}\) (?:d16|d32|unknown) (0x[0-9a-f]{8}) (0x[0-9a-f]{8}) \(\d+ dec\), write ok$)");
    return re;
}

const std::regex &vmusb_write_re()
{
    static const std::regex re(
        R"(^vmusb_write_reg (0x[0-9a-f]{8}) (0x[0-9a-f]{8}) \(.*\) \(\d+ dec\), write ok$)");
    return re;
}

const std::regex &accu_write_re()
{
    static const std::regex re(
        R"(^accu_write (?:[a-z0-9]+ )?\(amod=0x[0-9a-f]{2}\) (?:d16|d32|unknown) (0x[0-9a-f]{8}) -> (0x[0-9a-f]{8}) \(\d+ dec\), write ok$)");
    return re;
}

bool starts_with(const std::string &s, const char *prefix)
{
    return s.rfind(prefix, 0) == 0;
}

// A successful write of format_result, text after the indentation.
bool parse_success_write(const std::string &text, LoggedWrite &w)
{
    if (!(starts_with(text, "write") || starts_with(text, "vmusb_write_reg ")
          || starts_with(text, "accu_write ")))
        return false;

    std::smatch m;
    if (std::regex_match(text, m, write_re()) || std::regex_match(text, m, vmusb_write_re())
        || std::regex_match(text, m, accu_write_re()))
    {
        w = { m[1].str(), m[2].str() };
        return true;
    }
    return false;
}

// mvme writes the log as UTF-8 (toUtf8(), mvlc_readout_worker.cc:510). Text
// taken from it goes into the report, whose JSON library (nlohmann 3.3.0)
// throws on invalid UTF-8, so any other log is not checked.
bool is_valid_utf8(const std::string &text)
{
    const auto *p = reinterpret_cast<const unsigned char *>(text.data());
    const size_t n = text.size();
    for (size_t i = 0; i < n;)
    {
        const unsigned c = p[i];
        const size_t len = c < 0x80 ? 1 : c >= 0xC2 && c <= 0xDF ? 2 : c >= 0xE0 && c <= 0xEF ? 3
            : c >= 0xF0 && c <= 0xF4 ? 4 : 0;
        if (!len || i + len > n)
            return false;
        // The second byte's range excludes overlong forms, surrogates and code
        // points above U+10FFFF.
        const unsigned lo = c == 0xE0 ? 0xA0 : c == 0xF0 ? 0x90 : 0x80;
        const unsigned hi = c == 0xED ? 0x9F : c == 0xF4 ? 0x8F : 0xBF;
        for (size_t k = 1; k < len; ++k)
        {
            const unsigned b = p[i + k];
            if (k == 1 ? b < lo || b > hi : b < 0x80 || b > 0xBF)
                return false;
        }
        i += len;
    }
    return true;
}

std::vector<std::string> split_lines(const std::string &text)
{
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos < text.size())
    {
        auto nl = text.find('\n', pos);
        if (nl == std::string::npos)
            nl = text.size();
        lines.push_back(text.substr(pos, nl - pos));
        pos = nl + 1;
    }
    return lines;
}

}

SettingsChanges find_settings_changes(const std::string &logText)
{
    SettingsChanges result;
    const auto raw = split_lines(logText);

    if (raw.empty())
    {
        result.reason = "messages.log is empty";
        return result;
    }
    if (!is_valid_utf8(logText))
    {
        result.reason = "messages.log is not valid UTF-8 (mvme writes it as UTF-8)";
        return result;
    }

    std::vector<Line> lines;
    lines.reserve(raw.size());
    for (const auto &r: raw)
        lines.push_back(classify(r));

    // "mvme <version> (<bitness>) running on ..." (mvme_context.cc:1751-1755).
    {
        static const std::regex re(R"(^mvme (\S+) \()");
        std::smatch m;
        if (lines[0].mvme && std::regex_search(lines[0].text, m, re))
            result.mvmeVersion = m[1].str();
    }

    auto is_marker = [](const Line &l, const char *message)
    {
        return l.mvme && l.indent == 0 && l.text == message;
    };

    size_t enter = 0;
    while (enter < lines.size() && !is_marker(lines[enter], "Entering readout loop"))
        ++enter;
    if (enter == lines.size())
    {
        result.reason = "messages.log has no \"Entering readout loop\" line";
        return result;
    }

    size_t leave = enter + 1;
    while (leave < lines.size() && !is_marker(lines[leave], "Leaving readout loop"))
        ++leave;
    if (leave == lines.size())
    {
        result.reason = "messages.log has no \"Leaving readout loop\" line after the start of the readout";
        return result;
    }

    // Self-check: every DAQ start writes module settings through format_result
    // (vme_daq.cc:215-222, run_script with LogEachResult, vme_script_exec.cc:58-66).
    bool startWriteSeen = false;
    for (size_t i = 0; i < enter && !startWriteSeen; ++i)
    {
        LoggedWrite w;
        startWriteSeen = lines[i].mvme && parse_success_write(lines[i].text, w);
    }
    if (!startWriteSeen)
    {
        result.reason = "no register write of the DAQ start recognised in messages.log"
                        " (a log format FLIRT does not know)";
        return result;
    }

    result.checked = true;

    // The script being read: its lines end at the next mvme line that is not
    // indented (spdlog lines from other threads and logError lines, which have
    // no prefix, do not end it).
    bool open = false, titled = false;
    SettingsChange current;

    auto close = [&]()
    {
        if (!open)
            return;
        if (!current.writes.empty())
            result.changes.push_back(current);
        else if (titled && (current.commands || current.error))
        {
            current.writesListed = false;
            result.changes.push_back(current);
        }
        open = false;
    };

    auto begin = [&](const std::string &time, const std::string &source, bool withTitle)
    {
        close();
        open = true;
        titled = withTitle;
        current = {};
        current.time = time;
        current.source = source;
    };

    static const std::regex runningRe(R"re(^Running script "(.*)"$)re");
    static const std::regex debugRe(R"(^VME Debug: write (0x[0-9a-f]{8}) -> (0x[0-9a-f]{4,8}), write ok$)");
    static const std::regex executedRe(R"(^Executed (\d+) commands, no errors$)");

    for (size_t i = enter + 1; i < leave; ++i)
    {
        const auto &l = lines[i];
        std::smatch m;

        if (l.mvme && l.indent == 0)
        {
            close();
            if (std::regex_match(l.text, m, runningRe))
                begin(l.time, "script \"" + m[1].str() + "\"", true);
            else if (std::regex_match(l.text, m, debugRe))
            {
                SettingsChange change;
                change.time = l.time;
                change.source = "VME Debug";
                change.writes.push_back({ m[1].str(), m[2].str() });
                result.changes.push_back(change);
            }
        }
        else if (l.mvme)
        {
            LoggedWrite w;
            if (parse_success_write(l.text, w))
            {
                if (!open)
                    begin(l.time, "script", false);
                current.writes.push_back(w);
            }
            else if (open && titled && std::regex_match(l.text, m, executedRe))
                current.commands = std::stoll(m[1].str());
        }
        else if (!l.spdlog && open && titled && starts_with(l.text, "Error: "))
            current.error = l.text.substr(7);
    }
    close();

    return result;
}

std::string settings_changes_line(const SettingsChanges &result)
{
    if (!result.checked)
        return "note: settings changes during the run not checked: " + result.reason;
    if (result.changes.empty())
        return {};

    auto describe = [](const SettingsChange &c)
    {
        if (c.source == "VME Debug" && c.writes.size() == 1)
            return c.time + " VME Debug write " + c.writes[0].address + " -> " + c.writes[0].value;
        if (c.writesListed)
            return c.time + " " + c.source + " (" + std::to_string(c.writes.size()) + " register write"
                + (c.writes.size() == 1 ? "" : "s") + ")";
        if (c.error)
            return c.time + " " + c.source + " (failed: " + *c.error + "; mvme does not list its writes)";
        return c.time + " " + c.source + " (" + std::to_string(c.commands.value_or(0))
            + " commands; mvme does not list their writes)";
    };

    std::string line = "warning: settings changed during the run: ";
    const size_t shown = std::min<size_t>(3, result.changes.size());
    for (size_t i = 0; i < shown; ++i)
        line += (i ? "; " : "") + describe(result.changes[i]);
    if (result.changes.size() > shown)
        line += "; and " + std::to_string(result.changes.size() - shown) + " more";
    return line + "; see the report";
}

}
