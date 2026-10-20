#ifndef FLIRT_COMMON_PLATFORM_H
#define FLIRT_COMMON_PLATFORM_H

// The few system calls that differ between Linux and macOS, one helper each.
// On Linux each helper is exactly the call it replaces.

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

}

#endif
