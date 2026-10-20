#ifndef FLIRT_RUN_INFO_H
#define FLIRT_RUN_INFO_H

// What the listfile records about the run itself, as flirt-convert's
// report["run"] shows it: run times, counts and the settings changes.

#include <mesytec-mvlc/mvlc_constants.h>
#include <mesytec-mvlc/mvlc_readout_parser.h>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

#include "flirt/listfile.h"
#include "flirt/listfile_scan.h"
#include "flirt/run_summary.h"
#include "flirt/settings_changes.h"
#include "flirt/vme_config.h"

namespace flirt
{

struct RunFacts
{
    RunTimes times;
    mesytec::mvlc::readout_parser::ReadoutParserCounters counters;
    uint64_t events = 0;            // readout events, the sum of eventHits
    mesytec::mvlc::ConnectionType connection = mesytec::mvlc::ConnectionType::ETH;
    SettingsChanges settings;
};

RunFacts run_facts(const ListfileInfo &listfile,
                   const mesytec::mvlc::readout_parser::ReadoutParserCounters &counters,
                   const RunRecorder &recorder);

// Settings changed through mvme during the run, from messages.log
// (settings_changes.h). An absent or damaged entry is "not checked", never a
// failure.
SettingsChanges settings_changes(const ListfileInfo &listfile);

// report["run"]; times ISO 8601 local with the offset, unknowns null.
nlohmann::json run_json(const RunFacts &facts, const VMEConfig &vmeConfig);

// A report's "run" (run_json's object) back into the facts the texts above
// need: times, events and the settings changes. Missing fields stay unknown; flirt-convert's batch
// reads its children's reports with it.
RunFacts run_facts_from_json(const nlohmann::json &run);

}

#endif
