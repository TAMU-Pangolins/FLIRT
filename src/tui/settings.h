#ifndef FLIRT_TUI_SETTINGS_H
#define FLIRT_TUI_SETTINGS_H

// What flirt-tui remembers between sessions, in
// <prefix>/settings/flirt-tui.json, or the file FLIRT_TUI_SETTINGS names
// (docs/design.md, "TUI", "Remembered settings"). Not the listfiles, and no
// Advanced option: a remembered dither offset, accept-damaged setting or
// experiment name would silently change the next session's output or safety.

#include <map>
#include <string>

namespace flirt::tui
{

struct Settings
{
    std::string outputDir;
    std::string analysis = "archive";   // archive, default-filters, file
    std::string analysisPath;
    bool waveforms = true;
    bool hitFlags = false;      // Add pile-up/overflow flags
    std::map<std::string, std::string> lastDir;     // browser mode -> folder
};

// The file's path for the flirt-tui at `exe` (a real path, <prefix>/bin/flirt-tui):
// FLIRT_TUI_SETTINGS if it is an absolute path (a relative one is ignored),
// else <prefix>/settings/flirt-tui.json; empty if `exe` is not absolute.
std::string settings_path(const std::string &exe);

// The remembered settings; defaults for anything missing, unreadable or
// invalid. `problem` (if given) gets a plain sentence when the file exists
// but cannot be read or is not a settings file; empty otherwise (a missing
// file is no problem, nor is a value of the wrong type).
Settings load_settings(const std::string &path, std::string *problem = nullptr);

// Writes atomically (a temporary file, then rename), making its folder
// first; false on failure, with a plain sentence in `problem` (if given).
bool save_settings(const std::string &path, const Settings &settings, std::string *problem = nullptr);

}

#endif
