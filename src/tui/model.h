#ifndef FLIRT_TUI_MODEL_H
#define FLIRT_TUI_MODEL_H

// flirt-tui's state and the logic that does not draw: the list, the options
// and their defaults, why Convert is disabled, the start body, the file
// browser (docs/design.md, "TUI"); a run's status, the headline and the
// progress are job.cc's display fields, shown as they are.

#include <cstdint>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <vector>

#include "common/compression.h"
#include "settings.h"

namespace flirt::tui
{

using json = nlohmann::json;

struct FileEntry
{
    std::string path, name, id;     // id: device:inode (list_directory)
    int64_t size = -1;
    bool checked = true;
};

// The options and their defaults; the numbers as typed.
struct Options
{
    std::string analysis = "archive";   // archive, default-filters, file
    std::string analysisPath;
    bool waveforms = true, hitFlags = false, official = false, acceptDamaged = false;
    std::string jobs = "1", threads = "1", ditherOffset = "0";
    std::string experimentName = "Experiment", experimentTitle;
    std::string compression = default_compression().name;  // common/compression.h
};

// The options form's items (the right box with box 2 chosen), in their order.
enum class Item
{
    Output, AnalysisArchive, AnalysisDefault, AnalysisFile, Waveforms, HitFlags, Official, Damaged, Compression,
    Jobs, Threads, Name, Title, Dither
};

// The compression after `name` in compressions(), the first after the last.
std::string next_compression(const std::string &name);

// A view that opens in the right box, over its content (Browser, Help,
// Command), or the quit question over the screen.
enum class Dialog { None, Browser, Help, Command, Quit };

struct BrowserEntry
{
    std::string name, type, id;     // type: dir, file, other
    int64_t size = 0, mtime = 0;
    bool listfile = false;
    bool up = false;                // the "../" row: a dir, the parent folder
};

// The file browser: listfile (add listfiles), analysis (one .analysis file),
// output (a folder).
struct Browser
{
    enum class Zone { List, Path, NewFolder, Buttons };

    std::string mode, path;
    bool listed = false;
    bool hasParent = false;
    std::string parent;
    std::vector<BrowserEntry> entries;  // the ones shown in this mode
    int cursor = 0;
    std::set<std::string> picked;       // ids
    std::string error;
    Zone zone = Zone::List;
    int button = 0;
    bool editing = false;               // the path or the new folder's name
    std::string editText;
};

struct Model
{
    // The help's first lines: the version, the machine and the user (empty
    // when unknown).
    std::string version, host, user, home;

    std::vector<FileEntry> files;
    std::string outputDir;
    Options opts;
    std::map<std::string, std::string> lastDir;     // browser mode -> folder

    // The boxes (docs/design.md, "TUI", "Layout"): 1 and 2 on the left, 3 the
    // right box.
    int box = 1;                // the focus
    int chosen = 1;             // the left box the right box shows (the focus when it is 1 or 2)
    int back = 0;               // where a view in the right box returns (the focus it was opened from)
    bool logTab = false;        // the right box's tab (Listfiles, box 1 chosen): Details or Log

    int fileRow = 0;            // box 1's selected listfile
    int fileCursor = 0;         // the list's row kept in view
    // A conversion started with s: box 1's selection follows each run as it
    // starts, until the user moves it (follow_runs, input.cc).
    bool following = false;
    std::set<std::string> runsStarted;  // this job's runs seen started (runId)
    Item focus = Item::Output;  // the options form's item
    bool editing = false;       // the focused text field
    std::string editText;

    Dialog dialog = Dialog::None;
    Browser browser;
    std::string detailPath;     // the row the details show
    int detailScroll = 0;
    bool reportShown = false;
    std::map<std::string, std::vector<std::string>> reports;    // report path -> lines
    std::map<std::string, std::string> reportErrors;            // report path -> error
    int logScroll = 0;
    int helpScroll = 0;
    int commandScroll = 0;
    int quitChoice = 0;         // 0 Keep converting, 1 Stop and quit

    // The mouse: on unless --no-mouse (the help and command texts say which);
    // the last left press, for double-clicks and the guard against a press on
    // what a changed screen put under the pointer.
    bool mouse = true;
    struct Press
    {
        bool valid = false;
        int kind = 0;           // Target::Kind
        int index = 0;
        Item item = Item::Output;
        int x = -1, y = -1;
        int64_t ms = 0;
        bool changed = false;   // it opened or closed a view, or opened a folder
    } lastPress;

    json job;                   // null, or JobManager's job (its display fields)
    uint64_t jobVersion = 0;    // JobManager's version of `job`
    std::string goError;        // why the last Convert did not start, or that the settings are not read or not remembered
};

bool busy(const Model &m);
int checked_count(const Model &m);
// The last job's run for this input, or null.
const json *run_for(const Model &m, const std::string &path);

// A view shown in the right box (browser, help, command).
bool is_view(Dialog d);
// The left box the right box shows: `chosen`.
int chosen_box(const Model &m);
// The focus to box 1, 2 or 3; box 1 or 2 is then the one chosen.
void set_box(Model &m, int box);
const FileEntry *selected_file(const Model &m);

// "Convert 2 runs", "Stop", "Stopping...".
std::string convert_label(const Model &m);
// Why Convert is disabled; empty if it is not.
std::string convert_reason(const Model &m);

// The start body (src/ui/start_args.h); null with `error` if an
// Advanced number is not a whole number.
json start_body(const Model &m, std::string &error);

// The options form's items, in their order (one line each).
const std::vector<Item> &form_items();
bool focused(const Model &m, Item item);
bool is_text_field(Item item);
bool is_number_field(Item item);
std::string &field_value(Model &m, Item item);
const std::string &field_value(const Model &m, Item item);

// Adds entries not in the list yet (by id, else path), checked.
void add_files(Model &m, const std::vector<FileEntry> &entries);

Settings to_settings(const Model &m);
void apply_settings(Model &m, const Settings &s);

// The file browser.
void open_browser(Model &m, const std::string &mode);
bool browse(Model &m, const std::string &path);
bool is_added(const Model &m, const std::string &id);
int addable_count(const Model &m);
std::vector<std::string> browser_buttons(const Model &m);
// Enter (fromKey) or Space on the entry under the cursor.
void browser_activate(Model &m, bool enter);
void browser_ok(Model &m);
void browser_add_all(Model &m);
void browser_create_folder(Model &m, const std::string &name);
void browser_button(Model &m, int index);

// The report's lines (pretty-printed JSON), read once per job.
void load_report(Model &m, const std::string &path);

}

#endif
