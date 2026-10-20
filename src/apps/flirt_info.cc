// flirt-info: what mvme MVLC listfile archives record about their runs:
// start, stop, events, lost packets, the rate over time with pauses, low-rate
// periods and rate changes, the mvme version and system, the modules and
// settings changes. It only reads: inputs are
// opened read-only and nothing is written except --csv FILE.
//
// Each listfile is replayed through mesytec-mvlc's readout parser alone
// (scan_listfile, listfile_scan.h); the run information is built as
// flirt-convert builds its report["run"] (run_info.h).

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <csignal>
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "common/cores.h"
#include "common/failures.h"
#include "common/names.h"
#include "common/platform.h"
#include "common/utf8.h"
#include "flirt/integrity.h"
#include "flirt/listfile.h"
#include "flirt/listfile_scan.h"
#include "flirt/module_templates.h"
#include "flirt/run_info.h"
#include "flirt/vme_config.h"

namespace
{

using json = nlohmann::json;

// Bad arguments: exit code 2 before anything is read.
struct usage_error: std::runtime_error
{
    bool showUsage;

    explicit usage_error(const std::string &what, bool showUsage_ = false)
        : std::runtime_error(what), showUsage(showUsage_)
    { }
};

void usage(const char *argv0)
{
    std::cerr << "usage: " << argv0 << " [--csv <file> | --json] <input>...\n"
              << "\n"
              << "Shows what mvme MVLC listfile archives record about their runs, without\n"
              << "converting them: start, stop, events, lost packets, the rate over time with\n"
              << "pauses, low-rate periods and rate changes, the mvme version and system, the\n"
              << "modules and settings changes. An input is a listfile archive or a directory,\n"
              << "which gives its *.zip files (not recursive, in natural order).\n"
              << "One listfile: its details; several: one row per run, then a summary line.\n"
              << "Inputs are only read; nothing is written except the --csv file.\n"
              << "\n"
              << "  --csv <file>        also write every field of every run to this new CSV file\n"
              << "                      (written at the end, never over an existing file;\n"
              << "                      - : the CSV on stdout instead)\n"
              << "  --json              stdout: one JSON object per listfile, for programs\n"
              << "\n"
              << "Exit codes: 0 every listfile was read (damaged ones included), 1 some could\n"
              << "not be read at all, 2 usage.\n";
}

struct Options
{
    std::vector<std::string> inputs;
    std::string csvPath;    // empty: none; "-": stdout
    bool json = false;
};

Options parse_args(int argc, char *argv[])
{
    Options opt;
    std::vector<std::string> sources;

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc)
                throw usage_error(arg + " needs a value");
            return argv[++i];
        };

        if (arg == "--csv") opt.csvPath = value();
        else if (arg == "--json") opt.json = true;
        else if (arg == "-") sources.push_back(arg);
        else if (!arg.empty() && arg[0] == '-') throw usage_error("unknown option " + arg, true);
        else sources.push_back(arg);
    }

    if (opt.json && opt.csvPath == "-")
        throw usage_error("--json and --csv - both want stdout");
    if (opt.csvPath.empty() && std::find(argv + 1, argv + argc, std::string("--csv")) != argv + argc)
        throw usage_error("--csv needs a file name");

    for (const auto &path: sources)
    {
        try
        {
            flirt::add_listfile_input(path, opt.inputs);
        }
        catch (const std::runtime_error &e)
        {
            throw usage_error(e.what());
        }
    }
    if (opt.inputs.empty())
        throw usage_error("no inputs", true);

    // The CSV file must be new: never an input (same device and inode), a
    // directory or any other existing file; write_csv() creates it exclusively.
    if (!opt.csvPath.empty() && opt.csvPath != "-")
    {
        if (const auto refusal = flirt::name_refusal(flirt::own_name(opt.csvPath)); !refusal.empty())
            throw usage_error("--csv " + refusal);
        struct stat cst;
        if (stat(opt.csvPath.c_str(), &cst) == 0)
        {
            if (S_ISDIR(cst.st_mode))
                throw usage_error("--csv " + opt.csvPath + " is a directory");
            for (const auto &input: opt.inputs)
            {
                struct stat ist;
                if (stat(input.c_str(), &ist) == 0 && ist.st_dev == cst.st_dev && ist.st_ino == cst.st_ino)
                    throw usage_error("--csv " + opt.csvPath + " is the input " + input);
            }
        }
        if (lstat(opt.csvPath.c_str(), &cst) == 0)
            throw usage_error("--csv " + opt.csvPath + " exists; flirt-info never replaces a file: give a new file name");
    }

    return opt;
}

// One listfile's information.
struct Info
{
    std::string input, runId;
    std::string status = "ok";      // ok, damaged, unreadable
    std::string problem;            // unreadable: why; damaged: the I2 detail
    uint64_t bytes = 0;
    std::optional<mesytec::mvlc::ConnectionType> connection;
    flirt::VMEConfig vmeConfig;
    flirt::RunFacts facts;
    flirt::LibraryMessages parserWarnings;  // mesytec-mvlc's messages during the replay
};

// Reads one listfile; no output.
Info read_info(const std::string &input, const std::vector<flirt::ModuleMeta> &templates)
{
    Info info;
    info.input = input;
    info.runId = flirt::run_id_from_path(input);

    try
    {
        // Only standard names, as flirt-convert (common/names.h).
        if (const auto refusal = flirt::name_refusal(flirt::own_name(input)); !refusal.empty())
        {
            info.runId = flirt::printable_text(info.runId);
            throw std::runtime_error(refusal);
        }

        struct stat st;
        if (stat(input.c_str(), &st) == 0)
            info.bytes = static_cast<uint64_t>(st.st_size);

        // The same refusals as flirt-convert's (listfile.h, listfile_scan.h).
        const auto listfile = flirt::read_listfile_info(input);
        info.connection = listfile.format;
        info.vmeConfig = flirt::parse_vme_config(listfile.vmeConfigJson, templates);
        const auto scan = flirt::scan_listfile(listfile, info.vmeConfig);
        info.facts = flirt::run_facts(listfile, scan.parserCounters, scan.run);
        info.parserWarnings = scan.parserWarnings;

        if (scan.listfileEntry.result() != "pass")
        {
            info.status = "damaged";
            info.problem = scan.listfileEntry.detail();
        }
    }
    catch (const std::exception &e)
    {
        info.status = "unreadable";
        info.problem = e.what();
    }
    return info;
}

std::string connection_name(const Info &info)
{
    if (!info.connection)
        return {};
    return *info.connection == mesytec::mvlc::ConnectionType::ETH ? "ETH"
        : *info.connection == mesytec::mvlc::ConnectionType::USB ? "USB" : "unknown";
}

bool is_eth(const Info &info)
{
    return info.connection && *info.connection == mesytec::mvlc::ConnectionType::ETH;
}

// "event0: mdpp32_scp (mdpp32_scp), other (type) (disabled)", events joined by "; ".
std::string modules_text(const flirt::VMEConfig &vme)
{
    std::string text;
    for (const auto &event: vme.events)
    {
        std::string modules;
        for (const auto &m: event.modules)
            modules += (modules.empty() ? "" : ", ") + m.name.toStdString() + " ("
                + (m.typeName.isEmpty() ? std::string("unknown") : m.typeName.toStdString()) + ")"
                + (m.enabled ? "" : " (disabled)");
        text += (text.empty() ? "" : "; ") + event.name.toStdString() + ": " + modules;
    }
    return text;
}

// 12.3 MB: binary units, as ls -h and mvme's format_number (util/strings.cc:29-79).
std::string size_text(uint64_t bytes)
{
    const char *const units[] = { "B", "kB", "MB", "GB", "TB" };
    double value = double(bytes);
    size_t unit = 0;
    while (value >= 1024 && unit + 1 < sizeof(units) / sizeof(units[0]))
        value /= 1024, ++unit;
    char buf[64];
    std::snprintf(buf, sizeof(buf), unit ? "%.1f %s" : "%.0f %s", value, units[unit]);
    return buf;
}

// The lost packets as the details show them.
std::string lost_details(const Info &info)
{
    if (!is_eth(info))
        return "not recorded (" + connection_name(info) + ")";
    const auto text = flirt::loss_text(info.facts);
    return text.empty() ? "none" : text;
}

std::string recorded_by(const Info &info)
{
    const auto &log = info.facts.log;
    auto known = [](const std::optional<std::string> &s) { return s ? *s : std::string("unknown"); };
    return "mvme " + known(log.mvmeVersion) + " on " + known(log.system) + ", MVLC over "
        + connection_name(info) + ", jumbo frames "
        + (log.jumboFrames ? (*log.jumboFrames ? "on" : "off") : "unknown")
        + ", receive buffer " + known(log.receiveBuffer);
}

// The labeled lines of the details block, as print_details prints them and
// --json carries them ("details").
struct DetailLine
{
    std::string label, text;
};

std::vector<DetailLine> detail_lines(const Info &info)
{
    if (info.status == "unreadable")
        return { {"unreadable", info.problem} };
    const auto &f = info.facts;
    // run_summary_line() starts with "run ", which the label already says.
    std::vector<DetailLine> lines = {
        {"run", flirt::run_summary_line(f.times, f.events).substr(4)},
        {"lost packets", lost_details(info)},
        {"rate", flirt::rate_summary_text(f.rate)},
        {"recorded by", recorded_by(info)},
        {"modules", modules_text(info.vmeConfig)} };
    if (const auto line = flirt::settings_changes_line(f.settings); !line.empty())
        lines.push_back({"settings", line});
    // mesytec-mvlc's messages, counted instead of printed (library_log.h).
    if (const auto n = info.parserWarnings.count)
        lines.push_back({"parser", std::to_string(n) + (n == 1 ? " warning" : " warnings")
                                       + " about the recording (not a FLIRT problem); flirt-info --json lists "
                                       + (n > info.parserWarnings.messages.size() ? "the first "
                                              + std::to_string(info.parserWarnings.messages.size()) : "them")});
    if (info.status == "damaged")
        lines.push_back({"damaged", flirt::plain_check_detail(info.problem)});   // common/failures.h
    return lines;
}

// The rate warnings after the labeled lines (run_timeline.h); none if unreadable.
std::vector<std::string> detail_warnings(const Info &info)
{
    return info.status == "unreadable" ? std::vector<std::string>() : flirt::rate_warnings(info.facts.rate, "flirt-info --json lists them all");
}

// The receive-buffer note under the details; empty if none (unreadable: none).
std::string detail_note(const Info &info)
{
    return info.status == "unreadable" ? std::string() : flirt::receive_buffer_note(info.facts);
}

void print_details(const Info &info)
{
    std::cout << info.runId << " (" << info.input << ", " << size_text(info.bytes) << ")\n";
    for (const auto &line: detail_lines(info))
        std::cout << "  " << line.label << std::string(14 - line.label.size(), ' ') << line.text << "\n";
    for (const auto &line: detail_warnings(info))
        std::cout << line << "\n";
    if (const auto note = detail_note(info); !note.empty())
        std::cout << note << "\n";
}

std::string pad(const std::string &s, size_t width)
{
    return s.size() >= width ? s + " " : s + std::string(width - s.size(), ' ');
}

std::string right(const std::string &s, size_t width)
{
    return s.size() >= width ? " " + s : std::string(width - s.size(), ' ') + s;
}

std::string rate_text(const flirt::RunFacts &f)
{
    if (!f.times.live || *f.times.live <= 0)
        return "unknown";
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.1f", double(f.events) / double(*f.times.live));
    return buf;
}

std::string table_header(size_t runWidth)
{
    return pad("run", runWidth) + pad("start", 18) + pad("length", 17) + right("events", 9) + right("rate/s", 8)
        + "  " + pad("lost packets", 16) + "flags";
}

std::string table_row(const Info &info, size_t runWidth)
{
    if (info.status == "unreadable")
        return pad(info.runId, runWidth) + "unreadable: " + info.problem;

    const auto &f = info.facts;
    std::string lost;
    if (!is_eth(info))
        lost = "n/a (" + connection_name(info) + ")";
    else if (!f.counters.ethPacketLoss)
        lost = "none";
    else
        lost = std::to_string(f.counters.ethPacketLoss) + " ("
            + flirt::share_text(f.counters.ethPacketLoss, f.counters.ethPacketLoss + f.counters.ethPacketsProcessed) + ")";

    std::string flags = flirt::rate_flags_text(f.rate);
    if (info.status == "damaged")
        flags += (flags.empty() ? "" : "; ") + std::string("damaged: ") + flirt::plain_check_detail(info.problem);

    auto row = pad(info.runId, runWidth)
        + pad(f.times.start ? flirt::format_local_time(*f.times.start, "%Y-%m-%d %H:%M") : "unknown", 18)
        + pad(f.times.duration ? flirt::format_duration(*f.times.duration) : "unknown", 17)
        + right(std::to_string(f.events), 9) + right(rate_text(f), 8) + "  " + pad(lost, 16) + flags;
    row.erase(row.find_last_not_of(' ') + 1);
    return row;
}

// A number for the CSV: up to 9 decimals, trailing zeros removed.
std::string csv_number(double v)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.9f", v);
    std::string s = buf;
    if (s.find('.') != std::string::npos)
    {
        s.erase(s.find_last_not_of('0') + 1);
        if (s.back() == '.')
            s.pop_back();
    }
    return s;
}

template <typename T>
std::string csv_opt(const std::optional<T> &v)
{
    return v ? std::to_string(*v) : std::string();
}

// RFC 4180: a field with a comma, a quote or a line break is quoted, quotes doubled.
std::string csv_field(const std::string &raw)
{
    const auto text = flirt::valid_utf8(raw);
    if (text.find_first_of(",\"\r\n") == std::string::npos)
        return text;
    std::string out = "\"";
    for (char c: text)
        out += c == '"' ? std::string("\"\"") : std::string(1, c);
    return out + "\"";
}

const char *const CsvHeader = "listfile,run,status,problem,start,stop,duration_s,paused_s,live_s,events,rate_hz,"
    "median_rate_hz,eth_packets,lost_packets,lost_share,lost_events_estimate,loss_bursts,loss_minutes,flags,"
    "mvme_version,system,connection,jumbo_frames,receive_buffer_bytes,modules,settings_changes,parser_warnings";

std::string csv_row(const Info &info)
{
    std::vector<std::string> fields = { info.input, info.runId, info.status, info.problem };
    if (info.status == "unreadable")
    {
        fields.resize(27);
        fields[21] = connection_name(info);
    }
    else
    {
        const auto &f = info.facts;
        const auto &pc = f.counters;
        const bool eth = is_eth(info);
        const uint64_t lost = pc.ethPacketLoss, received = pc.ethPacketsProcessed;
        auto time = [](const std::optional<int64_t> &t) { return t ? flirt::iso8601_local(*t) : std::string(); };
        fields.push_back(time(f.times.start));
        fields.push_back(time(f.times.stop));
        fields.push_back(csv_opt(f.times.duration));
        fields.push_back(csv_opt(f.times.paused));
        fields.push_back(csv_opt(f.times.live));
        fields.push_back(std::to_string(f.events));
        fields.push_back(f.times.live && *f.times.live > 0
                         ? csv_number(double(f.events) / double(*f.times.live)) : std::string());
        fields.push_back(f.rate.median ? csv_number(*f.rate.median) : std::string());
        fields.push_back(eth ? std::to_string(received + lost) : std::string());
        fields.push_back(eth ? std::to_string(lost) : std::string());
        fields.push_back(eth && received + lost ? csv_number(double(lost) / double(received + lost))
                                                : eth ? std::string("0") : std::string());
        fields.push_back(eth ? std::to_string(flirt::events_lost_estimate(lost, received, f.events, f.lossBursts))
                             : std::string());
        fields.push_back(eth ? std::to_string(f.lossBursts) : std::string());
        fields.push_back(eth && !f.rate.minutes.empty() ? std::to_string(flirt::minutes_with_loss(f.rate))
                                                        : std::string());
        fields.push_back(flirt::rate_flags_text(f.rate));
        fields.push_back(f.log.mvmeVersion.value_or(""));
        fields.push_back(f.log.system.value_or(""));
        fields.push_back(connection_name(info));
        fields.push_back(f.log.jumboFrames ? (*f.log.jumboFrames ? "true" : "false") : "");
        fields.push_back(csv_opt(f.log.receiveBufferBytes));
        fields.push_back(modules_text(info.vmeConfig));
        fields.push_back(flirt::settings_changes_line(f.settings));
        fields.push_back(std::to_string(info.parserWarnings.count));
    }

    std::string row;
    for (size_t i = 0; i < fields.size(); ++i)
        row += (i ? "," : "") + csv_field(fields[i]);
    return row + "\r\n";
}

// As the conversion report's "modules": the event by its index; "details",
// "warnings" and "note" as the details block shows them; "parserWarnings"
// mesytec-mvlc's messages (the first 100).
json json_line(const Info &info)
{
    json modules = json::array();
    for (size_t ei = 0; ei < info.vmeConfig.events.size(); ++ei)
        for (const auto &m: info.vmeConfig.events[ei].modules)
            modules.push_back({ {"event", ei},
                                {"module", m.name.toStdString()}, {"type", m.typeName.toStdString()},
                                {"enabled", m.enabled} });
    json details = json::array();
    for (const auto &line: detail_lines(info))
        details.push_back({ {"label", line.label}, {"text", line.text} });
    const auto connection = connection_name(info);
    const auto note = detail_note(info);
    const auto &pw = info.parserWarnings;
    return { {"listfile", info.input}, {"runId", info.runId}, {"status", info.status},
             {"problem", info.problem.empty() ? json(nullptr) : json(info.problem)},
             {"bytes", info.bytes}, {"connection", connection.empty() ? json(nullptr) : json(connection)},
             {"modules", modules},
             {"run", info.status == "unreadable" ? json(nullptr) : flirt::run_json(info.facts, info.vmeConfig)},
             {"details", details}, {"warnings", detail_warnings(info)},
             {"note", note.empty() ? json(nullptr) : json(note)},
             {"parserWarnings", { {"count", pw.count}, {"messages", pw.messages} }} };
}

std::string real_path(const std::string &path)
{
    char buf[PATH_MAX];
    if (!realpath(path.c_str(), buf))
        throw std::runtime_error("cannot resolve " + path);
    return buf;
}

// The mvme-like prefix this executable was built into: <prefix>/bin/flirt-info,
// with the module templates in <prefix>/templates (as flirt-convert).
std::string own_prefix()
{
    auto exe = real_path(flirt::self_exe());
    auto bin = exe.substr(0, exe.rfind('/'));
    return bin.substr(0, bin.rfind('/'));
}

// What one listfile adds to the output, in each form (details are printed
// from the Info itself: one listfile only).
struct Result
{
    bool unreadable = false;
    std::string jsonLine, tableRow, csvRow;
};

Result result_of(const Info &info, size_t runWidth)
{
    Result r;
    r.unreadable = info.status == "unreadable";
    r.jsonLine = flirt::dump_valid_utf8(json_line(info)) + "\n";
    r.tableRow = table_row(info, runWidth) + "\n";
    r.csvRow = csv_row(info);
    return r;
}

// Several listfiles read at once, each in a forked child that sends its
// Result back through a pipe. Not threads: a replay points mesytec-mvlc's
// loggers, shared by the whole process, at its own sink and back
// (library_log.h ScopedLoggerSinks, scan_listfile), so two replays in one
// process would swap each other's sinks and messages. Forked while this
// process has one thread; a child only reads, writes its pipe and _exit()s.
// The output is the same as one listfile after another: the parent prints
// each Result in the order given, and reads a listfile itself whenever no
// child has it (a limit of 1, or a child that failed).
class Reader
{
    public:
        Reader(const std::vector<std::string> &inputs, const std::vector<flirt::ModuleMeta> &templates,
               size_t runWidth)
            : inputs_(inputs), templates_(templates), runWidth_(runWidth)
        { }

        ~Reader()
        {
            for (const auto &[i, child]: children_)
            {
                close(child.fd);
                kill(child.pid, SIGKILL);
                waitpid(child.pid, nullptr, 0);
            }
        }

        // Starts children for the listfiles from `current` on while fewer than
        // the limit (asked again before each) run; none at a limit of 1.
        void start_more(size_t current)
        {
            next_ = std::max(next_, current);
            for (;;)
            {
                if (next_ >= inputs_.size())
                    return;
                const int limit = meter_.read_limit_now(static_cast<int>(children_.size()));
                if (limit < 2 || children_.size() >= static_cast<size_t>(limit))
                    return;
                if (!start(next_))
                    return;
                ++next_;
            }
        }

        // The Result of listfile i if a child read it; false if none did or it failed.
        bool collect(size_t i, Result &result)
        {
            const auto found = children_.find(i);
            if (found == children_.end())
                return false;
            const auto child = found->second;
            children_.erase(found);
            std::string bytes;
            for (;;)
            {
                char chunk[65536];
                const ssize_t n = read(child.fd, chunk, sizeof(chunk));
                if (n < 0 && errno == EINTR)
                    continue;
                if (n <= 0)
                    break;
                bytes.append(chunk, static_cast<size_t>(n));
            }
            close(child.fd);
            int status = 0;
            while (waitpid(child.pid, &status, 0) < 0 && errno == EINTR)
            { }
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
                return false;
            return decode(bytes, result);
        }

    private:
        struct Child
        {
            pid_t pid;
            int fd;
        };

        bool start(size_t i)
        {
            int fds[2];
            if (pipe(fds) != 0)
                return false;
            std::cout << std::flush;
            std::fflush(nullptr);
            const pid_t pid = fork();
            if (pid < 0)
            {
                close(fds[0]);
                close(fds[1]);
                return false;
            }
            if (pid == 0)
            {
                close(fds[0]);
                for (const auto &[j, other]: children_)
                    close(other.fd);
                const auto bytes = encode(result_of(read_info(inputs_[i], templates_), runWidth_));
                size_t done = 0;
                while (done < bytes.size())
                {
                    const ssize_t n = write(fds[1], bytes.data() + done, bytes.size() - done);
                    if (n < 0 && errno == EINTR)
                        continue;
                    if (n <= 0)
                        _exit(1);
                    done += static_cast<size_t>(n);
                }
                _exit(0);
            }
            close(fds[1]);
            children_[i] = { pid, fds[0] };
            return true;
        }

        // Each field as its length, a newline, then its bytes.
        static std::string encode(const Result &r)
        {
            std::string out = r.unreadable ? "1" : "0";
            for (const auto *field: { &r.jsonLine, &r.tableRow, &r.csvRow })
                out += "\n" + std::to_string(field->size()) + "\n" + *field;
            return out;
        }

        static bool decode(const std::string &bytes, Result &r)
        {
            if (bytes.empty() || (bytes[0] != '0' && bytes[0] != '1'))
                return false;
            r.unreadable = bytes[0] == '1';
            size_t at = 1;
            for (auto *field: { &r.jsonLine, &r.tableRow, &r.csvRow })
            {
                if (at >= bytes.size() || bytes[at] != '\n')
                    return false;
                const auto end = bytes.find('\n', at + 1);
                if (end == std::string::npos)
                    return false;
                size_t size = 0;
                try
                {
                    size = std::stoull(bytes.substr(at + 1, end - at - 1));
                }
                catch (const std::exception &)
                {
                    return false;
                }
                if (end + 1 + size > bytes.size())
                    return false;
                *field = bytes.substr(end + 1, size);
                at = end + 1 + size;
            }
            return at == bytes.size();
        }

        const std::vector<std::string> &inputs_;
        const std::vector<flirt::ModuleMeta> &templates_;
        const size_t runWidth_;
        std::map<size_t, Child> children_;
        size_t next_ = 0;
        // How busy the cores are: one 200 ms sample before the first start,
        // then from the previous sample at each later start.
        flirt::CpuMeter meter_{ std::chrono::milliseconds(200), std::chrono::hours(24 * 365) };
};

// Created exclusively, as flirt-convert's --summary: never over an existing
// file, even one that appeared since parse_args() looked.
bool write_csv(const std::string &path, const std::string &text)
{
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0)
    {
        std::cerr << "error: cannot create " << path << ": " << std::strerror(errno) << "\n";
        return false;
    }
    bool ok = true;
    for (size_t done = 0; ok && done < text.size();)
    {
        const auto n = write(fd, text.data() + done, text.size() - done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            ok = false;
        else
            done += static_cast<size_t>(n);
    }
    if (close(fd) != 0)
        ok = false;
    if (!ok)
        std::cerr << "error: cannot write " << path << "\n";
    return ok;
}

}

int main(int argc, char *argv[])
{
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "-h" || std::string(argv[i]) == "--help")
        {
            usage(argv[0]);
            return 0;
        }

    Options opt;
    try
    {
        opt = parse_args(argc, argv);
    }
    catch (const usage_error &e)
    {
        std::cerr << "error: " << e.what() << "\n";
        if (e.showUsage)
            usage(argv[0]);
        return 2;
    }

    std::vector<flirt::ModuleMeta> templates;
    try
    {
        const auto templatesDir = own_prefix() + "/templates";
        templates = flirt::read_module_templates(QString::fromStdString(templatesDir));
        if (templates.empty())
            throw std::runtime_error("no mvme module templates found in " + templatesDir);
    }
    catch (const std::exception &e)
    {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }

    const size_t count = opt.inputs.size();
    const bool table = !opt.json && opt.csvPath != "-" && count > 1;
    const bool details = !opt.json && opt.csvPath != "-" && count == 1;
    size_t runWidth = 3;
    for (const auto &input: opt.inputs)
        runWidth = std::max(runWidth, flirt::run_id_from_path(input).size());
    runWidth += 2;
    if (table)
        std::cout << table_header(runWidth) << "\n" << std::flush;

    std::string csv = std::string(CsvHeader) + "\r\n";
    size_t unreadable = 0;
    // Several listfiles: up to the limit (CpuMeter) read at once, each in a child
    // process (Reader), printed in the order given once read.
    Reader reader(opt.inputs, templates, runWidth);
    for (size_t i = 0; i < count; ++i)
    {
        if (count > 1)
            reader.start_more(i);
        Result result;
        if (!reader.collect(i, result))
        {
            // On this thread: one listfile, a limit of 1, or a child that failed.
            const auto info = read_info(opt.inputs[i], templates);
            result = result_of(info, runWidth);
            if (details)
                print_details(info);
        }
        unreadable += result.unreadable;
        if (opt.json)
            std::cout << result.jsonLine;
        else if (table)
            std::cout << result.tableRow;
        if (!opt.csvPath.empty())
            csv += result.csvRow;
        std::cout << std::flush;
    }

    if (table)
        std::cout << count << " listfiles: " << count - unreadable << " read, " << unreadable << " unreadable\n";

    int exitCode = unreadable ? 1 : 0;
    if (opt.csvPath == "-")
        std::cout << csv;
    else if (!opt.csvPath.empty() && !write_csv(opt.csvPath, csv))
        exitCode = 1;
    std::cout << std::flush;
    return exitCode;
}
