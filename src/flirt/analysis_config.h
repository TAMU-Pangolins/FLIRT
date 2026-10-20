#ifndef FLIRT_ANALYSIS_CONFIG_H
#define FLIRT_ANALYSIS_CONFIG_H

#include <QString>
#include <QStringList>
#include <QUuid>
#include <string>
#include <vector>

#include "analysis/a2/a2.h"
#include "flirt/vme_config.h"

namespace flirt
{

// One analysis data source that feeds the event-server stream, after the same
// filtering and ordering the a2 adapter applies (a2_adapter.cc:1142-1320, 1623-1641).
struct DataSourceConfig
{
    enum class Kind { Extractor, ListFilterExtractor, MdppSampleDecoder };

    Kind kind;
    QString className;        // "analysis::Extractor", ...
    QString name;             // objectName(), before variablify
    int eventIndex = -1;
    int moduleIndex = -1;     // index into EventConfig::modules
    u64 rngSeed = 0;
    a2::DataSourceOptions::opt_t options = 0;
    QStringList parameterNames;
    // get_data_storage_bits (event_server_util.cc:43-58): the extractors' data
    // bits, 0 for every other data source.
    u32 dataBits = 0;

    // Kind::Extractor
    a2::data_filter::MultiWordFilter filter = {};
    u32 requiredCompletionCount = 0;

    // Kind::ListFilterExtractor
    a2::data_filter::ListFilter listFilter = {};
    u8 repetitions = 0;

    // Kind::MdppSampleDecoder (waveforms)
    std::string moduleTypeName;
    unsigned maxChannels = 0;
    unsigned maxSamples = 0;
};

struct AnalysisConfig
{
    int version = 0;
    // Indexed by event index; each list in a2 order (stable sort by module index).
    std::vector<std::vector<DataSourceConfig>> dataSources;
    // Indexed by event index: MultiEventProcessing as mvme reads it
    // (uses_multi_event_splitting, analysis_util.cc:576-591).
    std::vector<bool> multiEventProcessing;
    // Not part of the analysis: indexed by event index, the VME config module
    // indexes (in module order) that get a tree in <runId>_waveforms.root
    // (waveform_tree_modules(), sampling_config.h). Their traces are the
    // decoded event of an analysis MDPP sample decoder of the module's type
    // that reads the module, else of FLIRT's own call of mvme's decoder, which
    // adds no event-server data, descriptions, dither draws or branches.
    // Empty unless waveforms are requested.
    std::vector<std::vector<int>> waveformModules;
};

// Parses an mvme .analysis document against the VME config. Refuses inputs
// FLIRT cannot reproduce exactly: unsupported versions or data-source classes,
// and event building.
AnalysisConfig parse_analysis(const std::string &json, const VMEConfig &vmeConfig);

}

#endif
