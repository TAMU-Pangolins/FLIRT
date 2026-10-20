#ifndef FLIRT_UI_INFO_H
#define FLIRT_UI_INFO_H

// A listfile's run information for flirt-tui's details (docs/design.md,
// "TUI" and "Run information"): runs
// <prefix>/bin/flirt-info --json once per listfile and returns its JSON line
// (README "Run information (flirt-info)").

#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <sys/types.h>

namespace flirt
{

class InfoReader
{
    public:
        using json = nlohmann::json;

        explicit InfoReader(std::string helperExe): helperExe_(std::move(helperExe)) { }
        ~InfoReader();
        InfoReader(const InfoReader &) = delete;
        InfoReader &operator=(const InfoReader &) = delete;

        // `flirt-info --json <listfile>`'s first stdout line,
        // parsed; one read at a time. An unreadable listfile (exit code 1 with
        // its line) is a normal answer. A failure of the helper itself
        // (missing, ended without a line, a line that is not JSON, stopped) is
        // {"error": "<sentence>"}.
        json read(const std::string &listfile);

        // May be called from another thread while read() waits: kills and
        // reaps the running helper, the waiting read() returns an error at
        // once, and every later read() does so without starting one. Nothing
        // is left running.
        void stop();

    private:
        const std::string helperExe_;
        std::mutex readMutex_;      // one read() at a time
        std::mutex pidMutex_;       // pid_ and stopped_, also for stop() from another thread
        pid_t pid_ = 0;
        bool stopped_ = false;
};

}

#endif
