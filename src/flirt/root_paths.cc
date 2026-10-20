#include "flirt/root_paths.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <unistd.h>

namespace flirt
{

// new TFile(name) reads `name` as a URL (TUrl::SetUrl): leading and trailing
// spaces are stripped, it is cut at '?' (options) and '#' (anchor), and a
// special protocol prefix is stripped ("file:" etc.; ':' in general, as gEnv's
// Url.Special is configurable). gSystem->ExpandPathName then expands '$' and a
// leading '~' when the name holds any of "~*[]{}?$". Other names are passed
// unchanged, so the name recorded inside the file stays the client's.
std::string root_safe_name(const std::string &name)
{
    static const char special[] = "#?:~*[]{}$";
    std::string result = name;

    for (auto &c: result)
        if (c != '\0' && std::strchr(special, c))
            c = '_';
    for (size_t i = 0; i < result.size() && result[i] == ' '; ++i)
        result[i] = '_';
    for (size_t i = result.size(); i > 0 && result[i - 1] == ' '; --i)
        result[i - 1] = '_';
    return result;
}

ScopedChdir::ScopedChdir(const std::string &dir)
{
    // For search only, as chdir needs: a folder one may enter but not list works too.
#if defined(O_PATH)
    previous = open(".", O_PATH | O_DIRECTORY | O_CLOEXEC);     // Linux
#elif defined(O_SEARCH)
    previous = open(".", O_SEARCH | O_CLOEXEC);                 // macOS
#else
    previous = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
#endif
    if (previous < 0)
        throw std::runtime_error(std::string("getcwd: ") + std::strerror(errno));
    if (chdir(dir.c_str()) != 0)
    {
        const int err = errno;
        close(previous);
        throw std::runtime_error("chdir " + dir + ": " + std::strerror(err));
    }
}

ScopedChdir::~ScopedChdir()
{
    (void) !fchdir(previous);
    close(previous);
}

}
