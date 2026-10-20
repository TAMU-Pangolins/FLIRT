#include "flirt/pipeline.h"

#include "flirt/library_log.h"

#include <algorithm>
#include <chrono>
#include <mesytec-mvlc/mvlc_command_builders.h>
#include <mesytec-mvlc/mvlc_listfile_zip.h>
#include <mesytec-mvlc/mvlc_readout_config.h>
#include <mesytec-mvlc/mvlc_replay_worker.h>
#include <mesytec-mvlc/readout_buffer_queues.h>
#include <mesytec-mvlc/util/logging.h>

namespace mvlc = mesytec::mvlc;
namespace parser = mesytec::mvlc::readout_parser;
namespace es = mvme::event_server;
namespace splitter = mesytec::mvme::multi_event_splitter;

namespace flirt
{

namespace
{

// collect_multi_event_splitter_filters + multi_event_splitter_filters_to_strings
// (analysis_util.cc:611-665): per event, per enabled module, the module's
// header filters if splitting is enabled for the event, else none.
std::vector<std::vector<std::vector<std::string>>> splitter_filter_strings(
    const VMEConfig &vmeConfig, const AnalysisConfig &analysis)
{
    std::vector<std::vector<std::vector<std::string>>> result;

    for (size_t ei = 0; ei < vmeConfig.events.size(); ++ei)
    {
        std::vector<std::vector<std::string>> eventFilters;

        for (const auto &module: vmeConfig.events[ei].modules)
        {
            if (!module.enabled)
                continue;

            std::vector<std::string> moduleFilters;

            if (analysis.multiEventProcessing.at(ei))
                for (const auto &filter: module.eventHeaderFilters)
                    moduleFilters.emplace_back(filter.constData());

            eventFilters.emplace_back(moduleFilters);
        }

        result.emplace_back(eventFilters);
    }

    return result;
}

// mvme_context.cc:81-82
constexpr size_t ReadoutBufferCount = 4;
constexpr size_t ReadoutBufferSize = 1024 * 1024;

}

PipelineResult run_pipeline(const ListfileInfo &listfile, const VMEConfig &vmeConfig,
                            const AnalysisConfig &analysis, const RunInfo &runInfo,
                            const BatchSink &sink)
{
    const auto tStart = std::chrono::steady_clock::now();
    PipelineResult result;
    Extractor extractor(vmeConfig, analysis);

    // The readout structure comes from the recorded crate config, checked
    // against the VME config (listfile_scan.h).
    auto parserState = make_listfile_parser(listfile, vmeConfig);

    // The MDPP sample decoder logs through mvlc::get_logger("mdpp_sam_decode")
    // (mdpp_decode.cc:117-124): its messages are counted, not printed. The
    // readout parser's and the event builder's (library_log.h) are counted
    // and the first 100 kept for the report.
    auto decoderMessages = std::make_shared<MessageSink>();
    ScopedLoggerSinks decoderLog({ mvlc::get_logger("mdpp_sam_decode") }, decoderMessages);
    auto libraryMessages = std::make_shared<MessageSink>(KeptLibraryMessages);
    ScopedLoggerSinks libraryLog(replay_loggers(), libraryMessages);

    // Hands a batch to the consumer; the time spent there is not the producer's.
    MessageBatch batch;
    std::chrono::steady_clock::duration sinkTime{};

    auto deliver = [&]()
    {
        auto t0 = std::chrono::steady_clock::now();
        sink(batch);
        sinkTime += std::chrono::steady_clock::now() - t0;
        batch.clear();
    };

    batch.next() = extractor.beginRunMessage(runInfo);
    batch.commit();
    deliver();

    // Last part of the event data chain: the analysis (eventData_analysis,
    // mvlc_stream_worker.cc:177-274). The messages of a readout buffer are
    // collected and delivered once the buffer is parsed.
    auto analysisEventData = [&](void *, int /*crateIndex*/, int eventIndex,
                                 const parser::ModuleData *moduleDataList, unsigned moduleCount)
    {
        ++result.eventsProcessed;

        auto &msg = batch.next();
        if (extractor.processEvent(eventIndex, moduleDataList, moduleCount, msg))
        {
            batch.commit();
            ++result.messagesEmitted;
        }
    };

    // Optional multi-event splitter between parser and analysis
    // (mvlc_stream_worker.cc:314-329, 443-495).
    parser::ReadoutParserCallbacks callbacks;
    splitter::State splitterState;
    splitter::Callbacks splitterCallbacks;
    result.multiEventSplitting = std::any_of(analysis.multiEventProcessing.begin(),
                                             analysis.multiEventProcessing.end(),
                                             [](bool b) { return b; });

    if (result.multiEventSplitting)
    {
        std::error_code ec;
        std::tie(splitterState, ec) = splitter::make_splitter(splitter_filter_strings(vmeConfig, analysis));

        if (ec)
            throw std::runtime_error("multi_event_splitter: " + ec.message());

        splitterCallbacks.eventData = analysisEventData;
        callbacks.eventData = [&](void *userContext, int /*crateIndex*/, int ei,
                                  const parser::ModuleData *moduleDataList, unsigned moduleCount)
        {
            splitter::event_data(splitterState, splitterCallbacks, userContext, ei,
                                 moduleDataList, moduleCount);
        };
    }
    else
        callbacks.eventData = [&](void *userContext, int crateIndex, int ei,
                                  const parser::ModuleData *moduleDataList, unsigned moduleCount)
        {
            analysisEventData(userContext, crateIndex, ei, moduleDataList, moduleCount);
        };

    // Timeticks and other system events only feed analysis operators and
    // counters, never the data sources (mvlc_stream_worker.cc:277-312), so
    // they never reach the analysis here either.
    callbacks.systemEvent = [](void *, int, const u32 *, u32) {};

    // Replay exactly as MVLCListfileWorker does (mvlc_listfile_worker.cc:230-259),
    // through a handle that checks what it delivers (I2).
    mvlc::listfile::ZipReader zip;
    zip.openArchive(listfile.archivePath);
    CheckedReadHandle readHandle(zip, listfile.listfileEntry);

    mvlc::ReadoutBufferQueues queues(ReadoutBufferSize, ReadoutBufferCount);
    mvlc::ReplayWorker replay(queues, &readHandle);

    if (auto ec = replay.start().get())
        throw std::runtime_error("replay start failed: " + ec.message());

    auto process = [&](mvlc::ReadoutBuffer *buffer)
    {
        auto view = buffer->viewU32();
        bool processingOk = false;
        splitterState.processingFlags = {};

        // MVLC_StreamWorker::processBuffer (mvlc_stream_worker.cc:904-950):
        // exceptions escaping the parser are logged and the next buffer is
        // processed; the buffer counts as one with errors.
        try
        {
            auto pr = parser::parse_readout_buffer(
                static_cast<mvlc::ConnectionType>(buffer->type()), parserState, callbacks,
                result.parserCounters, buffer->bufferNumber(), view.data(), view.size());

            if (pr != parser::ParseResult::Ok)
                ++result.buffersNotParsedOk;

            processingOk = (pr == parser::ParseResult::Ok && !splitterState.processingFlags);
        }
        catch (const std::exception &)
        {
            ++result.parserExceptions;
        }

        if (!processingOk)
            ++result.buffersWithErrors;

        queues.emptyBufferQueue().enqueue(buffer);

        if (batch.size)
            deliver();
    };

    try
    {
        // MVLC_StreamWorker::start loop with StopWhenQueueEmpty (mvlc_stream_worker.cc:626-714).
        while (true)
        {
            if (auto buffer = queues.filledBufferQueue().dequeue(std::chrono::milliseconds(100)))
            {
                process(buffer);
                continue;
            }

            if (replay.state() == mvlc::ReplayWorker::State::Idle)
            {
                while (auto buffer = queues.filledBufferQueue().dequeue())
                    process(buffer);
                break;
            }
        }
    }
    catch (...)
    {
        replay.stop(); // lets the replay thread end, so it can be joined
        throw;
    }

    // Idle: the replay thread no longer reads, so readHandle's check is final.
    auto counters = replay.counters();
    result.buffersRead = counters.buffersRead;
    result.bytesRead = counters.bytesRead;
    result.listfileEntry = readHandle.check();

    // A read error ends the replay early. mvme does not look at it and exports
    // what was read (mvlc_listfile_worker.cc:249-292); FLIRT records it (I2).
    if (counters.eptr && result.listfileEntry.readError.empty())
        std::rethrow_exception(counters.eptr);
    if (counters.ec)
        throw std::runtime_error("replay error: " + counters.ec.message());

    batch.next() = extractor.endRunMessage();
    batch.commit();
    deliver();

    result.splitterCounters = splitterState.counters;
    result.waveformDecoderMessages = decoderMessages->result().count;
    result.parserWarnings = libraryMessages->result();

    result.producerSeconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - tStart - sinkTime).count();

    return result;
}

}
