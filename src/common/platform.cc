#include "platform.h"

#include <climits>
#include <cstdint>
#include <cstdlib>

#ifdef __APPLE__
#include <mach-o/dyld.h>
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

}
