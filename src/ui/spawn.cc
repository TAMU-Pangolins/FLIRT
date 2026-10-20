#include "spawn.h"

#include <climits>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <unistd.h>

#include "common/platform.h"

extern char **environ;

namespace flirt
{

std::string program_name()
{
    char resolved[PATH_MAX];
    const auto exe = self_exe();
    if (exe.empty() || !realpath(exe.c_str(), resolved))
        return "this program";
    const std::string path = resolved;
    return path.substr(path.rfind('/') + 1);
}

bool spawn_helper(const std::string &exe, const std::vector<std::string> &args, int stdinFd, int stdoutFd,
                  pid_t &pid, std::string &why)
{
    const auto slash = exe.rfind('/');
    const auto name = slash == std::string::npos ? exe : exe.substr(slash + 1);
    if (access(exe.c_str(), X_OK) != 0)
    {
        why = name + " is missing next to " + program_name();
        return false;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (stdinFd >= 0)
        posix_spawn_file_actions_adddup2(&actions, stdinFd, 0);
    else
        posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    if (stdoutFd >= 0)
        posix_spawn_file_actions_adddup2(&actions, stdoutFd, 1);
    else
        posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);

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
    // fcntl(FD_CLOEXEC), not atomic, and flirt-tui spawns from three threads
    // (flirt-convert, flirt-waveforms, flirt-info): a spawn between another
    // thread's pipe() and fcntl() passed that pipe to the child, e.g.
    // flirt-convert kept flirt-waveforms' stdout open, so the reader never saw
    // EOF and flirt-tui hung on quit (docs/design.md, "macOS (Apple silicon)").
    // Linux: pipe2(O_CLOEXEC) is atomic.
    short flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETPGROUP;
#ifdef __APPLE__
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
    posix_spawnattr_setflags(&attr, flags);

    std::vector<std::string> strings = { exe };
    strings.insert(strings.end(), args.begin(), args.end());
    std::vector<char *> argv;
    for (auto &s: strings)
        argv.push_back(s.data());
    argv.push_back(nullptr);
    const int rc = posix_spawn(&pid, exe.c_str(), &actions, &attr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    if (rc != 0)
    {
        why = "cannot start " + exe + ": " + std::strerror(rc);
        return false;
    }
    return true;
}

}
