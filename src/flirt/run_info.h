#ifndef FLIRT_RUN_INFO_H
#define FLIRT_RUN_INFO_H

// What the listfile records about the run itself, as flirt-convert's
// report["run"] shows it: run times, counts, packet loss, the rate over time,
// messages.log's facts and the settings changes.

#include <mesytec-mvlc/mvlc_constants.h>
#include <mesytec-mvlc/mvlc_readout_parser.h>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

#include "flirt/listfile.h"
#include "flirt/listfile_scan.h"
#include "flirt/messages_log.h"
#include "flirt/run_summary.h"
#include "flirt/run_timeline.h"
#include "flirt/settings_changes.h"
#include "flirt/vme_config.h"

namespace flirt
{

struct RunFacts
{
    RunTimes times;
    RateAnalysis rate;
    mesytec::mvlc::readout_parser::ReadoutParserCounters counters;
    uint64_t events = 0;            // readout events, the sum of eventHits
    uint64_t lossBursts = 0;
    mesytec::mvlc::ConnectionType connection = mesytec::mvlc::ConnectionType::ETH;
    MessagesLogFacts log;           // unknowns if messages.log is absent or damaged
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

// About how many events the lost packets took: lost x events / received
// + bursts, rounded (docs/design.md, "Run information").
uint64_t events_lost_estimate(uint64_t lost, uint64_t received, uint64_t events, uint64_t bursts);

// "0.0011 %": two significant digits, never in exponent form; "< 0.0001 %"
// below that.
std::string share_text(uint64_t part, uint64_t whole);

// The loss line's text after "lost packets: ", e.g. "12 of 1073420
// (0.0011 %), about 18 events, in 5 bursts; in 4 of 63 minutes, most in
// 14:31 (3), 14:52 (3), 15:03 (3)". Empty if no packet was lost.
std::string loss_text(const RunFacts &facts);

// Bins with lost packets / with live seconds, for loss_text and the report.
size_t minutes_with_loss(const RateAnalysis &rate);

// flirt-convert's lost row, the same facts in other words: "12 of 1,073,420
// packets (0.0011 %) in 5 bursts, about 18 events; in 4 of 63 minutes,
// most at 14:31, 14:52 and 15:03 (3 each)". Empty if no packet was lost.
std::string loss_block_text(const RunFacts &facts);

// The receive-buffer note, when packets were lost and messages.log's
// receive buffer is below the 10 MiB mesytec-mvlc asks for; else empty.
std::string receive_buffer_note(const RunFacts &facts);

// The same note as flirt-convert's run block words it, without "note: ".
std::string receive_buffer_text(const RunFacts &facts);

// A report's "run" (run_json's object) back into the facts the texts above
// need: times, events, packets, bursts, the rate, messages.log's facts and
// the settings changes (counters: only the ETH packets). Missing fields stay
// unknown; flirt-convert's batch reads its children's reports with it.
RunFacts run_facts_from_json(const nlohmann::json &run);

}

#endif
