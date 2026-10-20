#ifndef FLIRT_ROOT_OUTPUT_H
#define FLIRT_ROOT_OUTPUT_H

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <event_server/common/event_server_lib.h>

#include "flirt/message_batch.h"

namespace flirt
{

struct RootOutputOptions
{
    std::string outDir;             // existing directory receiving the outputs
    std::string mvmePrefix;         // holds include/mvme/mvme_root_event_objects.h
    // The listfile's run ID, for the output names and MVMERunInfo's RunID;
    // BeginRun carries it with invalid UTF-8 replaced (extract.cc).
    std::string runId;
    uint64_t ditherOffset = 0;      // TRandom3(4357) draws to skip before the first value
    // > 1: ROOT::EnableImplicitMT(implicitMTThreads), so ROOT compresses
    // baskets in parallel. The output is identical for any value.
    unsigned implicitMTThreads = 1;
    // The raw and histogram files' compression, as TFile takes it
    // (common/compression.h). 101, the TFile default mvme_root_client gets,
    // keeps the files identical to official ones.
    int compression = 101;
    // ROOT::EnableThreadSafety() before any other ROOT use, for another thread
    // writing its own ROOT file (the waveform writer).
    bool threadSafety = false;
    // Per event index: transmitted data-source indices whose values are not
    // written. They get no class member, branch or histogram, but each of
    // their values still takes its dither draw, so every other value is
    // stored exactly as mvme_root_client stores it.
    std::vector<std::vector<size_t>> omitDataSources;
};

struct RootOutputResult
{
    std::string rawFile;            // published <outDir>/<runId>_raw.root
    std::string histosFile;         // published <outDir>/<runId>_histos.root
    std::vector<std::string> extraFiles; // published addFile() files, in order
    std::vector<size_t> eventHits;  // tree entries per VME event
    // Each event tree's name and top-level branches (the modules), for
    // --dither-offset-from's same-run check.
    std::vector<std::pair<std::string, std::vector<std::string>>> eventTrees;
    size_t totalDataBytes = 0;
    // The files' sizes, as written (and as published).
    uint64_t rawBytes = 0, histosBytes = 0;
    std::vector<uint64_t> extraBytes;
};

// Wall time of the writer's stages, in seconds.
struct RootTimings
{
    double setup = 0;       // BeginRun: classes generated and compiled, files and histograms created
    double fill = 0;        // EventData batches: dither, tree and histogram fills
    double writeClose = 0;  // raw file written at EndRun, then closed
    double histograms = 0;  // histogram file written and closed at EndRun
    double publish = 0;     // sync(): outputs flushed to disk (fsync) before the report is made
};

// Produces mvme_root_client's output files from event-server Messages, in this
// process. Feed every message in order, then call close() after EndRun,
// sync() and publish() to move the outputs and the report into place. Outputs
// are written to a private temporary directory inside outDir; existing files
// are never replaced, and outputs not published are removed with the writer.
class RootWriter
{
    public:
        explicit RootWriter(const RootOutputOptions &options);
        ~RootWriter();

        void handleMessage(const mvme::event_server::Message &msg);
        // The consumer of run_pipeline(): the messages of one batch, in order.
        void handleBatch(const MessageBatch &batch);
        // After EndRun: closes the output files, still in the private directory.
        void close();
        // Before BeginRun: creates the private directory for the run `runId`
        // (the run ID BeginRun will carry) if needed and reserves `name` in
        // it, for a file someone else writes. Returns its absolute path.
        // publish() links it after the raw and histogram files, never over an
        // existing file; it is removed with the writer if not published.
        std::string addFile(const std::string &runId, const std::string &name);
        // After close(), and after every addFile() file is complete: flushes
        // every output to disk (fsync) and returns what publish() will
        // publish, its final paths and sizes, for the report.
        RootOutputResult sync();
        // After sync(): writes the report `reportContents` as `reportName` in
        // the private directory and flushes it; links the outputs into outDir
        // (raw, histograms, then the addFile() files) and flushes outDir;
        // links the report last and flushes outDir again. A report in outDir
        // therefore means its whole set is there and on disk, even after a
        // kill -9 or a power loss. Never over an existing file: on a failure
        // the files linked so far are taken back and nothing stays published.
        // Test only: FLIRT_TEST_KILL_AFTER_LINKS=<n> makes the process kill
        // itself (SIGKILL) after its n-th link (0: before the first).
        void publish(const std::string &reportName, const std::string &reportContents);
        const RootTimings &timings() const;

    private:
        struct Private;
        std::unique_ptr<Private> d;
};

// The experiment classes the client generates (user_objects*.mustache) for a
// stream description. FLIRT only uses them in memory; exposed for tests.
// Data sources to omit must already be removed from the description.
struct ExperimentCode
{
    std::string structName;         // ExperimentName
    std::string headerFilename;     // <ExperimentName>_mvme.h
    std::string header, impl, linkDef;
};

ExperimentCode render_experiment_code(const mvme::event_server::StreamInfo &streamInfo);

std::string root_version();

}

#endif
