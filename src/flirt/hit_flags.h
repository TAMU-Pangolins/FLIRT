#ifndef FLIRT_HIT_FLAGS_H
#define FLIRT_HIT_FLAGS_H

// --extract-hit-flags (docs/design.md, "Hit flags"): a flag
// source next to each MDPP amplitude or integral extractor, as mvme's manual
// describes editing an analysis for them (doc/sphinx/source/analysis.rst:536-571).
// mvme's filters match only '0' and '1' (data_filter.cc:46-55), so the pile-up
// and over/underflow bits (P, O) of a data word are never extracted otherwise.
// The edit happens on the analysis document before it is parsed, so the
// conversion follows it as it follows any analysis; the document is also
// written out, and official mvme given it produces the identical files.

#include <cstdint>
#include <string>
#include <vector>
#include <event_server/common/event_server_lib.h>

#include "flirt/analysis_config.h"
#include "flirt/vme_config.h"

namespace flirt
{

// One flag source the edit added.
struct HitFlagSource
{
    std::string name;       // "<original name>_flags"
    std::string original;   // the amplitude or integral source it copies
    std::string module, type;
    int eventIndex = -1, moduleIndex = -1;  // VME config indexes
    // True: value = 2 * pile-up + over/underflow (SCP); false: value =
    // overflow (QDC).
    bool pileUp = true;
    std::string filter;     // the flag source's filter string
};

struct HitFlagsEdit
{
    std::string analysisJson;               // the edited document, same top-level shape
    std::vector<HitFlagSource> sources;     // in the order they were appended
    // Why a module or source got no flag source: "not available for module X
    // (type Y)", "<name> exists already, not added".
    std::vector<std::string> notAdded;
};

// Appends a flag source for every analysis::Extractor with one subfilter
// whose fixed bits ('0'/'1') equal an amplitude or integral filter of its
// module type's mvme template (see hit_flags.cc). Throws unsupported_input if
// the document does not parse.
HitFlagsEdit add_hit_flag_sources(const std::string &analysisJson, const VMEConfig &vmeConfig);

// Per channel of one flag source: the floored values the event server sent.
struct HitFlagCounts
{
    uint64_t hits = 0, value1 = 0, value2 = 0, value3 = 0;
};

// Counts each flag source's values per channel from the EventData messages,
// as mvme_root_client reads them (event_server_lib.h Client). Feed every
// message of the stream in order.
class HitFlagCounter: public mvme::event_server::Client
{
    public:
        HitFlagCounter(const AnalysisConfig &analysis, const std::vector<HitFlagSource> &sources);

        // Per flag source (in `sources` order): per channel index.
        const std::vector<std::vector<HitFlagCounts>> &counts() const { return counts_; }

    protected:
        void serverInfo(const mvme::event_server::Message &, const mvme::event_server::json &) override {}
        void beginRun(const mvme::event_server::Message &, const mvme::event_server::StreamInfo &) override {}
        void eventData(const mvme::event_server::Message &msg, int eventIndex,
                       const std::vector<mvme::event_server::DataSourceContents> &contents) override;
        void endRun(const mvme::event_server::Message &, const mvme::event_server::json &) override {}
        // Never called: Client::handleMessage rethrows (event_server_lib.h:823-827).
        void error(const mvme::event_server::Message &, const std::exception &) override {}

    private:
        // Per event index: per data-source index, the flag source's index or -1.
        std::vector<std::vector<int>> slots_;
        std::vector<std::vector<HitFlagCounts>> counts_;
};

// The report's "hitFlags" object (enabled, the analysis file's name, the
// totals, per source the value meanings and the counts of the channels with
// hits, what was not added).
mvme::event_server::json hit_flags_json(const HitFlagsEdit &edit, const HitFlagCounter &counter,
                                        const std::string &analysisFileName);

// "hit flags: 1,204 pile-up, 312 over/underflow, 7 both (of 1,073,420 hits)".
std::string hit_flags_line(const mvme::event_server::json &hitFlags);

}

#endif
