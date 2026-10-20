#include "job.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <csignal>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <poll.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "common/estimate.h"
#include "common/failures.h"
#include "common/names.h"
#include "common/platform.h"
#include "common/utf8.h"

extern char **environ;

namespace flirt
{

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace
{

constexpr size_t LogLines = 200;
constexpr size_t RunMessages = 2000;    // per run and for the lines without a run

double seconds_between(Clock::time_point a, Clock::time_point b)
{
    return std::chrono::duration<double>(b - a).count();
}

void add_capped(std::vector<std::string> &lines, const std::string &line)
{
    if (lines.size() < RunMessages)
        lines.push_back(line);
}

// "8 s", "2 min 13 s", "1 h 05 min": an elapsed time, as the TUI shows it.
std::string format_elapsed(double seconds)
{
    const long s = seconds < 0 ? 0 : static_cast<long>(seconds + 0.5);
    char text[32];
    if (s < 60)
        std::snprintf(text, sizeof(text), "%ld s", s);
    else if (s < 3600)
        std::snprintf(text, sizeof(text), "%ld min %ld s", s / 60, s % 60);
    else
        std::snprintf(text, sizeof(text), "%ld h %02ld min", s / 3600, s % 3600 / 60);
    return text;
}

std::string plural(size_t n, const std::string &word)
{
    return std::to_string(n) + " " + word + (n == 1 ? "" : "s");
}

bool starts_with(const std::string &s, const char *prefix)
{
    return s.rfind(prefix, 0) == 0;
}

// What a run's row says, and its level: neutral, busy, ok, warning, error.
// Decided here so that the screen decides nothing (docs/design.md, "TUI").
json run_status(const std::string &phase, int percent, const json &result, const json &note, unsigned warnings,
                const std::vector<std::string> &messages, FailureFor wordsFor)
{
    auto status = [](const std::string &text, const char *level, const std::string &detail = {},
                     const std::string &todo = {}, const std::string &check = {}) {
        auto maybe = [](const std::string &s) { return s.empty() ? json(nullptr) : json(s); };
        return json({ {"text", text}, {"level", level}, {"detail", maybe(detail)}, {"todo", maybe(todo)},
                      {"check", maybe(check)} });
    };
    const std::string why = note.is_string() ? note.get<std::string>() : "";
    if (!result.is_string())
    {
        if (phase == "starting") return status("Starting", "busy");
        if (phase == "converting") return status(percent >= 0 ? std::to_string(percent) + "%" : "Converting", "busy");
        if (phase == "finishing") return status("Finishing: writing the files", "busy");
        return status("Waiting", "neutral");
    }
    const auto r = result.get<std::string>();
    if (r == "converted")
    {
        if (why != "status ok")
            return status("Converted · damaged input accepted", "warning", why);
        return warnings ? status("Converted · " + plural(warnings, "warning"), "warning") : status("Converted", "ok");
    }
    if (r == "skipped") return status("Already converted · skipped", "neutral");
    // A bad end in plain words (common/failures.h): the headline, why, what to do.
    const bool diskFull = std::any_of(messages.begin(), messages.end(), [](const std::string &m) {
        return m.find("No space left on device") != std::string::npos;
    });
    const auto f = explain_failure(r, why, diskFull, wordsFor);
    if (r == "outputs-exist") return status("Output files exist · not converted", "warning", f.why, f.todo);
    if (r == "failed") return status("Failed: " + f.headline, "error", f.why, f.todo, f.check);
    if (r == "unsupported") return status("Not supported: " + f.headline, "error", f.why, f.todo, f.check);
    if (r == "interrupted") return status("Stopped", "warning", f.why, f.todo);
    if (r == "not-started") return status("Not started", "neutral");
    if (r == "unfinished") return status("Not finished by mvme", "warning", why);
    return status(r, "neutral", why);
}

}

std::string shell_quote(const std::string &word)
{
    const bool plain = !word.empty() && word.find_first_not_of(
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-./=:,+@%") == std::string::npos;
    if (plain)
        return word;

    // A C0 or C1 control, DEL or invalid UTF-8: $'...' with \xHH escapes,
    // safe for the terminal and pasted back into bash or zsh with the real
    // bytes (docs/design.md, "Text that is not valid UTF-8").
    const std::string valid = valid_utf8(word);
    bool special = valid != word;
    for (size_t i = 0; !special && i < word.size(); ++i)
    {
        const auto c = static_cast<unsigned char>(word[i]);
        // C1 controls are U+0080..U+009F: 0xC2 0x80..0x9F in UTF-8.
        special = c < 0x20 || c == 0x7F
            || (c == 0xC2 && i + 1 < word.size() && static_cast<unsigned char>(word[i + 1]) < 0xA0);
    }
    if (special)
    {
        std::string out = "$'";
        for (size_t i = 0; i < word.size(); ++i)
        {
            const auto c = static_cast<unsigned char>(word[i]);
            // Bytes of valid UTF-8 above ASCII stay as they are, but for a C1 control.
            const bool validByte = valid[i] == word[i];
            const bool c1 = c == 0xC2 && i + 1 < word.size() && static_cast<unsigned char>(word[i + 1]) < 0xA0
                && validByte;
            if (c == '\\' || c == '\'')
                out += std::string("\\") + static_cast<char>(c);
            else if (c < 0x20 || c == 0x7F || (c >= 0x80 && !validByte) || c1)
            {
                const auto hex = [](unsigned v) { return "0123456789abcdef"[v & 0xF]; };
                out += std::string("\\x") + hex(c >> 4) + hex(c);
                if (c1)
                {
                    const auto n = static_cast<unsigned char>(word[++i]);
                    out += std::string("\\x") + hex(n >> 4) + hex(n);
                }
            }
            else
                out += static_cast<char>(c);
        }
        return out + "'";
    }

    std::string out = "'";
    for (char c: word)
        out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return out + "'";
}

JobManager::~JobManager()
{
    stop_and_wait();
}

bool JobManager::start(const std::vector<std::string> &args, std::string &error, bool &busy)
{
    busy = false;
    std::unique_lock<std::mutex> lock(mutex_);
    if (job_ && job_->state != "finished")
    {
        busy = true;
        error = "a conversion is running; wait for it to end or cancel it";
        return false;
    }
    // The previous job's reader has set "finished" and is about to return.
    if (reader_.joinable())
        reader_.join();

    auto job = std::make_unique<Job>();
    job->command = { convertExe_, "--json-events" };
    job->command.insert(job->command.end(), args.begin(), args.end());

    int out[2] = { -1, -1 }, err[2] = { -1, -1 };
    if (pipe_cloexec(out) != 0 || pipe_cloexec(err) != 0)
    {
        error = std::string("cannot create a pipe: ") + std::strerror(errno);
        for (int fd: { out[0], out[1], err[0], err[1] })
            if (fd >= 0)
                close(fd);
        return false;
    }

    std::vector<char *> argv;
    for (auto &a: job->command)
        argv.push_back(a.data());
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, out[1], 1);
    posix_spawn_file_actions_adddup2(&actions, err[1], 2);

    // Its own process group, so a Ctrl-C in flirt-tui's terminal reaches it
    // only through flirt-tui. No signal blocked, default actions: flirt-tui
    // blocks its stop signals and ignores SIGPIPE, and flirt-convert stops on
    // SIGHUP and SIGPIPE only if they are not ignored at its start.
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t none, defaults;
    sigemptyset(&none);
    sigemptyset(&defaults);
    for (int sig: { SIGINT, SIGTERM, SIGHUP, SIGPIPE, SIGCHLD })
        sigaddset(&defaults, sig);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &defaults);
    posix_spawnattr_setpgroup(&attr, 0);
    // macOS: every fd not set up by the file actions is closed in the child
    // (POSIX_SPAWN_CLOEXEC_DEFAULT). pipe_cloexec() there is pipe() then
    // fcntl(FD_CLOEXEC), not atomic: a spawn between another
    // thread's pipe() and fcntl() would pass that pipe to the child
    // (docs/design.md, "macOS (Apple silicon)").
    // Linux: pipe2(O_CLOEXEC) is atomic.
    short flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETPGROUP;
#ifdef __APPLE__
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
    posix_spawnattr_setflags(&attr, flags);

    pid_t pid = 0;
    const int rc = posix_spawn(&pid, convertExe_.c_str(), &actions, &attr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    close(out[1]);
    close(err[1]);
    if (rc != 0)
    {
        close(out[0]);
        close(err[0]);
        error = "cannot start " + convertExe_ + ": " + std::strerror(rc);
        return false;
    }

    job->pid = pid;
    job->started = static_cast<int64_t>(std::time(nullptr));
    job->startClock = Clock::now();
    job_ = std::move(job);
    changed();
    reader_ = std::thread(&JobManager::read_output, this, out[0], err[0]);
    return true;
}

bool JobManager::cancel()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!job_ || job_->state == "finished")
        return false;
    if (job_->state == "running")
    {
        // The reader reaps the pid under this lock, and sets "finished" with it.
        kill(job_->pid, SIGINT);
        job_->state = "stopping";
        changed();
    }
    return true;
}

bool JobManager::stop_and_wait()
{
    bool running = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (job_ && job_->state != "finished")
        {
            running = true;
            if (job_->state == "running")
            {
                kill(job_->pid, SIGINT);
                job_->state = "stopping";
                changed();
            }
        }
    }
    if (reader_.joinable())
        reader_.join();
    return running;
}

void JobManager::shutdown()
{
    std::lock_guard<std::mutex> lock(mutex_);
    shutdown_ = true;
    cv_.notify_all();
}

void JobManager::changed()
{
    ++version_;
    cv_.notify_all();
}

json JobManager::state(uint64_t after, std::chrono::milliseconds wait)
{
    std::unique_lock<std::mutex> lock(mutex_);
    if (version_ <= after && !shutdown_)
        cv_.wait_for(lock, wait, [&] { return version_ > after || shutdown_; });
    return { {"version", version_}, {"job", job_json()} };
}

// Reads the batch's stdout (events) and stderr (lines) until both close,
// then reaps it.
void JobManager::read_output(int outFd, int errFd)
{
    struct Stream
    {
        int fd;
        std::string partial;
        bool events;
    };
    Stream streams[] = { { outFd, {}, true }, { errFd, {}, false } };

    while (streams[0].fd >= 0 || streams[1].fd >= 0)
    {
        pollfd fds[2];
        for (int i = 0; i < 2; ++i)
            fds[i] = { streams[i].fd, POLLIN, 0 };
        if (poll(fds, 2, -1) < 0)
        {
            if (errno == EINTR)
                continue;
            break;
        }
        for (int i = 0; i < 2; ++i)
        {
            auto &s = streams[i];
            if (s.fd < 0 || !(fds[i].revents & (POLLIN | POLLHUP | POLLERR)))
                continue;
            char buf[65536];
            const ssize_t n = read(s.fd, buf, sizeof(buf));
            if (n < 0 && errno == EINTR)
                continue;
            std::lock_guard<std::mutex> lock(mutex_);
            if (n <= 0)
            {
                close(s.fd);
                s.fd = -1;
                if (!s.partial.empty())
                    s.events ? on_stdout_line(s.partial) : on_stderr_line(s.partial);
                s.partial.clear();
            }
            else
            {
                s.partial.append(buf, static_cast<size_t>(n));
                size_t begin = 0, end;
                while ((end = s.partial.find('\n', begin)) != std::string::npos)
                {
                    const auto line = s.partial.substr(begin, end - begin);
                    s.events ? on_stdout_line(line) : on_stderr_line(line);
                    begin = end + 1;
                }
                s.partial.erase(0, begin);
            }
            changed();
        }
    }
    for (auto &s: streams)
        if (s.fd >= 0)
            close(s.fd);

    pid_t pid;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pid = job_->pid;
    }
    // Waits for the exit without reaping, then reaps under the lock with
    // "finished": cancel() never signals a pid another process may have got.
    siginfo_t info;
    while (waitid(P_PID, pid, &info, WEXITED | WNOWAIT) < 0 && errno == EINTR) { }

    std::lock_guard<std::mutex> lock(mutex_);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) { }
    for (const auto &m: job_->early)
        add_capped(job_->messages, m.first + ": " + m.second);
    job_->early.clear();
    // Without an end event (a crash), the exit code as a shell shows it.
    if (job_->exitCode.is_null())
        job_->exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    job_->state = "finished";
    job_->endClock = Clock::now();
    changed();
}

JobManager::RunState *JobManager::find_run(const json &runId)
{
    if (!runId.is_string())
        return nullptr;
    for (auto &run: job_->runs)
        if (run.runId == runId.get<std::string>())
            return &run;
    return nullptr;
}

void JobManager::add_run_message(RunState &run, const std::string &text)
{
    add_capped(run.messages, text);
    if (starts_with(text, "warning: "))
        ++run.warnings;
    else if (starts_with(text, "note: "))
        ++run.notes;
}

void JobManager::add_log(const std::string &line)
{
    job_->log.push_back(valid_utf8(line));
    while (job_->log.size() > LogLines)
        job_->log.pop_front();
}

void JobManager::on_stderr_line(const std::string &line)
{
    add_log(line);
}

// flirt-convert's events carry text cleaned of invalid UTF-8 (common/utf8.h):
// the job's own argument that cleans to `sent`, else `sent` (e.g. a file
// found in a folder given as input). For a valid name the same string.
std::string JobManager::real_argument(const std::string &sent) const
{
    for (size_t i = 2; i < job_->command.size(); ++i)   // after the program and --json-events
        if (valid_utf8(job_->command[i]) == sent)
            return job_->command[i];
    return sent;
}

// One --json-events line (README "Events for programs").
void JobManager::on_stdout_line(const std::string &line)
{
    json e;
    try
    {
        e = json::parse(line);
    }
    catch (const std::exception &)
    {
        add_log(line);  // not an event
        return;
    }
    try
    {
        const auto event = e.value("event", std::string());
        auto &job = *job_;
        if (event == "start")
            job.outputDir = real_argument(e.at("options").value("outputDir", std::string()));
        else if (event == "plan")
        {
            job.runs.clear();
            for (const auto &r: e.at("runs"))
            {
                RunState run;
                // runId as sent: later events are keyed by it.
                run.runId = r.at("runId").get<std::string>();
                run.input = real_argument(r.at("input").get<std::string>());
                run.bytes = r.at("bytes").get<uint64_t>();
                run.action = r.at("action").get<std::string>();
                run.phase = run.action == "convert" ? "pending" : "";
                job.runs.push_back(run);
            }
            job.planned = true;
            for (const auto &m: job.early)
                if (auto run = find_run(m.first))
                    add_run_message(*run, m.second);
                else
                    add_capped(job.messages, m.first + ": " + m.second);
            job.early.clear();
        }
        else if (event == "progress")
        {
            if (auto run = find_run(e.at("runId")))
            {
                run->phase = e.at("phase").get<std::string>();
                run->percent = e.at("percent").is_number() ? e.at("percent").get<int>() : -1;
                if (!job.anyStarted)
                {
                    job.anyStarted = true;
                    job.firstStart = Clock::now();
                }
            }
        }
        else if (event == "message")
        {
            const auto text = e.at("text").get<std::string>();
            const auto &runId = e.at("runId");
            if (auto run = find_run(runId))
                add_run_message(*run, text);
            else if (runId.is_string() && !job.planned)
                job.early.emplace_back(runId.get<std::string>(), text);
            else
                add_capped(job.messages, runId.is_string() ? runId.get<std::string>() + ": " + text : text);
        }
        else if (event == "result")
        {
            if (auto run = find_run(e.at("runId")))
            {
                run->result = e.at("result");
                run->note = e.value("note", json(nullptr));
                run->seconds = e.value("seconds", json(nullptr));
                run->phase = "ended";
                // The report and the files it has in the output folder
                // (skipped runs too), named after the real input's run ID.
                const auto base = job.outputDir + "/" + run_id_from_path(run->input);
                const auto report = e.value("report", json(nullptr));
                run->report = report.is_string() && valid_utf8(base + ".flirt.json") == report.get<std::string>()
                    ? json(base + ".flirt.json") : report;
                for (const char *suffix: { "_raw.root", "_histos.root", "_waveforms.root", "_hit_flags.analysis",
                                           ".flirt.json" })
                {
                    const auto path = base + suffix;
                    struct stat st;
                    if (!job.outputDir.empty() && run->result != "outputs-exist" && run->result != "interrupted"
                        && stat(path.c_str(), &st) == 0)
                        run->outputs.push_back(path);
                }
                // --extract-hit-flags: the report's counts, for the details.
                if (run->report.is_string() && (run->result == "converted" || run->result == "skipped"))
                    try
                    {
                        std::ifstream in(run->report.get<std::string>());
                        const auto flags = json::parse(in).at("hitFlags");
                        if (flags.value("enabled", false))
                            run->hitFlags = flags;
                    }
                    catch (const std::exception &)
                    {
                    }
            }
        }
        else if (event == "end")
        {
            job.summary = e.at("summary");
            job.exitCode = job.summary.value("exitCode", json(nullptr));
            job.stoppedBy = job.summary.value("stoppedBy", json(nullptr));
        }
        else if (event == "usage-error")
            job.usageError = e.at("message");
    }
    catch (const std::exception &)
    {
        add_log(line);  // an event this version does not understand
    }
}

json JobManager::job_json() const
{
    if (!job_)
        return nullptr;
    const auto &job = *job_;
    const auto now = job.state == "finished" ? job.endClock : Clock::now();

    // The command as a person would type it: without --json-events, so
    // stdout gets the human summary.
    std::string commandLine;
    for (size_t i = 0; i < job.command.size(); ++i)
        if (i != 1)     // command[1] is --json-events (start())
            commandLine += (commandLine.empty() ? "" : " ") + shell_quote(job.command[i]);

    // The estimate as flirt-convert computes it: from the first start on,
    // weighted by input size (common/estimate.h).
    json estimate = nullptr;
    json runs = json::array();
    std::vector<EstimateRun> estimated;
    for (const auto &run: job.runs)
    {
        if (run.action == "convert")
        {
            EstimateRun e;
            e.bytes = run.bytes;
            if (run.result.is_string())
                e.phase = run.result == "converted" ? EstimateRun::Phase::Converted : EstimateRun::Phase::Ended;
            else if (run.phase == "pending")
                e.phase = EstimateRun::Phase::Pending;
            else
            {
                e.phase = EstimateRun::Phase::Running;
                e.fraction = run.phase == "finishing" ? 1.0 : run.percent > 0 ? run.percent / 100.0 : 0.0;
            }
            estimated.push_back(e);
        }
        runs.push_back({ {"runId", run.runId}, {"input", run.input}, {"bytes", run.bytes}, {"action", run.action},
                         {"phase", run.phase.empty() ? json(nullptr) : json(run.phase)},
                         {"percent", run.percent < 0 ? json(nullptr) : json(run.percent)},
                         {"result", run.result}, {"note", run.note}, {"seconds", run.seconds},
                         {"report", run.report}, {"messages", run.messages},
                         {"warnings", run.warnings}, {"notes", run.notes}, {"outputs", run.outputs},
                         {"hitFlags", run.hitFlags},
                         {"status", run_status(run.phase, run.percent, run.result, run.note, run.warnings,
                                               run.messages, wordsFor_)} });
    }

    // The overall share done, weighted by size, and the headline.
    double doneBytes = 0, allBytes = 0;
    size_t convert = 0, started = 0;
    for (size_t i = 0; i < estimated.size(); ++i)
    {
        const auto &e = estimated[i];
        allBytes += static_cast<double>(e.bytes);
        ++convert;
        if (e.phase != EstimateRun::Phase::Pending)
            ++started;
        if (e.phase == EstimateRun::Phase::Converted || e.phase == EstimateRun::Phase::Ended)
            doneBytes += static_cast<double>(e.bytes);
        else if (e.phase == EstimateRun::Phase::Running)
            doneBytes += e.fraction * static_cast<double>(e.bytes);
    }
    const json progress = allBytes > 0 ? json(doneBytes / allBytes) : job.state == "finished" ? json(1.0) : json(0.0);
    if (job.state == "running" && job.anyStarted)
    {
        const double left = estimate_seconds(estimated, seconds_between(job.firstStart, now));
        if (left >= 0)
            estimate = "about " + format_duration(left) + " left";
    }

    std::string headline, level = "busy";
    const std::string elapsed = format_elapsed(seconds_between(job.startClock, now));
    if (job.state == "stopping")
        headline = "Stopping...";
    else if (job.state == "running")
    {
        if (!job.planned)
            headline = "Checking the listfiles...";
        else if (convert == 0)
            headline = "Finishing...";
        else
            headline = "Converting " + std::to_string(std::max<size_t>(started, 1)) + " of " + std::to_string(convert)
                + (estimate.is_string() ? " · " + estimate.get<std::string>() : "");
    }
    else if (job.usageError.is_string())
    {
        headline = "Not started: flirt-convert refused the settings";
        level = "error";
    }
    else
    {
        // Counts by result, in plain words.
        static const std::pair<const char *, const char *> words[] = {
            {"converted", "converted"}, {"skipped", "already converted"}, {"outputs-exist", "with existing outputs"},
            {"failed", "failed"}, {"unsupported", "not supported"}, {"interrupted", "stopped"},
            {"not-started", "not started"} };
        std::string counts;
        size_t warned = 0;
        for (const auto &run: job.runs)
            if (run.result == "converted" && (run.warnings || run.note != "status ok"))
                ++warned;
        for (const auto &[result, word]: words)
        {
            size_t n = 0;
            for (const auto &run: job.runs)
                n += run.result == result;
            if (n)
                counts += (counts.empty() ? "" : ", ") + std::to_string(n) + " " + word;
        }
        if (counts.empty())
            counts = "nothing to do";
        headline = (job.stoppedBy.is_string() ? "Stopped: " : "Finished: ") + counts + " · " + elapsed;
        auto any = [&](const char *result) {
            for (const auto &run: job.runs)
                if (run.result == result)
                    return true;
            return false;
        };
        // A crash before the plan has no runs: its exit code decides.
        const bool crashed = job.runs.empty() && job.exitCode != 0;
        if (any("failed") || any("unsupported") || crashed)
            level = "error";
        else if (any("outputs-exist") || any("interrupted") || any("not-started") || warned || job.exitCode != 0)
            level = "warning";
        else
            level = "ok";
    }

    return {
        {"state", job.state}, {"command", job.command}, {"commandLine", commandLine},
        {"started", job.started}, {"elapsed", seconds_between(job.startClock, now)},
        {"estimate", estimate}, {"runs", runs}, {"messages", job.messages},
        {"usageError", job.usageError}, {"exitCode", job.exitCode}, {"stoppedBy", job.stoppedBy},
        {"summary", job.summary}, {"outputDir", job.outputDir.empty() ? json(nullptr) : json(job.outputDir)},
        {"progress", progress}, {"headline", { {"text", headline}, {"level", level} }},
        {"elapsedText", elapsed}, {"log", json(std::vector<std::string>(job.log.begin(), job.log.end()))} };
}

}
