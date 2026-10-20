// Batch parent of flirt-convert (docs/design.md, "The batch").
//
// Each listfile is converted by a child process, this same executable with
// --convert-one, so every conversion has fresh ROOT state (gROOT, gRandom),
// exactly like a fresh `mvme_root_client --single-run`. The parent itself never
// initializes ROOT. Outputs are never deleted or overwritten.

#include <RVersion.h>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include <spawn.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "batch.h"
#include "common/failures.h"
#include "common/names.h"
#include "common/platform.h"
#include "common/utf8.h"
#include "flirt/listfile.h"
#include "flirt_version.h"

extern char **environ;

namespace flirt
{

namespace
{

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

// Bad arguments: exit code 2 before anything starts.
struct usage_error: std::runtime_error
{
    bool showUsage;

    explicit usage_error(const std::string &what, bool showUsage_ = false)
        : std::runtime_error(what), showUsage(showUsage_)
    { }
};

void usage(const char *argv0)
{
    std::cerr << "usage: " << argv0 << " <input>... -o <outdir> [-j N] [--list <file>]"
              << " [--summary <file.json>]"
              << " [--dither-offset <N>]"
              << " [--accept-damaged-input] [--json-events]\n"
              << "\n"
              << "Converts mvme MVLC listfile archives into <runId>_raw.root, <runId>_histos.root\n"
              << "and a report <runId>.flirt.json in <outdir>. Tested against mvme_root_client\n"
              << "--single-run for recordings of mvme 1.16.2-1.18.0 with one VME event, no event\n"
              << "builder, and the modules mdpp32_scp, mdpp32_qdc and mvlc_event_stamper (README,\n"
              << "\"Tested with\"); for anything else, compare one run against it first.\n"
              << "An input is a listfile archive or a directory, which gives its *.zip files\n"
              << "(not recursive, in natural order).\n"
              << "\n"
              << "  -o, --output-dir    existing directory for the outputs\n"
              << "  -j N                convert up to N listfiles at once (default: 1, one run at\n"
              << "                      a time)\n"
              << "  --list <file>       inputs, one per line; blank lines and lines starting\n"
              << "                      with # are ignored (repeatable)\n"
              << "  --summary <file>    also write the summary as JSON to this new file (exit\n"
              << "                      code 1 if it cannot be written)\n"
              << "  --dither-offset     skip N draws of TRandom3(4357) first; 0 matches a fresh\n"
              << "                      mvme_root_client --single-run (default: 0)\n"
              << "  --accept-damaged-input\n"
              << "                      convert and publish even if integrity checks fail; the\n"
              << "                      report lists the failed checks\n"
              << "  --json-events       stdout: one JSON event per line for programs, instead of\n"
              << "                      the summary lines\n"
              << "\n"
              << "Exit codes: 0 all converted, 1 a conversion failed or the --summary file could\n"
              << "not be written, 2 usage, 3 unsupported input, 130 interrupted.\n";
}

struct Options
{
    std::vector<std::string> inputs;
    std::string outDir;
    unsigned jobs = 1;
    std::string summaryPath;
    uint64_t ditherOffset = 0;
    bool acceptDamagedInput = false;
    bool jsonEvents = false;    // --json-events: stdout carries JSON events only
};

struct Run
{
    std::string input, runId;
    std::string result;     // empty: to be converted
    std::string note;
    int exitCode = -1;      // -1: no child exit code
    double seconds = 0;
    pid_t pid = 0;
    FILE *log = nullptr;    // the child's stdout and stderr
    Clock::time_point start;
    bool convert = false;   // this batch converts it (not skipped or refused)
    bool diskFull = false;  // the child said "No space left on device"
};

const char *const Results[] = { "converted", "failed", "unsupported", "interrupted", "not-started" };

double seconds_since(Clock::time_point t0)
{
    return std::chrono::duration<double>(Clock::now() - t0).count();
}

bool exists(const std::string &path)
{
    struct stat st;
    return lstat(path.c_str(), &st) == 0;
}

uint64_t parse_number(const std::string &opt, const std::string &text)
{
    if (text.empty() || !std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c); }))
        throw usage_error(opt + " needs a non-negative integer, got '" + text + "'");
    try
    {
        return std::stoull(text);
    }
    catch (const std::exception &)
    {
        throw usage_error(opt + " value out of range: " + text);
    }
}

// A listfile or a directory's listfiles (add_listfile_input, common/names.h).
void add_input(const std::string &path, std::vector<std::string> &inputs)
{
    try
    {
        add_listfile_input(path, inputs);
    }
    catch (const std::runtime_error &e)
    {
        throw usage_error(e.what());
    }
}

// A name FLIRT handles that is not standard (common/names.h): a usage error.
void check_name(const std::string &option, const std::string &path)
{
    if (const auto refusal = name_refusal(own_name(path)); !refusal.empty())
        throw usage_error(option + " " + refusal);
}

void add_list(const std::string &listFile, std::vector<std::string> &inputs)
{
    check_name("--list", listFile);
    std::ifstream in(listFile);
    if (!in)
        throw usage_error("cannot read list file " + listFile);

    std::string line;
    while (std::getline(in, line))
    {
        const auto first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos || line[first] == '#')
            continue;
        const auto last = line.find_last_not_of(" \t\r");
        add_input(line.substr(first, last - first + 1), inputs);
    }
}

Options parse_args(int argc, char *argv[])
{
    Options opt;
    // Expanded after all options are read.
    std::vector<std::pair<bool, std::string>> sources;    // (a --list file, path)

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc)
                throw usage_error(arg + " needs a value");
            return argv[++i];
        };

        if (arg == "-o" || arg == "--output-dir") opt.outDir = value();
        else if (arg == "-j")
        {
            auto jobs = parse_number(arg, value());
            if (jobs < 1 || jobs > 1024)
                throw usage_error("-j must be between 1 and 1024");
            opt.jobs = static_cast<unsigned>(jobs);
        }
        else if (arg == "--list") sources.emplace_back(true, value());
        else if (arg == "--summary") opt.summaryPath = value();
        else if (arg == "--dither-offset")
        {
            opt.ditherOffset = parse_number(arg, value());
        }
        else if (arg == "--accept-damaged-input") opt.acceptDamagedInput = true;
        else if (arg == "--json-events") opt.jsonEvents = true;
        else if (!arg.empty() && arg[0] == '-') throw usage_error("unknown option " + arg, true);
        else sources.emplace_back(false, arg);
    }

    for (const auto &[isList, path]: sources)
    {
        if (isList)
            add_list(path, opt.inputs);
        else
            add_input(path, opt.inputs);
    }
    if (opt.inputs.empty())
        throw usage_error("no inputs", true);

    struct stat st;
    if (opt.outDir.empty())
        throw usage_error("no output directory (-o)");
    if (stat(opt.outDir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
        throw usage_error("output directory does not exist: " + opt.outDir);
    char buf[PATH_MAX];
    if (!realpath(opt.outDir.c_str(), buf))
        throw usage_error("cannot resolve " + opt.outDir);
    opt.outDir = buf;
    // The folder the outputs go in, by its real name ("-o ." is the working
    // folder's); the folders above it may hold any name.
    if (const auto refusal = name_refusal(own_name(opt.outDir)); !refusal.empty() && opt.outDir != "/")
        throw usage_error("output directory " + refusal);

    if (!opt.summaryPath.empty())
        check_name("--summary", opt.summaryPath);
    if (!opt.summaryPath.empty() && exists(opt.summaryPath))
        throw usage_error("summary file exists: " + opt.summaryPath);

    return opt;
}

std::string raw_path(const Options &opt, const Run &run) { return opt.outDir + "/" + run.runId + "_raw.root"; }
std::string histos_path(const Options &opt, const Run &run) { return opt.outDir + "/" + run.runId + "_histos.root"; }
std::string report_path(const Options &opt, const Run &run) { return opt.outDir + "/" + run.runId + ".flirt.json"; }

// What the conversion report records as the build (flirt_convert.cc). ROOT's
// version from the headers, as gROOT->GetVersion() gives ROOT_RELEASE.
json build_identity()
{
    return { {"version", FLIRT_GIT_DESCRIBE}, {"mvme", MVME_GIT_DESCRIBE}, {"root", ROOT_RELEASE} };
}

std::string signal_name(int sig)
{
    switch (sig)
    {
        case SIGINT: return "SIGINT";
        case SIGTERM: return "SIGTERM";
        case SIGKILL: return "SIGKILL";
        case SIGSEGV: return "SIGSEGV";
        case SIGABRT: return "SIGABRT";
        case SIGBUS: return "SIGBUS";
        case SIGFPE: return "SIGFPE";
        case SIGILL: return "SIGILL";
        case SIGHUP: return "SIGHUP";
        case SIGPIPE: return "SIGPIPE";
        case SIGQUIT: return "SIGQUIT";
        default: return "signal " + std::to_string(sig);
    }
}

// An unlinked read-write file in the output folder dir, closed on exec, so
// nothing is written outside it (docs/design.md, "Nothing written outside
// the FLIRT folder"): O_TMPFILE where it exists and the file system takes it
// (the file never has a name), else a ".flirt-tmp-XXXXXX" file unlinked at
// once (macOS). -1 with errno set on failure.
int temp_file_in(const std::string &dir)
{
#ifdef O_TMPFILE
    const int tmpFd = open(dir.c_str(), O_TMPFILE | O_RDWR | O_CLOEXEC, 0600);
    if (tmpFd >= 0)
        return tmpFd;
    // EOPNOTSUPP, EISDIR (a kernel without it), EINVAL...: the named way.
#endif
    std::string path = dir + "/.flirt-tmp-XXXXXX";
    const int fd = mkstemp(path.data());
    if (fd < 0)
        return -1;
    if (unlink(path.c_str()) != 0)
    {
        const int err = errno;
        close(fd);
        errno = err;
        return -1;
    }
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
}

// Starts `<exe> --convert-one <input> -o <outDir> [conversion options]`, its
// stdout and stderr going to an unlinked temporary file in <outDir>. False on
// failure.
bool launch(Run &run, const Options &opt, const char *argv0)
{
    run.start = Clock::now();
    const int logFd = temp_file_in(opt.outDir);
    run.log = logFd >= 0 ? fdopen(logFd, "w+") : nullptr;
    if (logFd >= 0 && !run.log)
    {
        const int err = errno;
        close(logFd);
        errno = err;
    }
    if (!run.log)
    {
        run.note = "cannot create a temporary file in " + printable_text(opt.outDir) + ": "
                   + std::strerror(errno);
        return false;
    }
    const int fd = fileno(run.log);
    fcntl(fd, F_SETFD, FD_CLOEXEC); // other children must not inherit it

    std::vector<std::string> args = { argv0, "--convert-one", run.input, "-o", opt.outDir };
    args.insert(args.end(), { "--dither-offset", std::to_string(opt.ditherOffset) });
    if (opt.acceptDamagedInput)
        args.push_back("--accept-damaged-input");

    std::vector<char *> argv;
    for (auto &a: args)
        argv.push_back(a.data());
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, fd, 1);
    posix_spawn_file_actions_adddup2(&actions, fd, 2);

    // The parent blocks these signals to wait for them; the child starts with
    // none blocked and default actions.
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t none, defaults;
    sigemptyset(&none);
    sigemptyset(&defaults);
    for (int sig: { SIGINT, SIGTERM, SIGCHLD })
        sigaddset(&defaults, sig);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &defaults);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);

    const std::string exe = self_exe();
    const int err = posix_spawn(&run.pid, exe.c_str(), &actions, &attr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);

    if (err)
    {
        fclose(run.log);
        run.log = nullptr;
        run.pid = 0;
        run.note = std::string("cannot start the conversion: ") + std::strerror(err);
        return false;
    }
    return true;
}

std::string read_log(FILE *log)
{
    std::string text;
    const int fd = fileno(log);
    char buf[65536];
    lseek(fd, 0, SEEK_SET);
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        text.append(buf, static_cast<size_t>(n));
    return text;
}

// A line flirt-convert prints on stderr, and with --json-events sends as a
// message event (README "Events for programs").
using Say = std::function<void(const std::string &)>;

// A child has exited: pass its output on as messages, prefixed with the run
// ID, and classify. The result messages are say_result()'s.
void finish(Run &run, int status, bool stopping, const Options &opt, const Say &say)
{
    run.seconds = seconds_since(run.start);
    const auto log = read_log(run.log);
    run.diskFull = log.find("No space left on device") != std::string::npos;
    std::istringstream output(log);
    fclose(run.log);
    run.log = nullptr;
    run.pid = 0;

    // The note: the first error, refusal or failed check the child printed.
    std::string reason, line;
    while (std::getline(output, line))
    {
        say(run.runId + ": " + line);
        if (reason.empty()
            && (line.rfind("error: ", 0) == 0 || line.rfind("refused: ", 0) == 0
                || (line.size() > 3 && line.compare(0, 3, "  I") == 0 && std::isdigit(static_cast<unsigned char>(line[3])))))
            reason = line.substr(line.find_first_not_of(' '));
    }

    if (WIFEXITED(status))
    {
        run.exitCode = WEXITSTATUS(status);
        if (run.exitCode == 0)
        {
            run.result = "converted";
            try
            {
                std::ifstream in(report_path(opt, run));
                run.note = "status " + json::parse(in).at("status").get<std::string>();
            }
            catch (const std::exception &)
            {
                run.note = "report unreadable";
            }
        }
        else if (run.exitCode == 3)
        {
            run.result = "unsupported";
            run.note = reason;
        }
        else if (run.exitCode == 130 && stopping)
        {
            run.result = "interrupted";
            run.note = "nothing was published";
        }
        else
        {
            run.result = "failed";
            run.note = reason.empty() ? "exit code " + std::to_string(run.exitCode) : reason;
        }
    }
    else
    {
        const int sig = WTERMSIG(status);
        // Before it ran, there were none of its outputs (the child refuses
        // existing ones). The report is published last, after the other
        // outputs are on disk (RootWriter::publish): with it the set is
        // complete, without it any of its outputs there are an incomplete set.
        std::string report;
        try
        {
            std::ifstream in(report_path(opt, run));
            if (in)
                report = json::parse(in).at("status").get<std::string>();
        }
        catch (const std::exception &)
        {
        }
        std::vector<std::string> present;
        for (const auto &path: { raw_path(opt, run), histos_path(opt, run) })
            if (exists(path))
                present.push_back(path.substr(path.rfind('/') + 1));
        if (!report.empty())
        {
            run.result = "converted";
            run.note = "status " + report;
            say(run.runId + ": killed by " + signal_name(sig)
                + " after it had published everything; its outputs and report are complete");
        }
        else if (!present.empty())
        {
            std::string names;
            for (size_t i = 0; i < present.size(); ++i)
                names += (i == 0 ? "" : i + 1 == present.size() ? " and " : ", ") + present[i];
            run.result = "failed";
            run.note = "killed by " + signal_name(sig) + " while publishing: " + names + " in " + opt.outDir
                + " are without their report, an incomplete set";
        }
        else
        {
            // Killed by a stop signal before its own handler was installed,
            // i.e. before it created any output.
            run.result = stopping && (sig == SIGINT || sig == SIGTERM || sig == SIGHUP) ? "interrupted" : "failed";
            run.note = "killed by " + signal_name(sig) + (reason.empty() ? "" : "; " + reason);
        }
    }
}

// A run that ended badly in plain words (common/failures.h): "<headline>
// (<check>: <why>)"; empty for the other results, which keep their note.
flirt::Failure failure_of(const Run &run)
{
    if (run.result != "failed" && run.result != "unsupported" && run.result != "interrupted")
        return {};
    return flirt::explain_failure(run.result, run.note, run.diskFull, flirt::FailureFor::Cli);
}

std::string reason_text(const flirt::Failure &f)
{
    const auto why = f.check.empty() ? f.why : f.check + (f.why.empty() ? "" : ": " + f.why);
    return f.headline + (why.empty() ? "" : " (" + why + ")");
}

// The result messages.
void say_result(const Run &run, const Say &say)
{
    std::ostringstream text;
    const auto f = failure_of(run);
    if (f.headline.empty())
        text << run.runId << ": " << run.result << (run.note.empty() ? "" : " (" + run.note + ")");
    else
        text << run.runId << ": " << run.result << ": " << reason_text(f);
    text << ", " << run.seconds << " s";
    say(text.str());
    if (!f.todo.empty())
        say(run.runId + ": to do: " + f.todo);
}

// The summary's "options" (also the --json-events start event's).
json options_json(const Options &opt)
{
    return { {"outputDir", opt.outDir}, {"jobs", opt.jobs},
             {"ditherOffset", opt.ditherOffset},
             {"acceptDamagedInput", opt.acceptDamagedInput} };
}

// A run in the summary's "results" (also a --json-events result event).
json result_json(const Options &opt, const Run &run)
{
    const auto report = report_path(opt, run);
    return { {"input", run.input}, {"runId", run.runId}, {"result", run.result},
             {"exitCode", run.exitCode < 0 ? json(nullptr) : json(run.exitCode)},
             {"seconds", run.seconds},
             {"report", exists(report) ? json(report) : json(nullptr)},
             {"note", run.note} };
}

json make_summary(const Options &opt, const std::deque<Run> &runs,
                  const std::map<std::string, size_t> &totals, double wall, int exitCode, int stoppedBy)
{
    json results = json::array();
    for (const auto &run: runs)
    {
        auto result = result_json(opt, run);
        results.push_back(result);
    }

    json jtotals = { {"runs", runs.size()}, {"seconds", wall} };
    for (const char *name: Results)
        jtotals[name] = totals.at(name);

    return {
        {"flirt", build_identity()},
        {"options", options_json(opt)},
        {"results", results},
        {"totals", jtotals},
        {"exitCode", exitCode},
        {"stoppedBy", stoppedBy ? json(signal_name(stoppedBy)) : json(nullptr)} };
}

// Created exclusively: never over an existing file. Invalid UTF-8 is
// replaced (common/utf8.h).
bool write_summary(const Options &opt, const json &summary, const Say &tell)
{
    const auto contents = dump_valid_utf8(summary, 2) + "\n";
    const auto fd = open(opt.summaryPath.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0)
    {
        tell("error: cannot create " + opt.summaryPath + ": " + std::strerror(errno));
        return false;
    }
    bool ok = write(fd, contents.data(), contents.size()) == static_cast<ssize_t>(contents.size());
    ok = (close(fd) == 0) && ok;
    if (!ok)
        tell("error: error writing " + opt.summaryPath);
    return ok;
}

// flirt-convert --json-events (README "Events for programs"): one compact
// JSON object per line on stdout, written and flushed when it happens, with
// invalid UTF-8 replaced (common/utf8.h).
class Events
{
    public:
        explicit Events(bool on): on_(on) { }
        bool on() const { return on_; }

        void send(const std::string &event, const json &fields = json::object())
        {
            if (!on_)
                return;
            json object = fields;
            object["event"] = event;
            std::cout << dump_valid_utf8(object) << "\n" << std::flush;
        }

    private:
        bool on_;
};

}

int run_batch(int argc, char *argv[])
{
    const auto t0 = Clock::now();

    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "-h" || std::string(argv[i]) == "--help")
        {
            usage(argv[0]);
            return 0;
        }

    // Stop signals and child exits are waited for with sigtimedwait()
    // (SignalWaiter, common/platform.h); blocked from the start, so a Ctrl-C
    // during the checks is not lost. SIGHUP
    // (terminal or ssh connection closed) and SIGPIPE (stderr or stdout closed,
    // e.g. `ssh host flirt-convert ...` without a terminal after the
    // connection dropped) stop the batch too, unless ignored at startup (nohup).
    sigset_t waited;
    sigemptyset(&waited);
    for (int sig: { SIGINT, SIGTERM, SIGCHLD })
        sigaddset(&waited, sig);
    for (int sig: { SIGHUP, SIGPIPE })
    {
        struct sigaction old = {};
        if (sigaction(sig, nullptr, &old) == 0 && old.sa_handler != SIG_IGN)
            sigaddset(&waited, sig);
    }
    sigprocmask(SIG_BLOCK, &waited, nullptr);
    // An ignored signal is discarded, not queued: a shell script starts
    // background commands with SIGINT ignored, and the batch must still stop.
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    SignalWaiter signalWaiter(waited);

    Options opt;
    // A deque: `running` and `queue` point into it while runs are added.
    std::deque<Run> runs;
    // Also for a usage error, so it is known before the arguments are checked.
    Events events(std::any_of(argv + 1, argv + argc, [](const char *a) { return std::string(a) == "--json-events"; }));

    try
    {
        opt = parse_args(argc, argv);

        // Output names derive from the run ID (listfile.h), so it must be unique.
        std::map<std::string, std::vector<std::string>> byRunId;
        std::vector<std::string> order;
        for (const auto &input: opt.inputs)
        {
            Run run;
            run.input = input;
            run.runId = run_id_from_path(input);
            if (byRunId[run.runId].empty())
                order.push_back(run.runId);
            byRunId[run.runId].push_back(input);
            runs.push_back(run);
        }

        std::string clashes;
        for (const auto &runId: order)
            if (byRunId[runId].size() > 1)
            {
                clashes += "\n  " + runId + ":";
                for (const auto &input: byRunId[runId])
                    clashes += " " + input;
            }
        if (!clashes.empty())
            throw usage_error("several inputs have the same run ID, so their outputs would clash:" + clashes);
    }
    catch (const usage_error &e)
    {
        std::cerr << "error: " << e.what() << "\n";
        if (e.showUsage)
            usage(argv[0]);
        events.send("usage-error", { {"message", valid_utf8(e.what())} });
        return 2;
    }
    events.send("start", { {"flirt", build_identity()}, {"options", options_json(opt)} });

    // From here on the text goes to stderr, a line at a time, and with
    // --json-events each line is also a message event: a line about one run
    // ("<runId>: ...") carries its run ID and the text after that prefix.
    const Say say = [&](const std::string &text)
    {
        std::cerr << text << "\n";
        if (!events.on())
            return;
        const Run *about = nullptr;
        for (const auto &run: runs)
            if (text.size() > run.runId.size() + 1 && text.compare(0, run.runId.size() + 2, run.runId + ": ") == 0
                && (!about || run.runId.size() > about->runId.size()))
                about = &run;
        events.send("message", { {"runId", about ? json(about->runId) : json(nullptr)},
                                 {"text", valid_utf8(about ? text.substr(about->runId.size() + 2) : text)} });
    };
    const Say &tell = say;

    // True if the run is to be converted.
    auto classify = [&](Run &run) -> bool
    {
        // A listfile's name becomes its run's file names: only a standard
        // one is converted (common/names.h), each other run goes on.
        if (const auto refusal = name_refusal(own_name(run.input)); !refusal.empty())
        {
            // Nothing is written under it; its lines show it safely.
            run.runId = printable_text(run.runId);
            run.result = "unsupported";
            run.note = "refused: " + refusal;
            say(run.runId + ": " + run.note);
            say_result(run, say);
            return false;
        }

        run.convert = true;
        return true;
    };

    // The runs to launch, in input order.
    std::deque<Run *> queue;
    for (auto &run: runs)
        if (classify(run))
            queue.push_back(&run);

    // The plan, then the results of the runs not converted.
    if (events.on())
    {
        json plan = json::array();
        for (const auto &run: runs)
            plan.push_back({ {"runId", run.runId}, {"input", run.input},
                             {"action", run.convert ? "convert" : run.result} });
        events.send("plan", { {"runs", plan} });
        for (const auto &run: runs)
            if (!run.convert)
                events.send("result", result_json(opt, run));
    }

    // At most opt.jobs children at a time.
    bool stopping = false;
    int stoppedBy = 0;
    std::vector<Run *> running;

    auto handle_signal = [&](int sig)
    {
        if (sig == SIGCHLD || stopping)
            return;
        stopping = true;
        stoppedBy = sig;
        const std::string why = sig == SIGHUP ? ", terminal closed" : sig == SIGPIPE ? ", output closed" : "";
        tell("stopping (" + signal_name(sig) + why + "): no new conversions; waiting for "
             + std::to_string(running.size()) + " running conversion(s) to end; nothing is half-published");
        for (auto run: running)
            kill(run->pid, SIGTERM);
    };

    auto wait_signal = [&](long nanoseconds) -> bool
    {
        timespec timeout = { nanoseconds / 1000000000, nanoseconds % 1000000000 };
        const int sig = signalWaiter.wait(timeout);
        if (sig > 0)
            handle_signal(sig);
        return sig > 0;
    };

    while (true)
    {
        while (wait_signal(0)) { }

        for (auto it = running.begin(); it != running.end();)
        {
            int status = 0;
            if (waitpid((*it)->pid, &status, WNOHANG) == (*it)->pid)
            {
                auto &run = **it;
                finish(run, status, stopping, opt, say);
                say_result(run, say);
                events.send("result", result_json(opt, run));
                it = running.erase(it);
            }
            else
                ++it;
        }

        while (!stopping && running.size() < opt.jobs && !queue.empty())
        {
            auto &run = *queue.front();
            queue.pop_front();

            say(run.runId + ": converting " + run.input);
            if (launch(run, opt, argv[0]))
            {
                running.push_back(&run);
            }
            else
            {
                run.result = "failed";
                say(run.runId + ": failed: " + reason_text(failure_of(run)));
                say(run.runId + ": to do: " + failure_of(run).todo);
                events.send("result", result_json(opt, run));
            }
        }

        if (running.empty() && (stopping || queue.empty()))
            break;

        // A quarter second while conversions run.
        long wait = 250000000;
        wait_signal(wait);
    }

    for (auto &run: runs)
        if (run.result.empty())
        {
            run.result = "not-started";
            events.send("result", result_json(opt, run));
        }

    std::map<std::string, size_t> totals;
    for (const char *name: Results)
        totals[name] = 0;
    for (const auto &run: runs)
        ++totals[run.result];

    int exitCode = 0;
    if (stopping)
        exitCode = 130;
    else if (totals["failed"])
        exitCode = 1;
    else if (totals["unsupported"])
        exitCode = 3;

    const double wall = seconds_since(t0);

    auto summary = make_summary(opt, runs, totals, wall, exitCode, stoppedBy);
    if (!opt.summaryPath.empty() && !write_summary(opt, summary, tell) && exitCode == 0)
        exitCode = 1;

    // The exit code differs from the summary's only if it could not be written.
    summary["exitCode"] = exitCode;
    events.send("end", { {"summary", summary} });
    return exitCode;
}

}
