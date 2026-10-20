#ifndef FLIRT_UI_DITHER_FIND_H
#define FLIRT_UI_DITHER_FIND_H

// flirt-tui's "Find from a file..." (docs/design.md, "TUI"): runs
// `<prefix>/bin/flirt-convert --find-dither-offset --json-events <file>` and
// hands each event line on (README "Events for programs"), as InfoReader runs
// flirt-info (info.h).

#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <sys/types.h>

namespace flirt
{

class DitherFinder
{
    public:
        using json = nlohmann::json;

        explicit DitherFinder(std::string convertExe): convertExe_(std::move(convertExe)) { }
        ~DitherFinder();
        DitherFinder(const DitherFinder &) = delete;
        DitherFinder &operator=(const DitherFinder &) = delete;

        // Runs the search on `file` and calls `event` with each JSON event
        // line as it comes; last with {"event": "exit"} once the process has
        // ended (with "error": "<sentence>" if it could not start, gave a line
        // that is not JSON, or ended without its "end" event). Blocks.
        void run(const std::string &file, const std::function<void(const json &)> &event);

        // May be called from another thread: kills and reaps the running
        // search (run() then returns at once), and every later run() does
        // nothing. Nothing is left running.
        void stop();

    private:
        const std::string convertExe_;
        std::mutex runMutex_;       // one run() at a time
        std::mutex pidMutex_;       // pid_ and stopped_
        pid_t pid_ = 0;
        bool stopped_ = false;
};

}

#endif
