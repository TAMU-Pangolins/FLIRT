#ifndef FLIRT_UI_WAVEFORMS_H
#define FLIRT_UI_WAVEFORMS_H

// The waveform view's reader (docs/design.md, "TUI", "Waveform view"): runs
// <prefix>/bin/flirt-waveforms, which reads the waveforms file with ROOT in a
// process of its own, and asks it one request at a time (README "Events for
// programs" for the requests and answers).

#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <sys/types.h>

namespace flirt
{

class WaveformReader
{
    public:
        using json = nlohmann::json;

        explicit WaveformReader(std::string helperExe): helperExe_(std::move(helperExe)) { }
        ~WaveformReader();
        WaveformReader(const WaveformReader &) = delete;
        WaveformReader &operator=(const WaveformReader &) = delete;

        // The helper's answer to one request; one request at a time. It is
        // started at the first request, and again at the next one after it
        // died. A failure of the helper itself (missing, died, an answer that
        // is not JSON) is {"error": "<sentence>"}.
        json ask(const json &request);

        // May be called from another thread while ask() waits: closes the
        // helper's stdin, kills and reaps it; the waiting ask() returns an
        // error at once. Nothing is left running.
        void stop();

    private:
        bool start(std::string &error);         // with askMutex_ held
        void finish();                          // with askMutex_ held: closes, kills and reaps
        std::string died();                     // with askMutex_ held: why it ended
        void reap(int *status);

        const std::string helperExe_;
        std::mutex askMutex_;       // one ask() at a time; the fds belong to its holder
        std::mutex pidMutex_;       // pid_ and stopping_, also for stop() from another thread
        pid_t pid_ = 0;
        bool stopping_ = false;
        int in_ = -1, out_ = -1;    // the helper's stdin and stdout
        std::string buffer_;        // what was read after the last answer
};

}

#endif
