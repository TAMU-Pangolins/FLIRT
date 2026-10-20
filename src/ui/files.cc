#include "files.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <iterator>
#include <sstream>
#include <sys/stat.h>
#include <vector>

#include "common/names.h"

namespace flirt
{

using json = nlohmann::json;

ApiResult api_error(int status, const std::string &message)
{
    return { status, json({ {"error", message} }) };
}

std::string normalize_path(const std::string &path)
{
    if (path.empty() || path[0] != '/')
        return {};
    std::vector<std::string> parts;
    std::istringstream in(path);
    std::string part;
    while (std::getline(in, part, '/'))
    {
        if (part.empty() || part == ".")
            continue;
        if (part == "..")
        {
            if (!parts.empty())
                parts.pop_back();
        }
        else
            parts.push_back(part);
    }
    std::string out;
    for (const auto &p: parts)
        out += "/" + p;
    return out.empty() ? "/" : out;
}

namespace
{

std::string parent_of(const std::string &normalized)
{
    const auto slash = normalized.rfind('/');
    return slash == 0 ? "/" : normalized.substr(0, slash);
}

std::string join(const std::string &dir, const std::string &name)
{
    return dir == "/" ? "/" + name : dir + "/" + name;
}

// A failed stat or opendir of `path`, as the error answer.
ApiResult path_error(const std::string &path, int err)
{
    return api_error(err == ENOENT ? 404 : 400, path + ": " + std::strerror(err));
}

}

ApiResult list_directory(const std::string &requested)
{
    const auto path = normalize_path(requested);
    if (path.empty())
        return api_error(400, "the path must be absolute: " + requested);

    struct stat st;
    if (stat(path.c_str(), &st) != 0)
        return path_error(path, errno);
    if (!S_ISDIR(st.st_mode))
        return api_error(400, "not a directory: " + path);

    DIR *dir = opendir(path.c_str());
    if (!dir)
        return path_error(path, errno);

    struct Entry
    {
        std::string name, type;
        json object;
    };
    std::vector<Entry> entries;
    while (auto e = readdir(dir))
    {
        const std::string name = e->d_name;
        if (name.empty() || name[0] == '.')
            continue;
        const auto full = join(path, name);
        // Symlinks followed; a broken one is "other".
        struct stat est;
        const bool followed = stat(full.c_str(), &est) == 0;
        if (!followed && lstat(full.c_str(), &est) != 0)
            continue;   // gone meanwhile
        const std::string type = !followed ? "other" : S_ISDIR(est.st_mode) ? "dir"
            : S_ISREG(est.st_mode) ? "file" : "other";
        entries.push_back({ name, type, {
            {"name", name}, {"type", type},
            {"size", static_cast<int64_t>(est.st_size)},
            {"mtime", static_cast<int64_t>(est.st_mtime)},
            {"id", std::to_string(est.st_dev) + ":" + std::to_string(est.st_ino)},
            {"listfile", type == "file" && is_listfile_name(name)} } });
    }
    closedir(dir);

    // Directories first, then natural order.
    std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) {
        if ((a.type == "dir") != (b.type == "dir"))
            return a.type == "dir";
        return natural_less(a.name, b.name);
    });

    json list = json::array();
    for (auto &e: entries)
        list.push_back(std::move(e.object));
    return { 200, json({ {"path", path}, {"parent", path == "/" ? json(nullptr) : json(parent_of(path))},
                         {"entries", list} }) };
}

ApiResult make_directory(const std::string &requested)
{
    const auto path = normalize_path(requested);
    if (path.empty())
        return api_error(400, "the path must be absolute: " + requested);
    if (path == "/")
        return api_error(400, "/ exists");

    struct stat st;
    const auto parent = parent_of(path);
    if (stat(parent.c_str(), &st) != 0)
        return path_error(parent, errno);
    if (!S_ISDIR(st.st_mode))
        return api_error(400, "not a directory: " + parent);
    if (lstat(path.c_str(), &st) == 0)
        return api_error(400, "already exists: " + path);
    if (mkdir(path.c_str(), 0777) != 0)
        return api_error(400, "cannot create " + path + ": " + std::strerror(errno));
    return { 200, json({ {"path", path} }) };
}

int read_report(const std::string &path, std::string &contents, ApiResult &result)
{
    const std::string suffix = ".flirt.json";
    if (path.empty() || path[0] != '/' || path.size() <= suffix.size()
        || path.compare(path.size() - suffix.size(), suffix.size(), suffix) != 0)
    {
        result = api_error(400, "not an absolute path ending in .flirt.json: " + path);
        return result.status;
    }
    struct stat st;
    if (stat(path.c_str(), &st) != 0)
    {
        result = path_error(path, errno);
        return result.status;
    }
    if (!S_ISREG(st.st_mode))
    {
        result = api_error(400, "not a file: " + path);
        return result.status;
    }
    if (st.st_size > 16 * 1024 * 1024)
    {
        result = api_error(400, "larger than 16 MiB: " + path);
        return result.status;
    }
    std::ifstream in(path, std::ios::binary);
    contents.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (!in.is_open() || in.bad())
    {
        result = api_error(400, "cannot read " + path + ": " + std::strerror(errno));
        return result.status;
    }
    return 200;
}

}
