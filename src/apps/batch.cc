// Batch parent of flirt-convert (docs/design.md, "The batch").
//
// Each listfile is converted by a child process, this same executable with
// --convert-one, so every conversion has fresh ROOT state (gROOT, gRandom),
// exactly like a fresh `mvme_root_client --single-run`. The parent itself never
// initializes ROOT. Outputs are never deleted or overwritten.

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
#include "common/names.h"
#include "common/platform.h"
#include "flirt/listfile.h"

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
    std::cerr << "usage: " << argv0 << " <input>... -o <outdir> [-j N]"
              << " [--dither-offset <N>]"
              << " [--accept-damaged-input]\n"
              << "\n"
              << "Converts mvme MVLC listfile archives into <runId>_raw.root, <runId>_histos.root\n"
              << "and a report <runId>.flirt.json in <outdir>. Tested against mvme_root_client\n"
              << "--single-run for recordings of mvme 1.16.2-1.18.0 with one VME event, no event\n"
              << "builder, and the modules mdpp32_scp, mdpp32_qdc and mvlc_event_stamper (README,\n"
              << "\"Tested with\"); for anything else, compare one run against it first.\n"
              << "\n"
              << "  -o, --output-dir    existing directory for the outputs\n"
              << "  -j N                convert up to N listfiles at once (default: 1, one run at\n"
              << "                      a time)\n"
              << "  --dither-offset     skip N draws of TRandom3(4357) first; 0 matches a fresh\n"
              << "                      mvme_root_client --single-run (default: 0)\n"
              << "  --accept-damaged-input\n"
              << "                      convert and publish even if integrity checks fail; the\n"
              << "                      report lists the failed checks\n"
              << "\n"
              << "Exit codes: 0 all converted, 1 a conversion failed, 2 usage, 3 unsupported\n"
              << "input, 130 interrupted.\n";
}

struct Options
{
    std::vector<std::string> inputs;
    std::string outDir;
    unsigned jobs = 1;
    uint64_t ditherOffset = 0;
    bool acceptDamagedInput = false;
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

Options parse_args(int argc, char *argv[])
{
    Options opt;

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
        else if (arg == "--dither-offset")
        {
            opt.ditherOffset = parse_number(arg, value());
        }
        else if (arg == "--accept-damaged-input") opt.acceptDamagedInput = true;
        else if (!arg.empty() && arg[0] == '-') throw usage_error("unknown option " + arg, true);
        else opt.inputs.push_back(arg);
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

    return opt;
}

std::string raw_path(const Options &opt, const Run &run) { return opt.outDir + "/" + run.runId + "_raw.root"; }
std::string histos_path(const Options &opt, const Run &run) { return opt.outDir + "/" + run.runId + "_histos.root"; }
std::string report_path(const Options &opt, const Run &run) { return opt.outDir + "/" + run.runId + ".flirt.json"; }

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
        run.note = "cannot create a temporary file in " + opt.outDir + ": "
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

// A line of the batch's text, on stderr.
using Say = std::function<void(const std::string &)>;

// A child has exited: pass its output on as messages, prefixed with the run
// ID, and classify. The result messages are say_result()'s.
void finish(Run &run, int status, bool stopping, const Options &opt, const Say &say)
{
    run.seconds = seconds_since(run.start);
    const auto log = read_log(run.log);
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

// The result messages.
void say_result(const Run &run, const Say &say)
{
    std::ostringstream text;
    text << run.runId << ": " << run.result << (run.note.empty() ? "" : " (" + run.note + ")");
    text << ", " << run.seconds << " s";
    say(text.str());
}

}

int run_batch(int argc, char *argv[])
{
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
        return 2;
    }

    // From here on the text goes to stderr, a line at a time.
    const Say say = [](const std::string &text) { std::cerr << text << "\n"; };
    const Say &tell = say;

    // The runs to launch, in input order.
    std::deque<Run *> queue;
    for (auto &run: runs)
        queue.push_back(&run);

    // At most opt.jobs children at a time.
    bool stopping = false;
    std::vector<Run *> running;

    auto handle_signal = [&](int sig)
    {
        if (sig == SIGCHLD || stopping)
            return;
        stopping = true;
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
                say(run.runId + ": failed (" + run.note + ")");
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

    return exitCode;
}

}
