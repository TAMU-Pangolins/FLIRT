#include "names.h"

#include "utf8.h"

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

namespace
{

bool standard_char(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'
        || c == '.' || c == ' ';
}

}

std::string nonstandard_name_reason(const std::string &name)
{
    for (size_t i = 0; i < name.size(); ++i)
    {
        const auto c = static_cast<unsigned char>(name[i]);
        if (standard_char(c))
            continue;
        if (c > 0x20 && c < 0x7F)
            return c == '"' ? "'\"' is not allowed" : "\"" + std::string(1, static_cast<char>(c)) + "\" is not allowed";
        // A whole character if it is valid UTF-8 and shown as it is; else
        // (a control character, a byte that is not UTF-8) it cannot be named.
        const size_t len = c < 0xC0 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        const auto character = name.substr(i, len);
        if (c >= 0x80 && character.size() == len && printable_text(character) == character)
            return "\"" + character + "\" is not allowed";
        return "an unprintable character is not allowed";
    }
    if (name.empty())
        return "an empty name is not allowed";
    if (name.find_first_not_of('.') == std::string::npos)
        return "a name of only dots is not allowed";
    if (name[0] == '-')
        return "a name must not start with \"-\"";
    if (name.front() == ' ' || name.back() == ' ')
        return "a name must not start or end with a space";
    return {};
}

std::string name_refusal(const std::string &name)
{
    const auto reason = nonstandard_name_reason(name);
    if (reason.empty())
        return {};
    return printable_text(name) + ": name not accepted: " + reason + " (use letters, digits, _ - . and spaces)";
}

std::string own_name(const std::string &path)
{
    const auto end = path.find_last_not_of('/');
    if (end == std::string::npos)
        return {};
    const auto slash = path.rfind('/', end);
    return path.substr(slash == std::string::npos ? 0 : slash + 1, end - (slash == std::string::npos ? 0 : slash + 1) + 1);
}

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
