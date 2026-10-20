#include "settings.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <nlohmann/json.hpp>
#include <sys/stat.h>
#include <unistd.h>

#include "common/utf8.h"

namespace flirt::tui
{

using json = nlohmann::json;

namespace
{

bool absolute(const std::string &path)
{
    return !path.empty() && path[0] == '/';
}

// mkdir -p for the directory holding `path`; 0, or errno with the folder
// that failed in `failed`.
int make_parents(const std::string &path, std::string &failed)
{
    for (size_t slash = path.find('/', 1); slash != std::string::npos; slash = path.find('/', slash + 1))
    {
        const auto dir = path.substr(0, slash);
        if (mkdir(dir.c_str(), 0777) != 0 && errno != EEXIST)
        {
            const int error = errno;
            failed = dir;
            return error;
        }
    }
    return 0;
}

}

std::string settings_path(const std::string &exe)
{
    if (const char *own = std::getenv("FLIRT_TUI_SETTINGS"); own && absolute(own))
        return own;
    if (!absolute(exe))
        return {};
    // <prefix>/bin/flirt-tui -> <prefix> (empty if the bin folder is at the top).
    const auto bin = exe.substr(0, exe.rfind('/'));
    const auto prefix = bin.substr(0, bin.rfind('/'));
    return prefix + "/settings/flirt-tui.json";
}

Settings load_settings(const std::string &path, std::string *problem)
{
    Settings s;
    if (problem)
        problem->clear();
    if (path.empty())
        return s;
    const std::string defaults = "Starting with the default choices: ";
    std::string text;
    {
        FILE *in = std::fopen(path.c_str(), "rb");
        if (!in)
        {
            // A missing file (or folder) is the first session: no problem.
            if (errno != ENOENT && errno != ENOTDIR && problem)
                *problem = defaults + "cannot read " + path + " (" + std::strerror(errno) + ")";
            return s;
        }
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), in)) > 0)
            text.append(buf, n);
        const int readError = std::ferror(in) ? errno : 0;
        std::fclose(in);
        if (readError)
        {
            if (problem)
                *problem = defaults + "cannot read " + path + " (" + std::strerror(readError) + ")";
            return s;
        }
    }
    try
    {
        const auto j = json::parse(text);
        if (!j.is_object())
        {
            if (problem)
                *problem = defaults + path + " is not a settings file";
            return s;
        }
        auto str = [&](const char *key, std::string &value, bool needAbsolute) {
            const auto it = j.find(key);
            if (it != j.end() && it->is_string() && (!needAbsolute || absolute(it->get<std::string>())))
                value = it->get<std::string>();
        };
        auto boolean = [&](const char *key, bool &value) {
            const auto it = j.find(key);
            if (it != j.end() && it->is_boolean())
                value = it->get<bool>();
        };
        str("outputDir", s.outputDir, true);
        std::string analysis;
        str("analysis", analysis, false);
        if (analysis == "archive" || analysis == "default-filters" || analysis == "file")
            s.analysis = analysis;
        str("analysisPath", s.analysisPath, true);
        boolean("waveforms", s.waveforms);
        boolean("hitFlags", s.hitFlags);
        // Other keys are ignored: "lowPriority" of older files is dropped at
        // the next save. lastDir "output" of older files is ignored: the
        // output browser opens in the output folder.
        const auto dirs = j.find("lastDir");
        if (dirs != j.end() && dirs->is_object())
            for (const char *mode: { "listfile", "analysis", "waveforms", "rawroot" })
            {
                const auto it = dirs->find(mode);
                if (it != dirs->end() && it->is_string() && absolute(it->get<std::string>()))
                    s.lastDir[mode] = it->get<std::string>();
            }
    }
    catch (const std::exception &)
    {
        if (problem)
            *problem = defaults + path + " is not a settings file";
        return Settings();
    }
    return s;
}

bool save_settings(const std::string &path, const Settings &s, std::string *problem)
{
    const std::string notRemembered = "Your choices are not remembered: ";
    auto fail = [&](const std::string &what, int error) {
        if (problem)
            *problem = notRemembered + what + (error ? std::string(" (") + std::strerror(error) + ")" : "");
        return false;
    };
    if (path.empty())
        return fail("no place for the settings file was found", 0);
    std::string failed;
    if (const int error = make_parents(path, failed))
        return fail("cannot make the folder " + failed, error);
    // A path that is not valid UTF-8 is saved as not set; the rest is saved.
    auto path_or_unset = [](const std::string &p) { return valid_utf8(p) == p ? p : std::string(); };
    json dirs = json::object();
    for (const auto &[mode, dir]: s.lastDir)
        if (!path_or_unset(dir).empty())
            dirs[mode] = dir;
    const json j = { {"outputDir", path_or_unset(s.outputDir)}, {"analysis", s.analysis},
                     {"analysisPath", path_or_unset(s.analysisPath)},
                     {"waveforms", s.waveforms}, {"hitFlags", s.hitFlags}, {"lastDir", dirs} };
    std::string text;
    try
    {
        text = dump_valid_utf8(j, 2) + "\n";
    }
    catch (const std::exception &)
    {
        return fail("cannot write " + path, 0);
    }
    const auto tmp = path + ".tmp." + std::to_string(getpid());
    FILE *out = std::fopen(tmp.c_str(), "wb");
    if (!out)
        return fail("cannot write " + path, errno);
    int error = 0;
    if (std::fwrite(text.data(), 1, text.size(), out) != text.size() || std::fflush(out) != 0)
        error = errno ? errno : EIO;
    if (std::fclose(out) != 0 && !error)
        error = errno ? errno : EIO;
    if (error)
    {
        std::remove(tmp.c_str());
        return fail("cannot write " + path, error);
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0)
    {
        error = errno;
        std::remove(tmp.c_str());
        return fail("cannot write " + path, error);
    }
    return true;
}

}
