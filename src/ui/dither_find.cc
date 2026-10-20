#include "dither_find.h"

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

DitherFinder::~DitherFinder()
{
    stop();
}

void DitherFinder::run(const std::string &file, const std::function<void(const json &)> &event)
{
    std::lock_guard<std::mutex> lock(runMutex_);
    {
        std::lock_guard<std::mutex> pidLock(pidMutex_);
        if (stopped_)
            return;
    }
    int out[2] = { -1, -1 };
    if (pipe_cloexec(out) != 0)
    {
        event({ {"event", "exit"}, {"error", std::string("cannot create a pipe: ") + std::strerror(errno)} });
        return;
    }
    // As InfoReader starts flirt-info (spawn_helper: own process group,
    // default signal actions, empty mask; stdin and stderr /dev/null).
    pid_t pid = 0;
    std::string why;
    const bool started = spawn_helper(convertExe_, { "--find-dither-offset", "--json-events", file }, -1, out[1], pid, why);
    close(out[1]);
    if (!started)
    {
        close(out[0]);
        event({ {"event", "exit"}, {"error", why} });
        return;
    }
    {
        std::lock_guard<std::mutex> pidLock(pidMutex_);
        pid_ = pid;
        if (stopped_)
            kill(pid_, SIGKILL);
    }

    // Each line as it comes.
    std::string buffer, problem;
    bool ended = false;
    for (;;)
    {
        char chunk[4096];
        const ssize_t n = ::read(out[0], chunk, sizeof(chunk));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        buffer.append(chunk, static_cast<size_t>(n));
        for (size_t newline; (newline = buffer.find('\n')) != std::string::npos;)
        {
            const auto line = buffer.substr(0, newline);
            buffer.erase(0, newline + 1);
            try
            {
                const auto e = json::parse(line);
                ended = ended || (e.is_object() && e.value("event", "") == "end");
                event(e);
            }
            catch (const std::exception &)
            {
                problem = "flirt-convert answered with something that is not JSON";
            }
        }
    }
    close(out[0]);

    // Reaped under pidMutex_, so that stop() never signals a reaped pid (as InfoReader::read).
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
    json last = { {"event", "exit"} };
    if (stopped)
        last["error"] = "the search was stopped";
    else if (!problem.empty())
        last["error"] = problem;
    else if (!ended && status >= 0 && WIFSIGNALED(status))
        last["error"] = "flirt-convert stopped unexpectedly (signal " + std::to_string(WTERMSIG(status)) + ")";
    else if (!ended)
        last["error"] = "flirt-convert stopped unexpectedly"
            + (status >= 0 && WIFEXITED(status) ? " (exit code " + std::to_string(WEXITSTATUS(status)) + ")" : std::string());
    event(last);
}

void DitherFinder::stop()
{
    {
        std::lock_guard<std::mutex> lock(pidMutex_);
        stopped_ = true;
        if (pid_ > 0)
            kill(pid_, SIGKILL);
    }
    std::lock_guard<std::mutex> lock(runMutex_);
}

}
