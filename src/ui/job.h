#ifndef FLIRT_UI_JOB_H
#define FLIRT_UI_JOB_H

// flirt-tui's conversion job: one `flirt-convert --json-events ...` at a
// time, in its own process group, its events turned into the state the TUI
// shows (docs/design.md, "TUI").

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <sys/types.h>
#include <thread>
#include <vector>

#include "common/failures.h"

namespace flirt
{

// A shell word that pastes back as the same argument.
std::string shell_quote(const std::string &word);

class JobManager
{
    public:
        using json = nlohmann::json;

        // `wordsFor`: whose words a failed run's to-do uses (common/failures.h):
        // flirt-tui's (its keys).
        explicit JobManager(std::string convertExe, FailureFor wordsFor = FailureFor::Tui)
            : convertExe_(std::move(convertExe)), wordsFor_(wordsFor) { }
        ~JobManager();

        // Runs `flirt-convert --json-events <args>`. False with the reason if a
        // job is running (`busy`) or it cannot be started.
        bool start(const std::vector<std::string> &args, std::string &error, bool &busy);

        // SIGINT to the running batch, once; false if none is running.
        bool cancel();

        // {version, job}: at once if the version is newer than `after`, else
        // when it changes, at most `wait` later (or at shutdown()).
        json state(uint64_t after, std::chrono::milliseconds wait);

        // flirt-tui stops: a running job gets SIGINT (once) and is waited for.
        // True if a job was running.
        bool stop_and_wait();

        // Wakes the waiting state() calls; they answer at once from now on.
        void shutdown();

    private:
        struct RunState
        {
            std::string runId, input, action;
            uint64_t bytes = 0;
            std::string phase;      // pending, starting, converting, finishing, ended; empty: null
            int percent = -1;       // -1: null
            json result, note, seconds, report;
            std::vector<std::string> messages;
            unsigned warnings = 0, notes = 0;   // message lines starting with "warning: ", "note: "
            std::vector<std::string> outputs;   // the run's files in the output folder, once it has ended
            json hitFlags;          // the report's "hitFlags" if it was converted with them
        };

        struct Job
        {
            std::string state = "running";  // running, stopping, finished
            std::vector<std::string> command;
            int64_t started = 0;            // unix s
            std::chrono::steady_clock::time_point startClock, endClock, firstStart;
            bool anyStarted = false;        // a run has started (firstStart is set)
            std::vector<RunState> runs;
            bool planned = false;           // the plan event came
            // Lines about a run that came before the plan (why it is skipped
            // or refused): runId, text; attached to the run by the plan.
            std::vector<std::pair<std::string, std::string>> early;
            std::vector<std::string> messages;
            std::string outputDir;          // from the start event
            json usageError, exitCode, stoppedBy, summary;
            std::deque<std::string> log;    // the last 200 stderr lines
            pid_t pid = 0;
        };

        void read_output(int outFd, int errFd);
        void on_stdout_line(const std::string &line);
        void on_stderr_line(const std::string &line);
        void add_log(const std::string &line);
        RunState *find_run(const json &runId);
        std::string real_argument(const std::string &sent) const;
        void add_run_message(RunState &run, const std::string &text);
        json job_json() const;
        void changed();     // with mutex_ held

        const std::string convertExe_;
        const FailureFor wordsFor_;
        mutable std::mutex mutex_;
        std::condition_variable cv_;
        uint64_t version_ = 1;
        bool shutdown_ = false;
        std::unique_ptr<Job> job_;
        std::thread reader_;
};

}

#endif
