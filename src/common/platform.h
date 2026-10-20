#ifndef FLIRT_COMMON_PLATFORM_H
#define FLIRT_COMMON_PLATFORM_H

// The few system calls that differ between Linux and macOS, one helper each.
// On Linux each helper is exactly the call it replaces.

#include <string>

namespace flirt
{

// A path naming this program's executable. Linux: /proc/self/exe, the
// kernel's link to it. macOS: _NSGetExecutablePath, resolved with realpath().
// Empty if it cannot be found.
std::string self_exe();

}

#endif
