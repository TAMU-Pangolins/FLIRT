#ifndef FLIRT_ROOT_PATHS_H
#define FLIRT_ROOT_PATHS_H

// ROOT and its interpreter never get a user's file or folder name: ROOT files
// are created from inside FLIRT's private directory under root_safe_name().

#include <string>

namespace flirt
{

// `name` with each byte ROOT would read in a file name (see root_paths.cc)
// replaced by '_'; unchanged when it holds none.
std::string root_safe_name(const std::string &name);

// Changes the working directory, and restores the previous one when leaving
// the scope, through a descriptor (fchdir): no length limit, and it works even
// if the previous directory was renamed meanwhile.
struct ScopedChdir
{
    int previous = -1;

    explicit ScopedChdir(const std::string &dir);
    ~ScopedChdir();

    ScopedChdir(const ScopedChdir &) = delete;
    ScopedChdir &operator=(const ScopedChdir &) = delete;
};

}

#endif
