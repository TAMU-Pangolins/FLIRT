#ifndef FLIRT_APPS_PROGRESS_H
#define FLIRT_APPS_PROGRESS_H

// Live progress of a batch on stderr (flirt-convert's parent, batch.cc).
// A child writes its progress lines (--progress-fd) to a file the parent
// reads with ProgressReader; ProgressDisplay shows them as a live line in a
// terminal (none in a log) and writes the batch's text above it.

#include <chrono>
#include <cstdint>
#include <string>
#include <sys/types.h>
#include <vector>

namespace flirt
{

// A run's state: its last complete progress line.
struct ProgressState
{
    enum class Phase { Starting, Progress, Finishing };

    Phase phase = Phase::Starting;
    uint64_t processed = 0, total = 0;

    // floor(100 * processed / total), -1 unless Progress with total > 0.
    int percent() const;
    // Share of the run done: Starting 0, Progress processed / total, Finishing 1.
    double fraction() const;
    // "starting", "<p>%", "running" (total 0) or "finishing".
    std::string text() const;
};

// Reads a child's progress file with pread only, at its own offset: the
// child's descriptor shares the file offset. Does not own the descriptor.
class ProgressReader
{
    public:
        explicit ProgressReader(int fd = -1): fd_(fd) { }

        // Reads what was appended since the last call.
        void poll();
        const ProgressState &state() const { return state_; }

    private:
        int fd_;
        off_t offset_ = 0;
        std::string partial_;   // an incomplete last line
        ProgressState state_;
};

class ProgressDisplay
{
    public:
        // A run this batch converts (skipped and outputs-exist runs are not).
        struct Run
        {
            enum class Phase { Pending, Running, Converted, Ended };

            std::string runId;
            uint64_t key = 0;       // the attempt (a watch may convert a run ID twice)
            uint64_t bytes = 0;     // input file size, for the estimate
            Phase phase = Phase::Pending;
            ProgressState state;    // Running
        };

        // Terminal mode if stderr is a terminal and TERM is set and not "dumb";
        // colour in terminal mode unless NO_COLOR is set and not empty.
        ProgressDisplay();
        ~ProgressDisplay();

        bool terminal() const { return terminal_; }
        bool colour() const { return colour_; }
        // Columns for the text: the terminal's, 80 in a log.
        size_t width() const;

        // Writes lines (without "\n") to stderr together, above the live line.
        void line(const std::string &text) { lines({ text }); }
        void lines(const std::vector<std::string> &texts);

        // Terminal mode: the live line of the running runs; log mode: nothing.
        // runs: in start order for the running ones; elapsed: seconds since
        // the first conversion started (0 before).
        void update(const std::vector<Run> &runs, double elapsed);

        // Terminal mode: shows `text` as the status line (an idle watch);
        // log mode: nothing.
        void idle(const std::string &text);

        // Clears the status line; no status line is drawn after.
        void finish();

    private:
        void draw();
        void clear();

        bool terminal_;
        bool colour_;
        bool finished_ = false;
        std::string status_;    // the status line's text, empty: none shown
        bool drawn_ = false;
        std::chrono::steady_clock::time_point lastDraw_;
};

}

#endif
