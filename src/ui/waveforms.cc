#include "waveforms.h"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "common/platform.h"
#include "common/utf8.h"
#include "spawn.h"

namespace flirt
{

using json = nlohmann::json;

namespace
{

json error(const std::string &sentence)
{
    return { {"error", sentence} };
}

}

WaveformReader::~WaveformReader()
{
    stop();
}

// As JobManager starts flirt-convert (spawn_helper, spawn.h).
bool WaveformReader::start(std::string &why)
{
    int in[2] = { -1, -1 }, out[2] = { -1, -1 };
    if (pipe_cloexec(in) != 0 || pipe_cloexec(out) != 0)
    {
        why = std::string("cannot create a pipe: ") + std::strerror(errno);
        for (int fd: { in[0], in[1], out[0], out[1] })
            if (fd >= 0)
                close(fd);
        return false;
    }

    pid_t pid = 0;
    const bool started = spawn_helper(helperExe_, {}, in[0], out[1], pid, why);
    close(in[0]);
    close(out[1]);
    if (!started)
    {
        close(in[1]);
        close(out[0]);
        return false;
    }
    in_ = in[1];
    out_ = out[0];
    buffer_.clear();
    std::lock_guard<std::mutex> lock(pidMutex_);
    pid_ = pid;
    if (stopping_)
        kill(pid_, SIGKILL);    // stop() came while it started: the ask ends at once
    return true;
}

// Kills and reaps the helper under pidMutex_, so that stop() never signals a
// reaped (maybe reused) pid; the wait is short, the helper is killed. Its
// status, if `status`.
void WaveformReader::reap(int *status)
{
    std::lock_guard<std::mutex> lock(pidMutex_);
    if (pid_ <= 0)
        return;
    kill(pid_, SIGKILL);    // no effect if it has already ended: its status stays
    int st = 0;
    while (waitpid(pid_, &st, 0) < 0 && errno == EINTR) { }
    if (status)
        *status = st;
    pid_ = 0;
}

void WaveformReader::finish()
{
    if (in_ >= 0)
        close(in_);
    in_ = -1;
    reap(nullptr);
    if (out_ >= 0)
        close(out_);
    out_ = -1;
    buffer_.clear();
}

std::string WaveformReader::died()
{
    bool stopped;
    {
        std::lock_guard<std::mutex> lock(pidMutex_);
        stopped = stopping_;
    }
    int status = -1;
    reap(&status);
    finish();
    if (stopped)
        return "the waveform reader was stopped";
    if (status >= 0 && WIFSIGNALED(status))
        return "flirt-waveforms stopped unexpectedly (signal " + std::to_string(WTERMSIG(status)) + ")";
    if (status >= 0 && WIFEXITED(status))
        return "flirt-waveforms stopped unexpectedly (exit code " + std::to_string(WEXITSTATUS(status)) + ")";
    return "flirt-waveforms stopped unexpectedly";
}

json WaveformReader::ask(const json &request)
{
    std::lock_guard<std::mutex> lock(askMutex_);
    if (in_ < 0)
    {
        std::string why;
        if (!start(why))
            return error(why);
    }

    // A file name that is not valid UTF-8 cannot be a JSON string: its bytes
    // go as hex in "fileBytes" too (docs/usage.md "Events for programs").
    auto sent = request;
    const auto file = request.is_object() ? request.value("file", json()) : json();
    if (file.is_string() && valid_utf8(file.get<std::string>()) != file.get<std::string>())
    {
        static const char digits[] = "0123456789abcdef";
        std::string hex;
        for (const unsigned char c: file.get<std::string>())
        {
            hex += digits[c >> 4];
            hex += digits[c & 15];
        }
        sent["fileBytes"] = hex;
    }
    // SIGPIPE is ignored by the programs using this class: a dead helper is EPIPE.
    const auto line = dump_valid_utf8(sent) + "\n";
    for (size_t done = 0; done < line.size();)
    {
        const ssize_t n = write(in_, line.data() + done, line.size() - done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return error(died());
        done += static_cast<size_t>(n);
    }

    size_t newline;
    while ((newline = buffer_.find('\n')) == std::string::npos)
    {
        char chunk[65536];
        const ssize_t n = read(out_, chunk, sizeof(chunk));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return error(died());
        buffer_.append(chunk, static_cast<size_t>(n));
    }
    const auto text = buffer_.substr(0, newline);
    buffer_.erase(0, newline + 1);
    try
    {
        auto answer = json::parse(text);
        // The file as the caller named it, not as JSON could carry it.
        if (answer.is_object() && answer.find("file") != answer.end() && file.is_string())
            answer["file"] = file;
        return answer;
    }
    catch (const std::exception &)
    {
        finish();
        return error("flirt-waveforms answered with something that is not JSON");
    }
}

void WaveformReader::stop()
{
    {
        std::lock_guard<std::mutex> lock(pidMutex_);
        stopping_ = true;
        if (pid_ > 0)
            kill(pid_, SIGKILL);    // a waiting ask() reads the end of its output at once
    }
    {
        std::lock_guard<std::mutex> lock(askMutex_);
        finish();
    }
    std::lock_guard<std::mutex> lock(pidMutex_);
    stopping_ = false;
}

}
