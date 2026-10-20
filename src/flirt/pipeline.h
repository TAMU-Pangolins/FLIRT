#ifndef FLIRT_PIPELINE_H
#define FLIRT_PIPELINE_H

#include <functional>
#include <mesytec-mvlc/mvlc_readout_parser.h>

#include "flirt/extract.h"
#include "flirt/library_log.h"
#include "flirt/listfile.h"
#include "flirt/listfile_scan.h"
#include "flirt/message_batch.h"
#include "multi_event_splitter.h"

namespace flirt
{

struct PipelineResult
{
    size_t buffersRead = 0;
    size_t bytesRead = 0;
    size_t eventsProcessed = 0;   // parsed readout events handed to the analysis
    size_t messagesEmitted = 0;   // EventData messages produced
    size_t parserExceptions = 0;  // buffers whose parsing threw (mvme skips them too)
    size_t buffersWithErrors = 0; // mvme's MVMEStreamProcessorCounters::buffersWithErrors
    size_t buffersNotParsedOk = 0; // buffers for which parse_readout_buffer() returned an error
    mesytec::mvlc::readout_parser::ReadoutParserCounters parserCounters;
    bool multiEventSplitting = false;
    mesytec::mvme::multi_event_splitter::Counters splitterCounters;
    // Messages of mvme's MDPP sample decoder (waveform sources), counted
    // instead of printed; mvme only writes them to its process stderr.
    size_t waveformDecoderMessages = 0;
    // mesytec-mvlc's log messages during the replay (the readout parser's
    // warnings, library_log.h): counted and the first 100 kept, not printed.
    LibraryMessages parserWarnings;
    // Wall time of the producer: replay, parse, extract (waveform decoding
    // included), i.e. run_pipeline() minus the time spent in the sink.
    double producerSeconds = 0;
};

// Receives the message batches in stream order (message_batch.h). The sink
// may take the batch's messages (e.g. by swapping); the batch is reused after.
using BatchSink = std::function<void (MessageBatch &batch)>;

// Producer: replays the listfile through the readout parser and the
// extractor. Hands the sink BeginRun, then the EventData messages of each
// readout buffer, then EndRun. Uses no ROOT and no gRandom.
PipelineResult run_pipeline(const ListfileInfo &listfile, const VMEConfig &vmeConfig,
                            const AnalysisConfig &analysis, const RunInfo &runInfo,
                            const BatchSink &sink);

}

#endif
