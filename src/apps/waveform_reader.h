#ifndef FLIRT_APPS_WAVEFORM_READER_H
#define FLIRT_APPS_WAVEFORM_READER_H

// flirt-waveforms's reading of a <runId>_waveforms.root (docs/design.md,
// "TUI", "Waveform view"; README "Waveforms" for the layout). One
// request in, one answer out, both JSON; every request names its file, and
// the indexes of the open file are kept. A frame is a run of consecutive
// entries with the same `event`: the writer fills one module's traces of one
// event together, in stream order (src/flirt/waveform_output.cc).
//
// A frame request's filter: all channels, one, or a set;
// its frames are those holding a trace of at least one of the filter's
// channels. `depth` asks for the frame at `position` and up to depth - 1
// frames before it in that list (the last N events, as mvme's waveform sink
// keeps one trace per channel per event, a2.cc waveform_sink_step); each
// trace carries its `age` (0 for the frame at position), oldest first.
//
// The tree list also gives each tree's typical y range
// and trace length, from its first 20,000 traces (README "Events for
// programs"); flirt-tui rounds the range.

#include <cstdint>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <sys/types.h>
#include <vector>

class TFile;
class TTree;

namespace flirt
{

class WaveformFileReader
{
    public:
        using json = nlohmann::json;

        WaveformFileReader();
        ~WaveformFileReader();
        WaveformFileReader(const WaveformFileReader &) = delete;
        WaveformFileReader &operator=(const WaveformFileReader &) = delete;

        // The answer to one request: {"id", ...} or {"id", "error"}.
        json handle(const json &request);

    private:
        struct TreeIndex
        {
            TTree *tree = nullptr;                  // owned by the file
            std::vector<int64_t> first;             // per frame: its first entry,
            std::vector<uint32_t> count;            // its number of entries,
            std::vector<int64_t> event;             // its event
            std::map<int, std::vector<uint32_t>> channelFrames;    // channel -> the frames it has traces in
            std::vector<int> setChannels;           // the last channel set asked for (sorted, unique),
            std::vector<uint32_t> setFrames;        // the sorted union of its channelFrames
            int maxSamples = 0;                     // the nsamples leaf's maximum
        };

        json trees(const json &request);
        json typical(TTree *tree, const std::string &name);
        json frame(const json &request);
        // Opens the file unless it is the open one, unchanged; throws the
        // sentence for the answer.
        void open(const std::string &path);
        void close();
        TTree *waveform_tree(const std::string &name);
        TreeIndex &index(const std::string &name);

        std::string path_;
        int fd_ = -1;
        dev_t dev_ = 0;
        ino_t ino_ = 0;
        off_t size_ = 0;
        int64_t mtimeNs_ = 0;
        std::unique_ptr<TFile> file_;
        std::map<std::string, TreeIndex> indexes_;
};

// ROOT's messages since the last call, one per line (SetErrorHandler).
std::string take_root_messages();
// Installs the handler that collects them instead of printing them.
void collect_root_messages();

}

#endif
