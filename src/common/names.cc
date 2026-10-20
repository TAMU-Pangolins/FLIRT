#include "names.h"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <unistd.h>
#include <vector>

namespace flirt
{

std::string run_id_from_path(const std::string &archivePath)
{
    // QFileInfo::completeBaseName(): file name without the last suffix.
    auto slash = archivePath.find_last_of('/');
    auto name = slash == std::string::npos ? archivePath : archivePath.substr(slash + 1);
    auto dot = name.find_last_of('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

std::string absolute_path(const std::string &path)
{
    if (path.empty() || path[0] == '/')
        return path;

    std::vector<char> buf(4096);
    while (!getcwd(buf.data(), buf.size()))
    {
        if (errno != ERANGE)
            throw std::runtime_error(std::string("getcwd: ") + std::strerror(errno));
        buf.resize(buf.size() * 2);
    }
    return std::string(buf.data()) + "/" + path;
}

}
