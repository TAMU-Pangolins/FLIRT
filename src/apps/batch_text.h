#ifndef FLIRT_APPS_BATCH_TEXT_H
#define FLIRT_APPS_BATCH_TEXT_H

// flirt-convert's human-readable text (README "What flirt-convert prints"):
// one block per run, printed when the run ends, and the end of the batch. The
// blocks are made from structured data, a converted run's from its report;
// batch.cc makes the others from its Run state. Only this text: the
// --json-events stream, the reports and --summary do not come from here.

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace flirt
{

// The colours.
enum class Tone { Plain, Ok, Skip, Warning, Error, Busy, Label, Dim };

// Text with the terminal's colour codes, or as it is when off.
class Painter
{
    public:
        explicit Painter(bool on = false): on_(on) { }
        bool on() const { return on_; }
        std::string operator()(const std::string &text, Tone tone) const;
        std::string bold(const std::string &text) const;

    private:
        bool on_;
};

struct BlockRow
{
    std::string label;      // run, events, files, lost, quality, note, warning, why, to do, ...
    std::string text;
    std::string shared;     // a note: its words when said once at the end for several runs
};

// What is said about one run.
struct RunBlock
{
    std::string runId;
    std::string mark;       // ✓ – ! ✗ ●
    Tone tone = Tone::Plain;
    std::string outcome;    // the coloured word: "converted", "skipped", "not converted", "failed", ...
    std::string rest;       // after it: " in 5.4 s", ": already converted ..."
    std::vector<BlockRow> rows;
    size_t warnings = 0;    // warning rows
    std::string attention;  // why it needs attention, for the end; empty: it needs none
};

// "5.4 s" under a minute, else "2 min 35 s" (run_summary.h format_duration).
std::string elapsed_text(double seconds);

// A converted run's block from its report (the .flirt.json, parsed).
RunBlock converted_block(const std::string &runId, double seconds, const nlohmann::json &report);

// Adds a warning row and counts it.
void add_warning(RunBlock &block, const std::string &text);

// The block's lines, wrapped at `width` columns; notes only if `notes`.
std::vector<std::string> render_block(const RunBlock &block, bool notes, const Painter &paint, size_t width);

// "    <label>   <text>": the label in its column, the text wrapped under the
// value column.
std::vector<std::string> render_row(const std::string &label, const std::string &text, const Painter &paint,
                                    size_t width);

// The end: "Done: ...", the runs that need attention, then the notes of
// several runs (said once). `blocks`: every run
// of the batch, in order; `counted`: their results' counts in "Done" words;
// `notesInBlocks`: the notes were said in the blocks (a single run, a watch).
std::vector<std::string> render_end(const std::vector<const RunBlock *> &blocks, const std::string &counts,
                                    size_t runsGiven, bool notesInBlocks, const Painter &paint, size_t width);

}

#endif
