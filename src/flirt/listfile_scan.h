#ifndef FLIRT_LISTFILE_SCAN_H
#define FLIRT_LISTFILE_SCAN_H

#include <mesytec-mvlc/mvlc_readout_parser.h>

#include "flirt/listfile.h"
#include "flirt/run_summary.h"
#include "flirt/run_timeline.h"
#include "flirt/vme_config.h"

namespace flirt
{

// Fed from the parser's callbacks with the parser's own running counters.
class RunRecorder
{
    public:
        // The parser's systemEvent callback (header and contents, size
        // counting both, mvlc_readout_parser.cc:436-440). The five timestamped
        // sections carry one u64 of Unix seconds as two u32 words, low word
        // first (listfile_write_timestamp_section, mvlc_listfile.cc:133-143).
        void systemEvent(const uint32_t *header, uint32_t size,
                         const mesytec::mvlc::readout_parser::ReadoutParserCounters &counters);

        // The parser's eventData callback, once per readout event, before any
        // multi-event splitting: a loss burst is each growth of ethPacketLoss
        // (counted at mvlc_readout_parser.cc:1258-1270) since the previous event.
        void eventData(const mesytec::mvlc::readout_parser::ReadoutParserCounters &counters)
        {
            if (counters.ethPacketLoss != lastLoss_)
            {
                ++lossBursts;
                lastLoss_ = counters.ethPacketLoss;
            }
        }

        // After the last buffer: loss after the last event, and the timeline's totals.
        void finish(const mesytec::mvlc::readout_parser::ReadoutParserCounters &counters);

        RunTimeAccumulator runTimes;
        RunTimeline timeline;
        uint64_t lossBursts = 0;

    private:
        uint32_t lastLoss_ = 0;
};

// Sum of the parser's eventHits: the readout events so far.
uint64_t readout_events(const mesytec::mvlc::readout_parser::ReadoutParserCounters &counters);

// The readout parser for the recorded crate config, after mvme's
// sanitize_readout_stacks, checked against the VME config; throws
// unsupported_input on a mismatch (pipeline.cc, mvlc_stream_worker.cc:549-562).
mesytec::mvlc::readout_parser::ReadoutParserState make_listfile_parser(const ListfileInfo &listfile,
                                                                        const VMEConfig &vmeConfig);

}

#endif
