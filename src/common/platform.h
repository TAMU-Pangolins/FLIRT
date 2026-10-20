#ifndef FLIRT_COMMON_PLATFORM_H
#define FLIRT_COMMON_PLATFORM_H

// The few system calls that differ between Linux and macOS, one helper each.
// On Linux each helper is exactly the call it replaces.

#include <csignal>
#include <ctime>
#include <string>
#include <sys/stat.h>

namespace flirt
{

// A file's modification time: st_mtim on Linux, st_mtimespec on macOS.
// Header only: the flirt library uses it without linking flirt_common.
inline timespec stat_mtime(const struct stat &st)
{
#ifdef __APPLE__
    return st.st_mtimespec;
#else
    return st.st_mtim;
#endif
}

// A path naming this program's executable. Linux: /proc/self/exe, the
// kernel's link to it. macOS: _NSGetExecutablePath, resolved with realpath().
// Empty if it cannot be found.
std::string self_exe();

// pipe2(fds, O_CLOEXEC). macOS has no pipe2: pipe(), then FD_CLOEXEC on both,
// which is not atomic: a posix_spawn in another thread in between passes the
// pipe on. Programs that spawn from several threads (flirt-tui:
// src/ui/job.cc, spawn.cc) therefore add POSIX_SPAWN_CLOEXEC_DEFAULT on macOS.
int pipe_cloexec(int fds[2]);

// sigtimedwait(set, nullptr, timeout) for signals the caller has blocked in
// every thread: the signal number, or -1 on timeout or interruption.
// macOS has no sigtimedwait: a kqueue EVFILT_SIGNAL filter reports each
// signal of the set, even one discarded as ignored (SIGCHLD at its default
// action is never left pending there), and a pending instance is then taken
// with sigwait() so it cannot fire later.
class SignalWaiter
{
    public:
        explicit SignalWaiter(const sigset_t &set);
        ~SignalWaiter();
        SignalWaiter(const SignalWaiter &) = delete;
        SignalWaiter &operator=(const SignalWaiter &) = delete;

        int wait(const timespec &timeout);

    private:
        sigset_t set_;
        int kq_ = -1;
};

}

#endif
