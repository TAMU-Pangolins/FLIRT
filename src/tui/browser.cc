// flirt-tui's file browser: the listfile, analysis and output modes over
// list_directory/make_directory, and the TUI's own waveforms mode (one
// *_waveforms.root for the Waveforms tab) and rawroot mode (one official
// *_raw.root for "Find from a file...").

#include <algorithm>

#include <sys/stat.h>

#include "common/names.h"
#include "model.h"
#include "text.h"
#include "ui/files.h"

namespace flirt::tui
{

namespace
{

// `path` (~ expanded) if it exists, else its nearest existing parent folder;
// "" for none; a path that is not absolute as it is.
std::string nearest_existing(const Model &m, std::string path)
{
    if (path == "~" || path.rfind("~/", 0) == 0)
        path = m.home + path.substr(1);
    const auto given = path;
    path = normalize_path(path);
    if (path.empty())
        return given;   // not absolute: browse's error, as before
    struct stat st;
    while (!path.empty() && stat(path.c_str(), &st) != 0)
    {
        if (path == "/")
            return "";
        const auto slash = path.rfind('/');
        path = slash == 0 ? "/" : path.substr(0, slash);
    }
    return path;
}

}

void open_browser(Model &m, const std::string &mode)
{
    m.browser = Browser();
    m.browser.mode = mode;
    m.dialog = Dialog::Browser;
    // Output: the current output folder, or its nearest existing parent;
    // not the last folder browsed. The others: the last folder browsed in
    // that mode.
    std::string start;
    if (mode == "output")
        start = nearest_existing(m, m.outputDir);
    else
    {
        const auto last = m.lastDir.find(mode);
        start = last != m.lastDir.end() ? last->second : "";
        if (start.empty() && mode == "waveforms")
            start = m.outputDir;
    }
    if (start.empty())
        start = m.home;
    if (!browse(m, start) && start != m.home)
    {
        const auto error = m.browser.error;
        if (browse(m, m.home))
            m.browser.error = error;
    }
}

bool browse(Model &m, const std::string &requested)
{
    auto &b = m.browser;
    std::string path = requested;
    if (path == "~" || path.rfind("~/", 0) == 0)
        path = m.home + path.substr(1);
    const auto result = list_directory(path);
    if (result.status != 200)
    {
        b.error = result.body.value("error", "cannot open " + path);
        return false;
    }
    b.error.clear();
    b.path = result.body["path"].get<std::string>();
    b.hasParent = result.body["parent"].is_string();
    b.parent = b.hasParent ? result.body["parent"].get<std::string>() : "";
    b.entries.clear();
    // "../" first where there is a parent (Enter to go up); a folder row, so
    // Space, a (add all) and the counts ignore it.
    if (b.hasParent)
    {
        BrowserEntry up;
        up.name = "..";
        up.type = "dir";
        up.up = true;
        b.entries.push_back(up);
    }
    for (const auto &e: result.body["entries"])
    {
        BrowserEntry entry;
        entry.name = e["name"].get<std::string>();
        entry.type = e["type"].get<std::string>();
        entry.id = e["id"].get<std::string>();
        entry.size = e["size"].get<int64_t>();
        entry.mtime = e["mtime"].get<int64_t>();
        entry.listfile = e["listfile"].get<bool>();
        const bool shown = entry.type == "dir" || (b.mode == "listfile" && entry.listfile)
            || (b.mode == "analysis" && entry.type == "file" && entry.name.size() > 9
                && entry.name.compare(entry.name.size() - 9, 9, ".analysis") == 0)
            || (b.mode == "waveforms" && entry.type == "file" && entry.name.size() > 15
                && entry.name.compare(entry.name.size() - 15, 15, "_waveforms.root") == 0)
            || (b.mode == "rawroot" && entry.type == "file" && entry.name.size() > 9
                && entry.name.compare(entry.name.size() - 9, 9, "_raw.root") == 0);
        if (shown)
            b.entries.push_back(entry);
    }
    b.listed = true;
    // On the first real entry when there is one.
    b.cursor = b.hasParent && b.entries.size() > 1 ? 1 : 0;
    b.picked.clear();
    if (b.mode != "output")
        m.lastDir[b.mode] = b.path;
    return true;
}

bool is_added(const Model &m, const std::string &id)
{
    return std::any_of(m.files.begin(), m.files.end(), [&](const FileEntry &f) { return f.id == id; });
}

int addable_count(const Model &m)
{
    int n = 0;
    for (const auto &e: m.browser.entries)
        n += e.type != "dir" && !is_added(m, e.id);
    return n;
}

std::vector<std::string> browser_buttons(const Model &m)
{
    const auto &b = m.browser;
    if (b.mode == "listfile")
    {
        const int n = addable_count(m);
        return { "Add " + std::to_string(b.picked.size()) + " selected",
                 "Add all " + std::to_string(n) + " listfile" + (n == 1 ? "" : "s") + " in this folder", "Cancel" };
    }
    if (b.mode == "output")
        return { "Choose this folder", "New folder...", "Cancel" };
    if (b.mode == "waveforms")
        return { "Open", "Cancel" };
    if (b.mode == "rawroot")
        return { "Find its offset", "Cancel" };
    return { "Choose", "Cancel" };
}

// enter: Enter or a double-click: a folder opened; a file added (listfile,
// with the ones picked), chosen (analysis) or opened (waveforms), the browser
// closed. Else Space or a click on a file: picked (listfile: toggled).
void browser_activate(Model &m, bool enter)
{
    auto &b = m.browser;
    if (b.cursor < 0 || b.cursor >= static_cast<int>(b.entries.size()))
        return;
    const auto e = b.entries[static_cast<size_t>(b.cursor)];
    if (e.up)
    {
        browse(m, b.parent);
        return;
    }
    if (e.type == "dir")
    {
        browse(m, join_path(b.path, e.name));
        return;
    }
    // Only a standard name is added, chosen or opened (common/names.h).
    if (const auto refusal = name_refusal(e.name); !refusal.empty())
    {
        b.error = refusal;
        return;
    }
    b.error.clear();
    if (b.mode == "listfile")
    {
        if (is_added(m, e.id))
            return;
        if (enter)
        {
            b.picked.insert(e.id);
            return browser_ok(m);
        }
        if (b.picked.count(e.id))
            b.picked.erase(e.id);
        else
            b.picked.insert(e.id);
    }
    else if (b.mode == "analysis" || b.mode == "waveforms" || b.mode == "rawroot")
    {
        b.picked = { e.id };
        if (enter)
            browser_ok(m);
    }
}

namespace
{

FileEntry file_entry(const std::string &dir, const BrowserEntry &e)
{
    FileEntry f;
    f.path = join_path(dir, e.name);
    f.name = e.name;
    f.id = e.id;
    f.size = e.size;
    return f;
}

}

void browser_ok(Model &m)
{
    auto &b = m.browser;
    if (b.mode == "listfile")
    {
        if (b.picked.empty())
            return;
        std::vector<FileEntry> add;
        for (const auto &e: b.entries)
            if (b.picked.count(e.id))
                add.push_back(file_entry(b.path, e));
        add_files(m, add);
    }
    else if (b.mode == "output")
    {
        if (!b.listed)
            return;
        // The folder above may hold any name; this one must be standard.
        if (const auto refusal = name_refusal(own_name(b.path)); !refusal.empty() && b.path != "/")
        {
            b.error = "This folder's name is not accepted: " + refusal;
            return;
        }
        m.outputDir = b.path;
        m.goError.clear();
    }
    else
    {
        const auto e = std::find_if(b.entries.begin(), b.entries.end(),
                                    [&](const BrowserEntry &x) { return b.picked.count(x.id) > 0; });
        if (e == b.entries.end())
            return;
        if (b.mode == "rawroot")
        {
            // Searched in the background (refresh() starts it).
            find_dither(m, join_path(b.path, e->name));
            m.dialog = Dialog::None;
            return;
        }
        if (b.mode == "waveforms")
        {
            // Into the Waveforms tab's list, shown there.
            open_waveforms_file(m, join_path(b.path, e->name), e->size);
            m.dialog = Dialog::None;
            return;
        }
        m.opts.analysis = "file";
        m.opts.analysisPath = join_path(b.path, e->name);
    }
    m.dialog = Dialog::None;
}

void browser_add_all(Model &m)
{
    auto &b = m.browser;
    std::vector<FileEntry> add;
    std::string refused;    // the first name not accepted
    int refusedCount = 0;
    for (const auto &e: b.entries)
        if (e.type != "dir" && !is_added(m, e.id))
        {
            const auto refusal = name_refusal(e.name);
            if (refusal.empty())
                add.push_back(file_entry(b.path, e));
            else if (refusedCount++ == 0)
                refused = refusal;
        }
    add_files(m, add);
    // Names not accepted: the browser stays open and says so.
    if (refusedCount)
    {
        b.error = (add.empty() ? "Nothing added: " : std::to_string(add.size()) + " added; not added: ") + refused
            + (refusedCount > 1 ? "; and " + std::to_string(refusedCount - 1) + " more not accepted" : "");
        return;
    }
    if (add.empty())
        return;
    m.dialog = Dialog::None;
}

void browser_create_folder(Model &m, const std::string &typed)
{
    auto &b = m.browser;
    auto name = typed;
    name.erase(0, name.find_first_not_of(" \t"));
    name.erase(name.find_last_not_of(" \t") + 1);
    if (name.empty() || name.find('/') != std::string::npos)
    {
        b.error = "A folder name, without /";
        return;
    }
    if (const auto refusal = name_refusal(name); !refusal.empty())
    {
        b.error = refusal;
        return;
    }
    const auto result = make_directory(join_path(b.path, name));
    if (result.status != 200)
    {
        b.error = result.body.value("error", "Cannot create the folder");
        return;
    }
    b.editing = false;
    b.zone = Browser::Zone::List;
    browse(m, result.body["path"].get<std::string>());
}

void browser_button(Model &m, int index)
{
    const auto buttons = browser_buttons(m);
    if (index < 0 || index >= static_cast<int>(buttons.size()))
        return;
    const auto &label = buttons[static_cast<size_t>(index)];
    auto &b = m.browser;
    if (label == "Cancel")
        m.dialog = Dialog::None;
    else if (label == "New folder...")
    {
        b.zone = Browser::Zone::NewFolder;
        b.editing = true;
        b.editText.clear();
    }
    else if (label.rfind("Add all ", 0) == 0)
        browser_add_all(m);
    else
        browser_ok(m);
}

}
