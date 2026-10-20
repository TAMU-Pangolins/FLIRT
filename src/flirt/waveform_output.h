#ifndef FLIRT_WAVEFORM_OUTPUT_H
#define FLIRT_WAVEFORM_OUTPUT_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "flirt/analysis_config.h"
#include "flirt/message_batch.h"
#include "flirt/vme_config.h"

namespace flirt
{

// One tree of <runId>_waveforms.root: a module of AnalysisConfig::waveformModules.
struct WaveformTreeSpec
{
    int eventIndex = -1;
    int moduleIndex = -1;       // VME config module index
    std::string name;           // the module's variablified name, as in physics_event
    std::string title;
    std::string module;         // the module's name
    std::string moduleType;     // the module's type name
};

// Per event, per module of AnalysisConfig::waveformModules, in module order.
// Refuses (unsupported_input) two modules whose trees would have the same name.
std::vector<WaveformTreeSpec> waveform_trees(const VMEConfig &vmeConfig, const AnalysisConfig &analysis);

// The traces of one module in one event, with the entry number of the event's
// EventData message in its VME event's tree in <runId>_raw.root.
struct WaveformEvent
{
    int eventIndex = -1;
    int moduleIndex = -1;
    int64_t entry = -1;
    mesytec::mvme::mdpp_sampling::DecodedMdppSampleEvent decoded;
};

struct WaveformBatch
{
    std::vector<WaveformEvent> events;

    void clear() { events.clear(); }
};

struct WaveformTreeStats
{
    uint64_t traces = 0;
    uint64_t samples = 0;
    uint64_t tracesOtherLength = 0;  // nsamples != 2 x the trace header's length
    std::vector<uint64_t> tracesPerLength; // index nsamples, for the waveform config check
};

// Writes <runId>_waveforms.root: one tree per WaveformTreeSpec, one entry per
// channel trace. The file and its trees are created by the constructor; the
// trees are filled in a thread of their own behind a FIFO of `capacity`
// batches. That thread touches only this file and its trees. Requires
// ROOT::EnableThreadSafety() when other threads use ROOT.
class WaveformWriter
{
    public:
        // path must be absolute: another thread may change the working directory.
        // compression: as TFile takes it, the raw file's (RootOutputOptions).
        WaveformWriter(const std::string &path, const std::vector<WaveformTreeSpec> &trees,
                       int compression = 101, size_t capacity = 16);
        // Stops the thread; the file is left incomplete.
        ~WaveformWriter();

        WaveformWriter(const WaveformWriter &) = delete;
        WaveformWriter &operator=(const WaveformWriter &) = delete;

        // Producer: queues the batch, leaving it empty. Blocks while the FIFO
        // is full. Rethrows the writer thread's exception.
        void push(WaveformBatch &batch);

        // After the last push(): waits for the thread, then writes and closes
        // the file. Rethrows the writer thread's exception.
        void finish();

        const std::vector<WaveformTreeSpec> &trees() const;
        const std::vector<WaveformTreeStats> &stats() const;
        double fillSeconds() const;         // in the writer thread
        double writeCloseSeconds() const;   // in finish()

    private:
        struct Private;
        std::unique_ptr<Private> d;
};

}

#endif
