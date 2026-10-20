#ifndef FLIRT_TUI_PLOT_LAYOUT_H
#define FLIRT_TUI_PLOT_LAYOUT_H

// The waveform view's numbers (docs/design.md, "TUI", "Waveform view"): the
// presets,
// the y range (typical or own), the grid's layout and pages, its ticks, and
// how many past events are drawn (the ssh limit). Pure functions, shared by
// the drawing and the model, so both see the same plots.

#include <string>
#include <vector>

#include "model.h"

namespace flirt::tui
{

// Traces: the last N per channel, as mvme's "Max Traces per Channel" (1 to
// 100, external/mvme/src/analysis/waveform_sink_1d_widget.cc:24-30).
const std::vector<int> &traces_presets();       // 1, 2, 5, 10, 20, 50, 100
const std::vector<int> &speed_presets(bool ssh);    // 1, 2, 5, 10, 20, 50 events/s; over ssh up to 10
int traces_setting(const Model &m);

// Over ssh at most 100 traces are drawn at once (docs/design.md, "TUI",
// "Waveform view").
constexpr int SshMaxTraces = 100;
// The frames asked for over ssh: min(traces, max(1, 100 / n)) for n channels
// on screen at once (2 -> 50, 10 -> 10, 32 -> 3).
int ssh_request_depth(int traces, int channels);
// The past events drawn: `limit` (the answer's depth, at most Traces) locally;
// over ssh the largest d <= limit whose traces (ages < d, `ages` one per
// trace on screen) number 100 or less, never less than 1.
int drawn_depth(const std::vector<int> &ages, int limit, bool ssh);

// Where the plots go: the right box's inside or the full window's (`z`),
// width x rows cells, and the rows left for the plots and their labels
// (the play bar and the controls below them; the info row in the full window).
struct PlotArea
{
    int width = 36, rows = 18;
    bool full = false;
    int region() const { return rows - (full ? 3 : 2); }
};
PlotArea plot_area(const Model &m, int screenWidth, int screenHeight);

// ---------- the y range ----------

// A round step for `span` in about `n` parts: 1, 2 or 5 times a power of
// ten, the smallest at least span / n.
double nice_step(double span, int n);
// The next round step: 1 -> 2 -> 5 -> 10.
double next_step(double step);
// The y range drawn, whole numbers, lo < hi; `note`: why the typical range
// is not measured ("no traces to measure"), or empty.
struct YRange
{
    int lo = 0, hi = 1;
    std::string note;
};
// Rounded out: step = nice_step(hi - lo, 4), lo and hi to multiples of
// step / 2 (of step if step / 2 is not whole, at least 1).
YRange round_range(double lo, double hi);
// A tree's typical range (flirt-waveforms's "typical": the 1st percentile of
// its first 20,000 traces' minima and the 99th of their maxima), rounded
// out; no traces: 0 to 100; all equal: one step around them; with a note.
YRange typical_range(const json &tree);
// "lo to hi" (any two whole numbers, lo < hi); false with the refusal.
bool parse_range(const std::string &text, int &lo, int &hi, std::string &why);
// The tree of the module shown (null if none) and its range drawn: its own
// range when chosen, else the typical one.
const json *shown_tree(const Model &m);
bool own_chosen(const Model &m);
YRange y_range(const Model &m);
// The module's channels: the highest channel with traces in the tree + 1
// (the answer's `channels`), rounded up to a multiple of 4 (the module type
// says what it can do, not what is used).
int module_channels(const json &channels);
// The time axis's samples: the tree's most common trace length (typical),
// else the longest trace of the frame (at least 2).
int time_samples(const Model &m);

// ---------- the grid ----------

// Panels in rows of 4: a label column (`labelWidth`: the widest y number and
// a space) after a one-cell margin, each panel an axis column and `pw` plot
// columns, `gap` between panels; each grid row a title, `ph` plot rows and
// the time axis; the time numbers under the page's last row. As many rows
// as fit with ph >= 3 and pw >= 8 (`fit`); the rows on pages balanced:
// pages = ceil(rows / fit), rows per page = ceil(rows / pages).
struct GridLayout
{
    bool fits = false;
    int labelWidth = 4, gap = 1, pw = 8, ph = 3;
    int fit = 0, pages = 1, rowsPerPage = 1;
};
GridLayout grid_layout(const PlotArea &a, const YRange &y, int rows);
// The y ticks of a ph-row plot: round values (whole multiples of a round
// step, at least nice_step(hi - lo, 4)), at least two text rows apart (the
// step grows until they are), top first; each at the text row of its
// braille row.
struct Tick
{
    int row = 0, value = 0;
};
std::vector<Tick> y_ticks(const YRange &y, int ph);
// The time ticks of a pw-column plot of `samples` samples (12.5 ns each):
// multiples of nice_step(last, 3) ns, each at its column.
struct TimeTick
{
    int column = 0;
    double ns = 0;
};
std::vector<TimeTick> time_ticks(int samples, int pw);
// The numbers under a panel, 1 + pw cells: each tick's number from its
// column (+1 for the axis), " ns" after the last if it fits; a number that
// would touch another or pass the edge is left out.
std::string time_labels(int samples, int pw);

// ---------- what is drawn ----------

// What is drawn now (the view and the model use the same): Show grid and at
// least one row fits (else overlaid), the panels' channels and the page, the
// y range and the time samples, the depth d, the drawn traces (indexes into
// the frame answer's traces, which must outlive it).
struct Drawn
{
    bool grid = false;              // the grid drawn
    bool gridChosen = false;        // Show grid (drawn, or overlaid where it does not fit)
    GridLayout layout;
    YRange y;
    int samples = 2;
    std::vector<int> panels;        // the grid: every panel's channel, in order
    int pages = 1, page = 0;        // the page shown (clamped)
    int firstRow = 0;               // the page's first grid row
    std::vector<int> onScreen;      // the grid: the page's channels
    int depth = 0;                  // d: the ages drawn are 0..d-1
    bool lowered = false;           // over ssh, d < min(Traces, position + 1)
    const json *all = nullptr;      // the answer's traces
    std::vector<size_t> traces;     // the drawn ones (of the channels on screen), oldest first
    const json &trace(size_t i) const { return (*all)[traces[i]]; }
};
Drawn drawn(const Model &m, const PlotArea &a);
// The grid's panels (whatever Show is): with Channels all every channel of
// the module (module_channels), else each picked channel.
std::vector<int> grid_panels(const Model &m);
// The channels of the drawn traces, sorted.
std::vector<int> drawn_channels(const Drawn &d);
// A channel's drawn traces beyond the y range (of one channel, or all):
// "▲<max>" if one is above, else "▼<min>" if one is below, else empty.
std::string beyond_mark(const Drawn &d, const int *channel);
// A trace's fields without copying: its channel, age, samples.
int trace_channel(const json &trace);
int trace_age(const json &trace);
const json &trace_samples(const json &trace);

// The depth to ask the helper for: Traces locally; over ssh
// ssh_request_depth with the channels that can be on screen at once
// (the grid: a page's panels; overlaid: the picked ones, or all the module's).
int request_depth(const Model &m);

}

#endif
