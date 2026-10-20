#ifndef FLIRT_TUI_MODEL_H
#define FLIRT_TUI_MODEL_H

// flirt-tui's state and the logic that does not draw: the list, the options
// and their defaults, why Convert is disabled, the start body, the file
// browser (docs/design.md, "TUI"); a run's status, the headline and the
// progress are job.cc's display fields, shown as they are.

#include <cstdint>
#include <ctime>
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
    Jobs, Threads, Name, Title, Dither, FindDither
};

// The compression after `name` in compressions(), the first after the last.
std::string next_compression(const std::string &name);

// A view that opens in the right box, over its content (Browser, Help,
// Command, the channel picker Channels), or the quit question over the screen.
enum class Dialog { None, Browser, Help, Command, Channels, Quit };

struct BrowserEntry
{
    std::string name, type, id;     // type: dir, file, other
    int64_t size = 0, mtime = 0;
    bool listfile = false;
    bool up = false;                // the "../" row: a dir, the parent folder
};

// The file browser: listfile (add listfiles), analysis (one .analysis file),
// output (a folder); waveforms (one *_waveforms.root to look at); rawroot
// (one official *_raw.root, for "Find from a file...").
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

// The waveform view (docs/design.md, "TUI", "Waveform view"):
// flirt-waveforms's answers as they came (README "Events for programs"). A
// frame is one event's traces of one module.
struct Waves
{
    bool open = false;              // shown, its requests sent
    std::string file;
    json trees = json::array();     // [{name, title, traces}]
    int module = 0;                 // the tree asked for last (keys start from it)
    json channel;                   // the filter asked for last: null (all) or a sorted list of channels
    json frame;                     // the frame answer drawn, or null
    std::string error;              // the last answer's error
    uint64_t firstId = 0;           // this view's first request: older answers are another view's
    uint64_t lastId = 0, shownId = 0;   // the latest request sent, the latest answer applied
    int64_t wantPosition = -1;      // the position asked for last, -1 if it asked for an event
    json outbox;                    // the request to send next, or null
    // Y range own, per module (tree name): its numbers, and whether it is
    // chosen (else the typical range).
    struct Own
    {
        int lo = 0, hi = 0;
        bool on = false;
    };
    std::map<std::string, Own> own;
    int page = 0;                   // the grid: the page shown
    bool going = false;             // `g`: the event being typed
    std::string goText;
    bool ranging = false;           // the own range being typed (box 2's Y range row)
    std::string rangeText, rangeError;
};

// The channel picker, a view in the right box: the module's channels 0..max as
// checkboxes.
struct Picker
{
    std::string module;
    int max = -1;                   // the grid: channels 0 to max
    std::set<int> withTraces;       // the module's channels with traces (the others cannot be picked)
    std::set<int> checked;
    int cursor = 0;                 // the channel under the cursor
    int button = -1;                // -1: in the grid; 0 All, 1 None, 2 Done
};

// "Find from a file...": flirt-convert --find-dither-offset --json-events on
// one official _raw.root, its events applied as they come.
struct DitherFind
{
    bool running = false;
    uint64_t id = 0;                // this search's number: events of an older one are ignored
    int percent = 0;
    bool answered = false;          // its dither-offset event came
    std::string outbox;             // the file to search next (refresh() starts it)
    std::string note;               // a one-line note under the field (the run held twice)
    std::string error;              // why no offset was found
};

// A plot's ranges: x in samples from 0 to xMax, y in sample values.
struct PlotRanges
{
    double xMax = 1, yMin = -1, yMax = 1;
};

// A row of box 1's Waveforms tab: a waveforms file published this session
// or opened with + open.
struct WaveEntry
{
    std::string path;
    int64_t size = -1;
    bool opened = false;
};

// A listfile's run information for the details:
// flirt-info --json's answer, cached by path, size and mtime.
struct RunInfo
{
    bool known = false;             // size and mtime known (stat worked); else asked at every open
    int64_t size = -1;
    timespec mtime = {};
    bool reading = false;
    json answer;                    // flirt-info's JSON line or {"error": ...}; null while reading
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
    bool wavesTab = false;      // box 1's tab: Listfiles or Waveforms
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
    std::string infoShown;      // the row whose run information was asked for while shown
    int detailScroll = 0;
    bool reportShown = false;
    std::map<std::string, std::vector<std::string>> reports;    // report path -> lines
    std::map<std::string, std::string> reportErrors;            // report path -> error
    std::map<std::string, RunInfo> infos;                       // listfile path -> run information
    std::vector<std::string> infoOutbox;                        // paths to ask flirt-info about
    std::vector<std::string> infoDrops;                         // rows taken off while being read
    // What the last read-ahead plan saw (plan_info plans again only when one
    // changes or a read finished) and what was last sent.
    std::vector<std::string> infoPlannedRows;
    std::string infoPlannedCursor, infoSentCursor;
    bool infoPlannedBusy = false, infoSentBusy = false, infoSent = false;
    bool infoReplan = true;     // a read finished
    int infoPlans = 0;          // plans made (tests)
    int logScroll = 0;
    int helpScroll = 0;
    int commandScroll = 0;
    int quitChoice = 0;         // 0 Keep converting, 1 Stop and quit

    // The Waveforms tab: its files, the one shown, what each file showed.
    std::vector<WaveEntry> waveFiles;
    int waveRow = 0, waveCursor = 0;
    std::set<std::string> wavesNoted;       // a job's start and a published path, listed once
    std::map<std::string, Waves> waveViews; // file -> its module, filter, position, own y ranges, page
    int plotRow = 0;            // box 2's row: Module, Channels, Show, Y range, Traces
    Waves waves;
    uint64_t waveIds = 0;       // the last request id, over all files
    // The waveform view's settings for the session, whatever the file (not in
    // the settings file): Show (the grid, or overlaid), Traces, the play state
    // and speed (indexes into the presets, plot_layout.h).
    bool grid = true;
    int tracesPreset = 0, speedPreset = 2;
    bool playing = false;
    int64_t lastStepMs = 0;     // the last step of the play
    bool stepNow = false;       // the next tick steps at once (play just pressed)
    bool fullPlot = false;      // `z`: the plot fills the screen
    bool dragging = false;      // the left button held on the play bar
    Picker picker;
    // Over ssh (SSH_CONNECTION set when flirt-tui started): at most 100
    // traces drawn at once, play at most 10 events a second.
    bool ssh = false;
    int screenWidth = 80, screenHeight = 24;    // the last known terminal size

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

    DitherFind find;

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
// The left box the right box shows: box 1 in the Waveforms tab, else `chosen`.
int chosen_box(const Model &m);
// The focus to box 1, 2 or 3; box 1 or 2 is then the one chosen.
void set_box(Model &m, int box);
const FileEntry *selected_file(const Model &m);
const WaveEntry *selected_wave(const Model &m);

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

// The detail's run information (info_model.cc). Asks flirt-info (into the
// outbox) unless the cache has an answer or a reading for the file's size
// and mtime; a file that cannot be stat'ed is asked about every time it is
// shown (`again`), once by the read-ahead.
void request_info(Model &m, const std::string &path, bool again = true);
// The read-ahead (docs/design.md, "Reading several listfiles at once"):
// every row's run information asked for, the cursor's row first, then list
// order; a row taken off the list while being read is dropped (infoDrops).
// Only when the list, the cursor's row or a conversion's running changed, or
// a read finished: not at every key or play tick (each plan stats every
// row).
void plan_info(Model &m);
// An answer for the path it was asked for (never the JSON's "listfile"),
// with the file's size and mtime when it was read (size < 0: unknown).
void apply_info_answer(Model &m, const std::string &path, int64_t size, const timespec &mtime, const json &answer);
// The rate graph of run.minutes in at most `width` cells: one character per
// cell of g minutes (g = 1 if they fit, else ceil(n / width)), the cell's
// events / live seconds scaled to the largest cell with >= 10 live seconds;
// a paused cell (no live seconds) a space. `times`: the first and last
// minute's HH:MM under it, if at least 12 cells. Both empty below 10 cells of
// width or 2 cells of graph.
struct RateGraph
{
    std::string bars, times;
};
RateGraph rate_graph(const json &minutes, int width);

// The waveform view (wave_model.cc).
constexpr double SamplePeriodNs = 12.5;   // mvme's MdppDefaultSamplePeriod (mdpp_decode.h:21)
// The run's _waveforms.root among its outputs (job.cc), or empty.
std::string run_waveforms(const json *run);
// The Waveforms tab's list: every converted or skipped run's waveforms file of
// the job, once per job and path, before the files opened with + open (de-duplicated
// by path); a run still converting never appears.
void note_waveforms(Model &m);
// A file chosen in the browser's waveforms mode: listed (if not yet),
// selected, the Waveforms tab shown.
void open_waveforms_file(Model &m, const std::string &path, int64_t size);
// The file shown follows box 1's Waveforms tab: its selected file is shown
// (as it was left: module, filter, position, own y ranges), and none outside the tab.
void sync_waveforms(Model &m);
// Shows this file: as it was left this session, else from its tree list.
void show_waveforms(Model &m, const std::string &file);
// Stops showing it; what it showed is kept for the file.
void close_waveforms(Model &m);
bool waves_reading(const Model &m);     // the latest request is not answered yet
// A frame request for the current module with these fields; into the outbox.
void request_frame(Model &m, const json &fields);
// An answer (the helper's, or the reader's error with the request's id and
// file): ignored for a closed view, another file or an id not newer than the
// one shown. A tree list asks for the first frame of the first module with
// traces (else the first module).
void apply_waveform_answer(Model &m, const json &answer);
// The own range typed in box 2's Y range row: Enter keeps it if valid
// (else the refusal stays shown), Esc cancels (wave_model.cc).
void start_range(Model &m);
void choose_typical(Model &m);
bool finish_range(Model &m);

// "Find from a file..." (dither_find_model.cc): the search asked for a file
// (into the outbox), stopped, and each of its events (with the search's id).
void find_dither(Model &m, const std::string &file);
void stop_find(Model &m);
void apply_find_event(Model &m, uint64_t id, const json &event);

// Play (wave_play.cc). One step (+1 frame, never skipping) when the previous
// step's answer has been applied and 1000/speed ms have passed since the last
// step; at the last frame it pauses. True if it changed something drawn (it
// paused); a step's request goes to the outbox.
bool play_tick(Model &m, int64_t nowMs);
// Space, a click on the play button: play (from the first frame if at the
// last) or pause.
void toggle_play(Model &m);
void pause(Model &m);
// `-` and `+`: the next preset, clamped; the speed label: faster, wrapping.
void change_speed(Model &m, int delta, bool wrap = false);
int speed(const Model &m);      // events per second

}

#endif
