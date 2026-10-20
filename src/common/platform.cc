#include "platform.h"

#include <climits>
#include <cstdint>
#include <cstdlib>
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <sys/event.h>
#endif

namespace flirt
{

std::string self_exe()
{
#ifdef __APPLE__
    char path[PATH_MAX];
    uint32_t size = sizeof(path);
    char resolved[PATH_MAX];
    if (_NSGetExecutablePath(path, &size) != 0 || !realpath(path, resolved))
        return {};
    return resolved;
#else
    return "/proc/self/exe";
#endif
}

SignalWaiter::SignalWaiter(const sigset_t &set)
    : set_(set)
{
#ifdef __APPLE__
    kq_ = kqueue();
    for (int sig = 1; sig < NSIG; ++sig)
    {
        if (sigismember(&set_, sig) != 1)
            continue;
        struct kevent change;
        EV_SET(&change, sig, EVFILT_SIGNAL, EV_ADD, 0, 0, nullptr);
        kevent(kq_, &change, 1, nullptr, 0, nullptr);
    }
#endif
}

SignalWaiter::~SignalWaiter()
{
    if (kq_ >= 0)
        close(kq_);
}

int SignalWaiter::wait(const timespec &timeout)
{
#ifdef __APPLE__
    struct kevent event;
    if (kevent(kq_, nullptr, 0, &event, 1, &timeout) != 1)
        return -1;
    const int sig = static_cast<int>(event.ident);
    sigset_t pending, one;
    sigemptyset(&one);
    sigaddset(&one, sig);
    int taken = 0;
    if (sigpending(&pending) == 0 && sigismember(&pending, sig) == 1)
        sigwait(&one, &taken);
    return sig;
#else
    return sigtimedwait(&set_, nullptr, &timeout);
#endif
}

}
