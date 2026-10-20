#ifndef FLIRT_PIPELINE_H
#define FLIRT_PIPELINE_H

#include <functional>
#include <mesytec-mvlc/mvlc_readout_parser.h>

#include "flirt/extract.h"
#include "flirt/integrity.h"
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
    // The listfile entry as the replay read it (check I2). A read error ends
    // the replay early, as in mvme, and is recorded here instead of thrown.
    EntryCheck listfileEntry;
    // Wall time of the producer: replay, parse, extract (waveform decoding
    // included), i.e. run_pipeline() minus the time spent in the sink.
    double producerSeconds = 0;
    // BeginRun, EndRun, UnixTimetick, Pause and Resume sections as the parser
    // delivered them, the preamble's BeginRun included (run_summary.h), the
    // rate over time and the loss bursts (listfile_scan.h).
    RunRecorder run;
};

// Receives the message batches in stream order (message_batch.h). The sink
// may take the batch's messages (e.g. by swapping); the batch is reused after.
using BatchSink = std::function<void (MessageBatch &batch)>;

// Called on the producer thread after each readout buffer is processed:
// bytesProcessed is the sum of the processed buffers' sizes, bytesTotal the
// listfile entry's declared uncompressed size (0 if unknown).
using ProgressFn = std::function<void (uint64_t bytesProcessed, uint64_t bytesTotal)>;

// Producer: replays the listfile through the readout parser and the
// extractor. Hands the sink BeginRun, then the EventData messages of each
// readout buffer, then EndRun. Uses no ROOT and no gRandom.
//
// waveformThreads > 0, if the analysis has MDPP sample decoder sources or,
// with collectTraces, tree modules (AnalysisConfig::waveformModules):
// those sources and FLIRT's decoders run on that many worker threads, each
// with its own Waveforms Extractor, one job per readout buffer, while this
// thread runs every other source on every event in order (their values depend
// on their sources' sequential random streams). The results are merged and
// delivered in buffer order, one batch per buffer, as with 0. Only the
// waveform sources' a2 values (sample + random, a2.cc:892) can differ from the
// sequential path; the event server sends only their channel-0 samples
// (event_server.cc:537) and floors them (:567-570) to the integer samples, so
// the messages are the same on both paths.
//
// collectTraces: every EventData message carries its waveform traces in the
// batch (MessageBatch::traces), on both paths. The traces are the decoder's
// output, before any random is added, so they do not depend on the path.
//
// progress: see ProgressFn; empty for none.
PipelineResult run_pipeline(const ListfileInfo &listfile, const VMEConfig &vmeConfig,
                            const AnalysisConfig &analysis, const RunInfo &runInfo,
                            const BatchSink &sink, unsigned waveformThreads = 0,
                            bool collectTraces = false, const ProgressFn &progress = {});

// True if the analysis has MDPP sample decoder (waveform) sources.
bool has_waveform_sources(const AnalysisConfig &analysis);

// True if a module has a tree in the waveforms file (AnalysisConfig::waveformModules).
bool has_waveform_modules(const AnalysisConfig &analysis);

}

#endif
