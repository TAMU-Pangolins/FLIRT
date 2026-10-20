#include "info.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "common/platform.h"
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

InfoReader::~InfoReader()
{
    stop();
}

json InfoReader::read(const std::string &listfile)
{
    std::lock_guard<std::mutex> lock(readMutex_);
    {
        std::lock_guard<std::mutex> pidLock(pidMutex_);
        if (stopped_)
            return error("the run information reader was stopped");
    }

    int out[2] = { -1, -1 };
    if (pipe_cloexec(out) != 0)
        return error(std::string("cannot create a pipe: ") + std::strerror(errno));
    const std::vector<std::string> args = { "--json", listfile };

    // As WaveformReader::start (waveforms.cc) starts flirt-waveforms: own
    // process group, default signal actions, empty mask (spawn_helper); stdin
    // and stderr /dev/null.
    pid_t pid = 0;
    std::string why;
    const bool started = spawn_helper(helperExe_, args, -1, out[1], pid, why);
    close(out[1]);
    if (!started)
    {
        close(out[0]);
        return error(why);
    }
    {
        std::lock_guard<std::mutex> pidLock(pidMutex_);
        pid_ = pid;
        if (stopped_)
            kill(pid_, SIGKILL);    // stop() came while it started: the read ends at once
    }

    // Its stdout to the end: flirt-info writes one line and exits.
    std::string text;
    for (;;)
    {
        char chunk[65536];
        const ssize_t n = ::read(out[0], chunk, sizeof(chunk));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        text.append(chunk, static_cast<size_t>(n));
    }
    close(out[0]);

    // Reaped under pidMutex_, so that stop() never signals a reaped (maybe
    // reused) pid, as WaveformReader::reap; polled, so that stop() can still
    // kill a helper that closed its stdout but does not end.
    int status = -1;
    bool stopped = false;
    for (;;)
    {
        {
            std::lock_guard<std::mutex> pidLock(pidMutex_);
            int st = 0;
            const pid_t r = waitpid(pid_, &st, WNOHANG);
            if (r == pid_ || (r < 0 && errno != EINTR))
            {
                if (r == pid_)
                    status = st;
                pid_ = 0;
                stopped = stopped_;
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    if (stopped)
        return error("the run information reader was stopped");
    const auto newline = text.find('\n');
    const auto line = text.substr(0, newline);
    if (line.empty())
    {
        if (status >= 0 && WIFSIGNALED(status))
            return error("flirt-info stopped unexpectedly (signal " + std::to_string(WTERMSIG(status)) + ")");
        if (status >= 0 && WIFEXITED(status))
            return error("flirt-info stopped unexpectedly (exit code " + std::to_string(WEXITSTATUS(status)) + ")");
        return error("flirt-info stopped unexpectedly");
    }
    try
    {
        auto answer = json::parse(line);
        if (!answer.is_object())
            return error("flirt-info answered with something that is not JSON");
        return answer;
    }
    catch (const std::exception &)
    {
        return error("flirt-info answered with something that is not JSON");
    }
}

void InfoReader::stop()
{
    {
        std::lock_guard<std::mutex> lock(pidMutex_);
        stopped_ = true;
        if (pid_ > 0)
            kill(pid_, SIGKILL);    // a waiting read() reads the end of its output at once
    }
    // The running read() has reaped it when it returns.
    std::lock_guard<std::mutex> lock(readMutex_);
}

}
