#include "names.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace flirt
{

bool natural_less(const std::string &a, const std::string &b)
{
    auto digit = [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; };
    size_t i = 0, j = 0;

    while (i < a.size() && j < b.size())
    {
        if (digit(a[i]) && digit(b[j]))
        {
            size_t ie = i, je = j;
            while (ie < a.size() && digit(a[ie])) ++ie;
            while (je < b.size() && digit(b[je])) ++je;
            while (i + 1 < ie && a[i] == '0') ++i;
            while (j + 1 < je && b[j] == '0') ++j;
            if (ie - i != je - j)
                return ie - i < je - j;
            if (int c = a.compare(i, ie - i, b, j, je - j))
                return c < 0;
            i = ie;
            j = je;
        }
        else
        {
            if (a[i] != b[j])
                return static_cast<unsigned char>(a[i]) < static_cast<unsigned char>(b[j]);
            ++i;
            ++j;
        }
    }
    if (a.size() - i != b.size() - j)
        return a.size() - i < b.size() - j;
    return a < b;
}

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

bool is_listfile_name(const std::string &name)
{
    return name.size() > 4 && name[0] != '.' && name.compare(name.size() - 4, 4, ".zip") == 0;
}

void add_listfile_input(const std::string &path, std::vector<std::string> &inputs)
{
    struct stat st;
    if (stat(path.c_str(), &st) != 0)
        throw std::runtime_error("no such file or directory: " + path);

    if (!S_ISDIR(st.st_mode))
    {
        inputs.push_back(path);
        return;
    }

    DIR *dir = opendir(path.c_str());
    if (!dir)
        throw std::runtime_error("cannot read directory " + path + ": " + std::strerror(errno));

    const std::string prefix = path.back() == '/' ? path : path + "/";
    std::vector<std::string> names;

    while (auto entry = readdir(dir))
    {
        std::string name = entry->d_name;
        struct stat est;
        if (is_listfile_name(name) && stat((prefix + name).c_str(), &est) == 0 && S_ISREG(est.st_mode))
            names.push_back(name);
    }
    closedir(dir);

    if (names.empty())
        throw std::runtime_error("no *.zip files in directory " + path);

    std::sort(names.begin(), names.end(), natural_less);
    for (const auto &name: names)
        inputs.push_back(prefix + name);
}

}
