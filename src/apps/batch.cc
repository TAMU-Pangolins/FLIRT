// Batch parent of flirt-convert (docs/design.md, "The batch").
//
// Each listfile is converted by a child process, this same executable with
// --convert-one, so every conversion has fresh ROOT state (gROOT, gRandom),
// exactly like a fresh `mvme_root_client --single-run`. The parent itself never
// initializes ROOT. Outputs are never deleted or overwritten: runs already
// converted with the same inputs, options and FLIRT build are skipped, runs
// with any other existing outputs are refused.

#include <QCryptographicHash>
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
#include <limits>
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
#include "batch_text.h"
#include "progress.h"

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
              << "Runs already converted with the same inputs, options and FLIRT build are\n"
              << "skipped; other existing outputs are never overwritten.\n"
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
              << "Exit codes: 0 all converted or skipped, 1 a conversion failed, outputs exist or\n"
              << "the --summary file could not be written, 2 usage, 3 unsupported input,\n"
              << "130 interrupted.\n";
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

// Why a run's existing outputs are not what converting it now would produce:
// the note (the summary's and the events') and the same in the words of the
// run's block. Empty if they are (the run is skipped).
struct Mismatch
{
    std::string note;       // "experimentName differs (report \"Experiment\", now \"Test\")"
    std::string header;     // after "not converted: ": "its files exist and the experiment name differs"
    std::string attention;  // the end's line: "its files exist with another experiment name"
    std::string why;        // "the report says \"Experiment\", this run asks for \"Test\""

    bool empty() const { return note.empty(); }
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
    int progressFd = -1;    // the child's progress lines (its fd 3)
    ProgressReader progress;
    Clock::time_point start;
    bool convert = false;   // this batch converts it (not skipped or refused)
    uint64_t bytes = 0;     // input size, for the progress estimate
    uint64_t key = 0;       // the launch's number, for the progress display
    bool diskFull = false;  // the child said "No space left on device"
    std::string eventPhase; // --json-events: the last progress event's phase and percent
    int eventPercent = -1;
    Clock::time_point eventTime;
    Mismatch why;           // outputs-exist: why, in the block's words
    std::vector<std::string> otherLines;    // ended badly: the child's lines the note does not hold
    std::string killedNote; // converted, then killed: said in its block
    RunBlock block;         // what was said about it (batch_text.h)
    bool said = false;      // its block was printed
};

const char *const Results[] = { "converted", "skipped", "outputs-exist", "failed",
                                "unsupported", "interrupted", "not-started" };

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
    return { {"version", FLIRT_GIT_DESCRIBE}, {"mvme", MVME_GIT_DESCRIBE}, {"root", ROOT_RELEASE},
             {"platform", build_platform()}, {"executableSha256", executable_sha256()} };
}
// A development build: every dirty tree of a commit, and every build without
// git, gives the same describe string.
bool development_build()
{
    const std::string version = FLIRT_GIT_DESCRIBE;
    const std::string dirty = "-dirty";
    return version.empty()
        || (version.size() >= dirty.size() && version.compare(version.size() - dirty.size(), dirty.size(), dirty) == 0);
}

// A mismatch said the same way in the header and the end's line.
Mismatch differs(const std::string &note, const std::string &what, const std::string &why)
{
    return { note, "its files exist " + what, "its files exist " + what, why };
}

// A setting that differs: "the <noun> differs", "with another <noun>".
Mismatch setting_differs(const std::string &note, const std::string &noun, const std::string &report,
                         const std::string &now)
{
    return { note, "its files exist and the " + noun + " differs", "its files exist with another " + noun,
             "the report says " + report + ", this run asks for " + now };
}

std::string file_name(const std::string &path)
{
    return path.substr(path.rfind('/') + 1);
}

Mismatch mismatch(const Options &opt, const Run &run)
{
    const auto reportPath = report_path(opt, run);
    json report;
    {
        std::ifstream in(reportPath);
        if (!in)
        {
            const auto note = "they have no report " + file_name(reportPath)
                + " (published last), so they are not a complete conversion: e.g. one stopped while publishing"
                  " (killed, or the machine went down)";
            return differs(note, "without their report", note);
        }
        try
        {
            report = json::parse(in);
        }
        catch (const std::exception &)
        {
            return differs("report " + reportPath + " cannot be parsed", "and their report cannot be read",
                           file_name(reportPath) + " cannot be parsed");
        }
    }

    auto at = [&](const char *pointer) -> json {
        try { return report.at(json::json_pointer(pointer)); }
        catch (const std::exception &) { return json(); }
    };

    if (at("/status") != "ok")
        return differs("the report's status is " + at("/status").dump(),
                       "from a conversion with status " + dump_valid_utf8(at("/status")),
                       "only a conversion with status \"ok\" is skipped");

    // The same build: FLIRT's describe string, mvme, ROOT and the platform;
    // for a development build also the same executable (README, "Skipping
    // and refusing").
    const auto build = build_identity();
    for (const char *key: { "version", "mvme", "root", "platform" })
        if (at(("/flirt/" + std::string(key)).c_str()) != build[key])
        {
            const auto note = std::string("made by another build (") + key + " "
                + dump_valid_utf8(at(("/flirt/" + std::string(key)).c_str())) + ", this is "
                + dump_valid_utf8(build[key]) + ")";
            return differs(note, "from another FLIRT build", note);
        }
    if (development_build() && (build["executableSha256"] == "" || at("/flirt/executableSha256") != build["executableSha256"]))
    {
        const auto note = "made by another flirt-convert executable (this development build, version "
            + dump_valid_utf8(build["version"]) + ", skips only outputs of the same executable)";
        return differs(note, "from another flirt-convert executable", note);
    }

    struct stat st;
    if (stat(run.input.c_str(), &st) != 0)
        return differs("cannot stat " + run.input, "and the input cannot be read", "cannot stat " + run.input);
    const timespec mtime = stat_mtime(st);
    const int64_t mtimeNs = static_cast<int64_t>(mtime.tv_sec) * 1000000000 + mtime.tv_nsec;
    if (at("/input/size") != json(static_cast<int64_t>(st.st_size)) || at("/input/mtimeNs") != json(mtimeNs))
    {
        const std::string note = "the input's size or modification time differs from the report's";
        return differs(note, "from another version of the input file", note);
    }

    if (at("/options/ditherOffset") != json(opt.ditherOffset))
        return setting_differs("ditherOffset differs (report " + dump_valid_utf8(at("/options/ditherOffset")) + ", now "
                               + std::to_string(opt.ditherOffset) + ")",
                               "dither offset", dump_valid_utf8(at("/options/ditherOffset")),
                               std::to_string(opt.ditherOffset));

    // The files against the report: the note names their paths, the block
    // their names (the folder is said once).
    auto files_differ = [](const std::string &path, const std::string &what)
    {
        return differs(path + " " + what, "and do not match their report", file_name(path) + " " + what);
    };
    const std::pair<std::string, const char *> files[] = {
        { raw_path(opt, run), "/output/rawBytes" }, { histos_path(opt, run), "/output/histosBytes" } };
    for (const auto &[path, pointer]: files)
    {
        if (stat(path.c_str(), &st) != 0)
            return files_differ(path, "is missing");
        if (at(pointer) != json(static_cast<int64_t>(st.st_size)))
            return files_differ(path, "size differs from the report's");
    }

    return {};
}

}

std::string build_platform()
{
#ifdef __APPLE__
    const std::string os = "macos";
#else
    const std::string os = "linux";
#endif
#if defined(__x86_64__)
    return os + "-x86_64";
#elif defined(__aarch64__) || defined(__arm64__)
    return os + "-arm64";
#else
    return os + "-unknown";
#endif
}

const std::string &executable_sha256()
{
    static const std::string hex = [] {
        std::ifstream in(self_exe(), std::ios::binary);
        if (!in)
            return std::string();
        QCryptographicHash hash(QCryptographicHash::Sha256);
        std::vector<char> buffer(1u << 20);
        while (in)
        {
            in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            if (in.gcount() > 0)
                hash.addData(buffer.data(), static_cast<int>(in.gcount()));
        }
        if (in.bad())
            return std::string();
        return hash.result().toHex().toStdString();
    }();
    return hex;
}

namespace
{


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
// stdout and stderr going to an unlinked temporary file in <outDir>, its
// progress lines to another one on its fd 3. False on failure.
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
    const int progressFile = run.log ? temp_file_in(opt.outDir) : -1;
    if (progressFile >= 0)
    {
        // Not fd 3: glibc 2.28's posix_spawn does not clear FD_CLOEXEC in a
        // dup2 whose source is its target.
        run.progressFd = fcntl(progressFile, F_DUPFD_CLOEXEC, 10);
        const int err = errno;
        close(progressFile);
        errno = err;
    }
    if (!run.log || run.progressFd < 0)
    {
        run.note = "cannot create a temporary file in " + printable_text(opt.outDir) + ": "
                   + std::strerror(errno);
        if (run.log)
            fclose(run.log);
        run.log = nullptr;
        return false;
    }
    run.progress = ProgressReader(run.progressFd);
    const int fd = fileno(run.log);
    fcntl(fd, F_SETFD, FD_CLOEXEC); // other children must not inherit it

    std::vector<std::string> args = { argv0, "--convert-one", run.input, "-o", opt.outDir,
                                      "--progress-fd", "3" };
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
    // After the log's: the log may be fd 3 here.
    posix_spawn_file_actions_adddup2(&actions, run.progressFd, 3);

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
        close(run.progressFd);
        run.progressFd = -1;
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

// A message event's text (--json-events; README "Events for programs"): the
// lines flirt-convert once printed on stderr, unchanged.
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
    close(run.progressFd);
    run.progressFd = -1;
    run.progress = ProgressReader();
    run.pid = 0;

    // The note: the first error, refusal or failed check the child printed.
    std::string reason, line;
    std::vector<std::string> lines;
    while (std::getline(output, line))
    {
        say(run.runId + ": " + line);
        lines.push_back(line);
        if (reason.empty()
            && (line.rfind("error: ", 0) == 0 || line.rfind("refused: ", 0) == 0
                || (line.size() > 3 && line.compare(0, 3, "  I") == 0 && std::isdigit(static_cast<unsigned char>(line[3])))))
            reason = line.substr(line.find_first_not_of(' '));
    }
    // For a run that ends badly: what the note leaves out, e.g. a second
    // failed check (its block's why rows).
    run.otherLines.clear();
    for (const auto &l: lines)
    {
        const auto text = l.substr(std::min(l.size(), l.find_first_not_of(' ')));
        if (!text.empty() && text != reason && text != "failed checks:" && text.rfind("nothing was published", 0) != 0
            && text != "interrupted: nothing was published")
            run.otherLines.push_back(text);
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
        // Before it ran, there were none of its outputs (classify). The
        // report is published last, after the other outputs are on disk
        // (RootWriter::publish): with it the set is complete, without it
        // any of its outputs there are an incomplete set.
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
            run.killedNote = "killed by " + signal_name(sig)
                + " after it had published everything; its outputs and report are complete";
            say(run.runId + ": " + run.killedNote);
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

// "a, b and c".
std::string listed(const std::vector<std::string> &items)
{
    std::string text;
    for (size_t i = 0; i < items.size(); ++i)
        text += (i == 0 ? "" : i + 1 == items.size() ? " and " : ", ") + items[i];
    return text;
}

// What is said about a run, from its result, its report (converted) or its
// failure (common/failures.h); README "What flirt-convert prints".
RunBlock block_of(const Run &run, const Options &opt)
{
    if (run.result == "converted")
    {
        RunBlock b;
        try
        {
            std::ifstream in(report_path(opt, run));
            b = converted_block(run.runId, run.seconds, json::parse(in));
        }
        catch (const std::exception &)
        {
            b = RunBlock{ run.runId, "✓", Tone::Ok, "converted", " in " + elapsed_text(run.seconds), {}, 0, {} };
            add_warning(b, "its report " + run.runId + ".flirt.json cannot be read");
            b.attention = "its report cannot be read";
        }
        if (!run.killedNote.empty())
            b.rows.push_back({ "note", run.killedNote, run.killedNote });
        return b;
    }

    RunBlock b;
    b.runId = run.runId;
    auto row = [&](const std::string &label, const std::string &text)
    {
        if (!text.empty())
            b.rows.push_back({ label, text, label == "note" ? text : std::string() });
    };
    const auto f = failure_of(run);
    if (run.result == "skipped")
    {
        b.mark = "–", b.tone = Tone::Skip, b.outcome = "skipped";
        b.rest = ": already converted with the same input, options and FLIRT build";
        return b;
    }
    if (run.result == "outputs-exist")
    {
        b.mark = "!", b.tone = Tone::Warning, b.outcome = "not converted";
        b.rest = ": " + run.why.header;
        row("why", run.why.why);
        // Only the run's files that are there, by their endings.
        std::vector<std::string> present;
        for (const auto &path: { raw_path(opt, run), histos_path(opt, run), report_path(opt, run) })
            if (exists(path))
                present.push_back(path.substr(opt.outDir.size() + 1 + run.runId.size()));
        row("to do", "delete " + listed(present) + " of this run to convert it again");
        b.attention = run.why.attention;
        return b;
    }
    if (run.result == "not-started")
    {
        b.mark = "–";
        b.tone = Tone::Plain;
        b.outcome = "not started";
        b.rest = run.note.empty() ? "" : ": " + run.note;
        b.attention = b.outcome + (run.note.empty() ? "" : ": " + run.note);
        return b;
    }

    // Failed, unsupported, interrupted: the plain words, then what the note
    // leaves out (a second failed check, say), at most a few lines.
    const bool interrupted = run.result == "interrupted";
    b.mark = interrupted ? "!" : "✗";
    b.tone = interrupted ? Tone::Warning : Tone::Error;
    b.outcome = interrupted ? "not converted" : run.result;
    const bool incomplete = run.note.find("without their report") != std::string::npos;
    b.rest = (run.result == "failed" ? " in " + elapsed_text(run.seconds) : std::string()) + ": " + f.headline
        + (run.result == "failed" && !incomplete ? "; nothing was published" : "");
    row("why", f.why + (f.check.empty() ? "" : (f.why.empty() ? "" : " ") + std::string("(check ") + f.check + ")"));
    const size_t shown = 3;
    for (size_t i = 0; i < run.otherLines.size() && i < shown; ++i)
    {
        const auto &l = run.otherLines[i];
        const auto colon = l.find(": ");
        // A failed check's line "<check>: <result>: <detail>".
        if (l.size() > 2 && l[0] == 'I' && std::isdigit(static_cast<unsigned char>(l[1])) && colon != std::string::npos
            && l.find(": ", colon + 2) != std::string::npos)
            row("why", plain_check_detail(l.substr(l.find(": ", colon + 2) + 2)) + " (check " + l.substr(0, colon) + ")");
        else
            row("why", l);
    }
    if (run.otherLines.size() > shown)
        row("why", "(" + std::to_string(run.otherLines.size() - shown) + " more lines: flirt-convert --json-events"
                   " shows them)");
    row("to do", f.todo);
    b.attention = f.headline;
    return b;
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

    // From here on the human text goes to stderr through the display
    // (batch_text.h: a block per run, the end), and with --json-events the
    // message events carry the lines flirt-convert once printed on stderr,
    // unchanged: a line about one run ("<runId>: ...") carries its run ID and
    // the text after that prefix.
    ProgressDisplay display;
    const Painter paint(display.colour());
    // With --json-events the blocks are not wrapped, each row one line:
    // flirt-tui wraps them at its own width. Else the terminal's width, 80 in
    // a log.
    auto text_width = [&] { return opt.jsonEvents ? std::numeric_limits<size_t>::max() / 2 : display.width(); };
    const Say say = [&](const std::string &text)
    {
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
    // A line said in both: the same words on stderr and in a message event.
    const Say tell = [&](const std::string &text)
    {
        display.line(text);
        say(text);
    };
    // A single run: notes stay in the blocks; otherwise the end says them,
    // once for the runs that share one.
    const bool notesInBlocks = runs.size() == 1;
    // A run's block, printed when it ends (never mixed with another's).
    auto print_block = [&](Run &run)
    {
        run.block = block_of(run, opt);
        run.said = true;
        display.lines(render_block(run.block, notesInBlocks, paint, text_width()));
    };

    // The folder said once: on a terminal $HOME as ~ (printable_text: the
    // folders above it may hold any bytes).
    auto shown_path = [&](const std::string &path)
    {
        const char *home = std::getenv("HOME");
        const std::string h = home ? home : "";
        if (display.terminal() && h.size() > 1 && (path == h || path.rfind(h + "/", 0) == 0))
            return printable_text("~" + path.substr(h.size()));
        return printable_text(path);
    };
    display.lines({ "Converting " + std::to_string(runs.size()) + (runs.size() == 1 ? " run" : " runs") + " into "
                    + shown_path(opt.outDir), "" });

    // Skip what is up to date, refuse other existing outputs; true if the run
    // is to be converted.
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
            print_block(run);
            return false;
        }

        const std::string outputs[] = { raw_path(opt, run), histos_path(opt, run), report_path(opt, run) };
        struct stat st;
        run.bytes = stat(run.input.c_str(), &st) == 0 ? static_cast<uint64_t>(st.st_size) : 0;
        if (std::none_of(std::begin(outputs), std::end(outputs), exists))
        {
            run.convert = true;
            return true;
        }

        const auto why = mismatch(opt, run);
        if (why.empty())
        {
            run.result = "skipped";
            run.note = "already converted";
            say(run.runId + ": skipped: already converted with the same input, options and FLIRT build");
        }
        else
        {
            run.result = "outputs-exist";
            run.note = why.note;
            run.why = why;
            // Only the run's files that are there.
            std::vector<std::string> present;
            for (const auto &path: outputs)
                if (exists(path))
                    present.push_back(path.substr(path.rfind('/') + 1));
            std::string names;
            for (size_t i = 0; i < present.size(); ++i)
                names += (i == 0 ? "" : i + 1 == present.size() ? " and " : ", ") + present[i];
            say(run.runId + ": outputs-exist: " + why.note + "; remove " + names + " in " + opt.outDir
                + " to reconvert");
        }
        print_block(run);
        return false;
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
            plan.push_back({ {"runId", run.runId}, {"input", run.input}, {"bytes", run.bytes},
                             {"action", run.convert ? "convert" : run.result} });
        events.send("plan", { {"runs", plan} });
        for (const auto &run: runs)
            if (!run.convert)
                events.send("result", result_json(opt, run));
    }

    // A progress event when the phase changes, and when the whole percent
    // changes but at most once per second.
    auto progress_event = [&](Run &run, bool launched)
    {
        if (!events.on())
            return;
        const auto &state = run.progress.state();
        const char *phase = launched || state.phase == ProgressState::Phase::Starting ? "starting"
            : state.phase == ProgressState::Phase::Progress ? "converting" : "finishing";
        const int percent = launched ? -1 : state.percent();
        const auto now = Clock::now();
        if (run.eventPhase == phase
            && (percent == run.eventPercent || now - run.eventTime < std::chrono::seconds(1)))
            return;
        run.eventPhase = phase;
        run.eventPercent = percent;
        run.eventTime = now;
        events.send("progress", { {"runId", run.runId}, {"phase", phase},
                                  {"percent", percent < 0 ? json(nullptr) : json(percent)} });
    };

    // At most opt.jobs children at a time.
    bool stopping = false;
    int stoppedBy = 0;
    std::vector<Run *> running;
    bool started = false;
    Clock::time_point firstStart;
    uint64_t launches = 0;

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

    // The runs this batch converts, running ones with their progress.
    auto show_progress = [&]()
    {
        using Phase = ProgressDisplay::Run::Phase;
        std::vector<ProgressDisplay::Run> list;
        for (auto &run: runs)
        {
            if (!run.convert)
                continue;
            ProgressDisplay::Run r;
            r.runId = run.runId;
            r.key = run.key;
            r.bytes = run.bytes;
            if (run.pid)
            {
                run.progress.poll();
                progress_event(run, false);
                r.phase = Phase::Running;
                r.state = run.progress.state();
            }
            else if (run.result.empty())
                r.phase = Phase::Pending;
            else
            {
                r.phase = run.result == "converted" ? Phase::Converted : Phase::Ended;
            }
            list.push_back(r);
        }

        const double elapsed = started ? seconds_since(firstStart) : 0;
        display.update(list, elapsed);
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

        // Before reaping, so a run's last progress lines are shown.
        show_progress();

        for (auto it = running.begin(); it != running.end();)
        {
            int status = 0;
            if (waitpid((*it)->pid, &status, WNOHANG) == (*it)->pid)
            {
                auto &run = **it;
                finish(run, status, stopping, opt, say);
                say_result(run, say);
                print_block(run);
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
            // A log: a line when each run starts (a terminal has the live line).
            if (!display.terminal())
                display.line("● " + run.runId + "   converting");
            if (!started)
            {
                started = true;
                firstStart = Clock::now();
            }
            run.key = ++launches;
            if (launch(run, opt, argv[0]))
            {
                running.push_back(&run);
                progress_event(run, true);
            }
            else
            {
                run.result = "failed";
                say(run.runId + ": failed: " + reason_text(failure_of(run)));
                say(run.runId + ": to do: " + failure_of(run).todo);
                print_block(run);
                events.send("result", result_json(opt, run));
            }
        }

        if (running.empty() && (stopping || queue.empty()))
            break;

        show_progress();
        // A quarter second while conversions run, so that a log shows each
        // tenth of a short run (a child writes its progress at most every
        // 0.5 s).
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
    else if (totals["failed"] || totals["outputs-exist"])
        exitCode = 1;
    else if (totals["unsupported"])
        exitCode = 3;

    const double wall = seconds_since(t0);
    display.finish();

    // The end (README "What flirt-convert prints"): the counts in words, the
    // runs that need attention, the notes said once.
    std::vector<const RunBlock *> blocks;
    for (auto &run: runs)
    {
        if (!run.said)
            run.block = block_of(run, opt);
        blocks.push_back(&run.block);
    }
    const std::pair<size_t, const char *> counted[] = {
        { totals["converted"], "converted" }, { totals["skipped"], "skipped" },
        { totals["outputs-exist"] + totals["interrupted"], "not converted" }, { totals["unsupported"], "unsupported" },
        { totals["failed"], "failed" }, { totals["not-started"], "not started" } };
    std::string counts;
    for (const auto &[n, word]: counted)
        if (n)
            counts += (counts.empty() ? "" : ", ") + std::to_string(n) + " " + word;
    counts = (counts.empty() ? std::string("no runs") : counts) + " in " + elapsed_text(wall)
        + (stoppedBy ? "; stopped by " + signal_name(stoppedBy) : "") + ".";
    display.lines(render_end(blocks, counts, runs.size(), notesInBlocks, paint, text_width()));

    auto summary = make_summary(opt, runs, totals, wall, exitCode, stoppedBy);
    if (!opt.summaryPath.empty() && !write_summary(opt, summary, tell) && exitCode == 0)
        exitCode = 1;

    // The exit code differs from the summary's only if it could not be written.
    summary["exitCode"] = exitCode;
    events.send("end", { {"summary", summary} });
    return exitCode;
}

}
