#ifndef FLIRT_SAMPLING_CONFIG_H
#define FLIRT_SAMPLING_CONFIG_H

// Waveform config check (docs/design.md, "Waveform config check"): the MDPP
// sampling setting the listfile's VME config programs, compared with the
// analysis's MDPP sample decoder sources and the traces decoded. Warnings
// only; nothing here changes what is extracted.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "flirt/analysis_config.h"
#include "flirt/vme_config.h"

namespace flirt
{

// MDPP registers (templates/mdpp16_scp/vme/init-03-Streaming and Sampling.vmescript).
constexpr uint32_t MdppOutputFormatRegister = 0x6044;  // bit 4: sampling
constexpr uint32_t MdppNumberSamplesRegister = 0x6148; // total samples per trace

// One write to a watched register found in a module's init scripts.
struct RegisterWrite
{
    uint32_t address = 0;
    std::optional<uint32_t> value;  // empty: could not be evaluated
    std::string script;
    int line = 0;
    std::string reason;             // why the value is unknown
};

// Evaluates the module's init scripts in mvme's order and variable scope, for
// the subset of vme_script that can write a register: `set`,
// `write <amod> <dwidth> <addr> <value>` and `<addr> <value>`. Every other
// command, `writeabs` and block contents are ignored; addresses match exactly
// (no base address). Returns the writes to the watched addresses in order. A
// write whose address cannot be evaluated counts as an unknown write to every
// watched address.
std::vector<RegisterWrite> evaluate_register_writes(const VMEConfig &vmeConfig, size_t eventIndex,
                                                    size_t moduleIndex,
                                                    const std::vector<uint32_t> &watched);

// The sampling setting of one module.
struct SamplingSetting
{
    int eventIndex = -1;
    int moduleIndex = -1;
    std::string module;
    std::string moduleType;
    std::optional<bool> sampling;           // empty: unknown
    std::string samplingFrom;               // where it comes from, or why it is unknown
    std::optional<uint32_t> numberSamples;  // empty: none, several or unknown
    std::string numberSamplesFrom;
};

// Enabled modules of type mdpp*, plus every module read by an MDPP sample
// decoder source, in event and module order.
std::vector<SamplingSetting> sampling_settings(const VMEConfig &vmeConfig, const AnalysisConfig &analysis);

// The first MDPP sample decoder source reading the module (a2 order), for the
// report's decoderSource; null if none.
const DataSourceConfig *mdpp_sample_decoder(const AnalysisConfig &analysis, int eventIndex, int moduleIndex);

// Why the module gets no tree in the waveforms file; empty if it gets one
// (docs/design.md, "Waveforms with any analysis"). It gets one if it is
// enabled, mvme's decoder supports its type (get_decoder_function,
// mdpp_decode.cc:352-384), its sampling is on or unknown, and its event has a
// data source in the analysis (without one mvme sends no EventData message,
// event_server.cc:500-507). The analysis's decoders play no part.
std::string why_no_waveforms(const VMEConfig &vmeConfig, const AnalysisConfig &analysis,
                             const SamplingSetting &setting);

// AnalysisConfig::waveformModules: per event, the modules with a tree, in
// module order. Every module mvme's decoder supports is an mdpp*, so the
// settings cover every candidate.
std::vector<std::vector<int>> waveform_tree_modules(const VMEConfig &vmeConfig, const AnalysisConfig &analysis,
                                                    const std::vector<SamplingSetting> &settings);

// True if the module has a tree (AnalysisConfig::waveformModules).
bool is_waveform_module(const AnalysisConfig &analysis, int eventIndex, int moduleIndex);

// The analysis MDPP sample decoder whose decoded event gives a tree module's
// traces: the first one (a2 order) that reads the module and whose
// moduleTypeName is the module's type, i.e. the same decoder function on the
// same data. Null if none: FLIRT then decodes the module itself.
const DataSourceConfig *reused_decoder(const VMEConfig &vmeConfig, const AnalysisConfig &analysis,
                                       int eventIndex, int moduleIndex);

// What the check knows about a module's waveforms.
struct ModuleWaveforms
{
    bool tree = false;          // the module has a tree (is_waveform_module)
    std::string whyNoTree;      // no tree: why_no_waveforms
    std::string analysisSource; // the analysis's first MDPP sample decoder source; empty: none
};

// Traces decoded for one module: count, and count per number of samples.
struct ModuleTraceCounts
{
    uint64_t traces = 0;
    std::vector<uint64_t> tracesPerLength;
};

struct WaveformConfigResult
{
    std::string result;     // "pass", "warn", "note" or "skipped"
    std::string message;
    std::optional<uint64_t> traces;
    std::optional<uint64_t> tracesWithConfiguredLength;
};

// Compares one module's setting with its waveforms and its traces (null: not
// decoded): a tree module's traces, else those an analysis decoder decoded.
WaveformConfigResult compare_waveform_config(const SamplingSetting &setting, const ModuleWaveforms &waveforms,
                                             const ModuleTraceCounts *traces, bool waveformsRequested);

}

#endif
