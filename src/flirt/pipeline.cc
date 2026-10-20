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
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

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

// One readout buffer's work for the waveform workers: copies of the module
// data the waveform sources and the tree modules need, per parsed event, and the Written
// Extractor's partial messages, produced on the producer thread.
struct WaveformJob
{
    struct Module { size_t offset; u32 size; }; // into words; size 0: not needed
    struct Event { int eventIndex; size_t firstModule; unsigned moduleCount; };

    std::vector<u32> words;
    std::vector<Module> modules;
    std::vector<Event> events;
    MessageBatch written;    // producer: partial messages of the written sources
    MessageBatch waveforms;  // worker: partial messages of the waveform sources
    MessageBatch out;        // worker: the merged messages, delivered in order
    bool done = false;

    void reset()
    {
        words.clear();
        modules.clear();
        events.clear();
        written.clear();
        waveforms.clear();
        out.clear();
        done = false;
    }
};

// K threads, each with its own Waveforms Extractor, process jobs; finished
// jobs are delivered strictly in submission order on the producer thread.
//
// Shared state between concurrently used Extractors, checked for v1.21.0:
// - every a2 data source has its own arena memory and pcg32 (a2.h:133,144,203);
//   they share only the static RealDist01 (a2.cc:186), whose operator() reads
//   its const parameters and keeps no state (libstdc++ 13 bits/random.h,
//   uniform_real_distribution::operator()); get_operator_table()'s lazy static
//   (a2.cc:4731-4735) is only reached with operators, which FLIRT's a2 has none;
// - mdpp_decode.cc: the filters are static const objects (:112-115), the
//   decoded event is a local value; the default logger function
//   (:119-124) looks the logger up under mesytec-mvlc's mutex (logging.cc:47-56;
//   it exists already, see run_pipeline), stores its level atomically (spdlog
//   level_t is std::atomic<int>, SPDLOG_NO_ATOMIC_LEVELS is OFF) and logs to
//   MessageSink (library_log.h), whose base_sink<std::mutex> locks per message;
// - each Extractor owns its sample events (Protected, extract.cc).
class WaveformWorkers
{
    public:
        using Deliver = std::function<void (MessageBatch &batch)>;

        // The Extractors are built here, on the producer thread, before any
        // worker starts.
        WaveformWorkers(unsigned count, const VMEConfig &vmeConfig, const AnalysisConfig &analysis,
                        bool collectTraces)
            : maxInFlight_(2 * count + 2)
            , collectTraces_(collectTraces)
        {
            for (unsigned i = 0; i < count; ++i)
                extractors_.emplace_back(std::make_unique<Extractor>(vmeConfig, analysis,
                                                                     SourceSet::Waveforms));
            for (unsigned i = 0; i < count; ++i)
                threads_.emplace_back([this, i] { work(*extractors_[i]); });
            current_ = takeJob();
        }

        // Joins the workers on every path; jobs not yet started are dropped.
        ~WaveformWorkers()
        {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                quit_ = true;
            }
            todoChanged_.notify_all();
            for (auto &t: threads_)
                t.join();
        }

        // The job the producer fills for the current readout buffer.
        WaveformJob &current() { return *current_; }

        // Submits the current job if it has events. At most maxInFlight_ jobs
        // are submitted and not yet delivered; finished ones are delivered in
        // order. Rethrows a worker's exception.
        void submit(const Deliver &deliver)
        {
            if (current_->events.empty())
                return;

            deliverFinished(maxInFlight_ - 1, deliver);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                todo_.push_back(current_);
                inFlight_.push_back(current_);
            }
            todoChanged_.notify_one();
            current_ = takeJob();
            deliverFinished(maxInFlight_, deliver);
        }

        // Delivers every submitted job, in order.
        void finish(const Deliver &deliver) { deliverFinished(0, deliver); }

    private:
        // Delivers finished jobs from the front; waits while more than
        // `keep` jobs are in flight.
        void deliverFinished(size_t keep, const Deliver &deliver)
        {
            while (true)
            {
                WaveformJob *job;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    if (error_)
                        std::rethrow_exception(error_);
                    if (inFlight_.empty())
                        return;
                    if (!inFlight_.front()->done)
                    {
                        if (inFlight_.size() <= keep)
                            return;
                        jobDone_.wait(lock, [this] { return inFlight_.front()->done || error_; });
                        if (error_)
                            std::rethrow_exception(error_);
                    }
                    job = inFlight_.front();
                    inFlight_.pop_front();
                }

                if (job->out.size)
                    deliver(job->out);

                std::lock_guard<std::mutex> lock(mutex_);
                spare_.push_back(job);
            }
        }

        WaveformJob *takeJob()
        {
            std::lock_guard<std::mutex> lock(mutex_);
            WaveformJob *job;
            if (spare_.empty())
            {
                jobs_.emplace_back(std::make_unique<WaveformJob>());
                job = jobs_.back().get();
            }
            else
            {
                job = spare_.back();
                spare_.pop_back();
            }
            job->reset();
            return job;
        }

        void work(Extractor &extractor)
        {
            std::vector<parser::ModuleData> moduleData;

            while (true)
            {
                WaveformJob *job;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    todoChanged_.wait(lock, [this] { return quit_ || !todo_.empty(); });
                    if (quit_)
                        return;
                    job = todo_.front();
                    todo_.pop_front();
                }

                try
                {
                    process(extractor, *job, moduleData, collectTraces_);
                }
                catch (...)
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (!error_)
                        error_ = std::current_exception();
                }

                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    job->done = true;
                }
                jobDone_.notify_all();
            }
        }

        static void process(Extractor &extractor, WaveformJob &job,
                            std::vector<parser::ModuleData> &moduleData, bool collectTraces)
        {
            for (const auto &event: job.events)
            {
                moduleData.assign(event.moduleCount, parser::ModuleData{});
                for (unsigned mi = 0; mi < event.moduleCount; ++mi)
                {
                    const auto &module = job.modules[event.firstModule + mi];
                    moduleData[mi].data.data = job.words.data() + module.offset;
                    moduleData[mi].data.size = module.size;
                }

                auto &msg = job.waveforms.next();
                if (extractor.processEvent(event.eventIndex, moduleData.data(), event.moduleCount, msg,
                                           collectTraces ? &job.waveforms.nextTraces() : nullptr))
                    job.waveforms.commit();
            }

            if (job.waveforms.size != job.written.size)
                throw std::logic_error("waveform workers: " + std::to_string(job.waveforms.size)
                                       + " partial messages for " + std::to_string(job.written.size)
                                       + " written ones");

            // The traces travel with the merged message.
            for (size_t i = 0; i < job.written.size; ++i)
            {
                extractor.merge(job.written.messages[i], job.waveforms.messages[i], job.out.next());
                std::swap(job.out.nextTraces(), job.waveforms.traces[i]);
                job.out.commit();
            }
        }

        const size_t maxInFlight_;
        const bool collectTraces_;
        std::vector<std::unique_ptr<Extractor>> extractors_;
        std::vector<std::unique_ptr<WaveformJob>> jobs_;
        std::mutex mutex_;
        std::condition_variable todoChanged_, jobDone_;
        std::deque<WaveformJob *> todo_, inFlight_;
        std::vector<WaveformJob *> spare_;
        WaveformJob *current_ = nullptr;
        std::exception_ptr error_;
        bool quit_ = false;
        std::vector<std::thread> threads_;
};

// Per event, per parser module index: whether a waveform source reads it or
// it has a tree in the waveforms file.
std::vector<std::vector<bool>> waveform_modules(const VMEConfig &vmeConfig,
                                                const AnalysisConfig &analysis)
{
    std::vector<std::vector<bool>> result;

    for (size_t ei = 0; ei < vmeConfig.events.size(); ++ei)
    {
        const auto indexMap = make_module_index_map(vmeConfig.events[ei]);
        std::vector<bool> needed(indexMap.size(), false);

        for (size_t pmi = 0; pmi < indexMap.size(); ++pmi)
            if (ei < analysis.dataSources.size())
                for (const auto &src: analysis.dataSources[ei])
                    if (src.kind == DataSourceConfig::Kind::MdppSampleDecoder
                        && src.moduleIndex == indexMap[pmi])
                        needed[pmi] = true;

        for (size_t pmi = 0; pmi < indexMap.size(); ++pmi)
            if (ei < analysis.waveformModules.size())
                for (int mi: analysis.waveformModules[ei])
                    if (mi == indexMap[pmi])
                        needed[pmi] = true;

        result.push_back(needed);
    }

    return result;
}

// mvme_context.cc:81-82
constexpr size_t ReadoutBufferCount = 4;
constexpr size_t ReadoutBufferSize = 1024 * 1024;

}

bool has_waveform_sources(const AnalysisConfig &analysis)
{
    for (const auto &sources: analysis.dataSources)
        for (const auto &src: sources)
            if (src.kind == DataSourceConfig::Kind::MdppSampleDecoder)
                return true;
    return false;
}

bool has_waveform_modules(const AnalysisConfig &analysis)
{
    return std::any_of(analysis.waveformModules.begin(), analysis.waveformModules.end(),
                       [](const std::vector<int> &modules) { return !modules.empty(); });
}

PipelineResult run_pipeline(const ListfileInfo &listfile, const VMEConfig &vmeConfig,
                            const AnalysisConfig &analysis, const RunInfo &runInfo,
                            const BatchSink &sink, unsigned waveformThreads, bool collectTraces,
                            const ProgressFn &progress)
{
    const auto tStart = std::chrono::steady_clock::now();
    PipelineResult result;
    // FLIRT's own decoders only run when traces are collected (extract.h).
    const bool parallelWaveforms = waveformThreads > 0
        && (has_waveform_sources(analysis) || (collectTraces && has_waveform_modules(analysis)));
    Extractor extractor(vmeConfig, analysis, parallelWaveforms ? SourceSet::Written : SourceSet::All);

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

    // Parallel waveform decoding: started once the decoder logger counts.
    std::unique_ptr<WaveformWorkers> workers;
    std::vector<std::vector<bool>> waveformModules;
    WaveformWorkers::Deliver deliverJob;

    if (parallelWaveforms)
    {
        waveformModules = waveform_modules(vmeConfig, analysis);
        workers = std::make_unique<WaveformWorkers>(waveformThreads, vmeConfig, analysis, collectTraces);
        deliverJob = [&](MessageBatch &merged)
        {
            auto t0 = std::chrono::steady_clock::now();
            sink(merged);
            sinkTime += std::chrono::steady_clock::now() - t0;
        };
    }

    // Last part of the event data chain: the analysis (eventData_analysis,
    // mvlc_stream_worker.cc:177-274). The messages of a readout buffer are
    // collected and delivered once the buffer is parsed.
    auto analysisEventData = [&](void *, int /*crateIndex*/, int eventIndex,
                                 const parser::ModuleData *moduleDataList, unsigned moduleCount)
    {
        ++result.eventsProcessed;

        if (workers)
        {
            // The written sources here, in order; the module data the waveform
            // sources need is copied for a worker (the buffer is reused).
            auto &job = workers->current();
            job.events.push_back({ eventIndex, job.modules.size(), moduleCount });

            for (unsigned pmi = 0; pmi < moduleCount; ++pmi)
            {
                const auto &md = moduleDataList[pmi].data;
                const bool needed = eventIndex >= 0 && static_cast<size_t>(eventIndex) < waveformModules.size()
                    && pmi < waveformModules[eventIndex].size() && waveformModules[eventIndex][pmi];
                job.modules.push_back({ job.words.size(), needed ? md.size : 0u });
                if (needed)
                    job.words.insert(job.words.end(), md.data, md.data + md.size);
            }

            if (extractor.processEvent(eventIndex, moduleDataList, moduleCount, job.written.next()))
            {
                job.written.commit();
                ++result.messagesEmitted;
            }
            return;
        }

        auto &msg = batch.next();
        if (extractor.processEvent(eventIndex, moduleDataList, moduleCount, msg,
                                   collectTraces ? &batch.nextTraces() : nullptr))
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
            result.run.eventData(result.parserCounters);
            splitter::event_data(splitterState, splitterCallbacks, userContext, ei,
                                 moduleDataList, moduleCount);
        };
    }
    else
        callbacks.eventData = [&](void *userContext, int crateIndex, int ei,
                                  const parser::ModuleData *moduleDataList, unsigned moduleCount)
        {
            result.run.eventData(result.parserCounters);
            analysisEventData(userContext, crateIndex, ei, moduleDataList, moduleCount);
        };

    // Timeticks and other system events only feed analysis operators and
    // counters, never the data sources (mvlc_stream_worker.cc:277-312), so
    // they never reach the analysis here either. The five timestamped
    // sections are recorded for the run summary and the rate over time
    // (RunRecorder, listfile_scan.h), with the parser's running counters. The
    // preamble's BeginRun reaches the parser too: read_preamble() seeks back to
    // just after the magic bytes (mvlc_listfile.cc:224-225) before the
    // ReplayWorker reads the stream (mvlc_replay_worker.cc:165, 205).
    callbacks.systemEvent = [&result](void *, int, const u32 *header, u32 size)
    {
        result.run.systemEvent(header, size, result.parserCounters);
    };

    // Replay exactly as MVLCListfileWorker does (mvlc_listfile_worker.cc:230-259),
    // through a handle that checks what it delivers (I2).
    mvlc::listfile::ZipReader zip;
    zip.openArchive(listfile.archivePath);
    CheckedReadHandle readHandle(zip, listfile.listfileEntry);

    mvlc::ReadoutBufferQueues queues(ReadoutBufferSize, ReadoutBufferCount);
    mvlc::ReplayWorker replay(queues, &readHandle);

    // For progress only; read before the replay thread starts reading.
    const uint64_t bytesTotal = readHandle.check().declaredKnown ? readHandle.check().declaredSize : 0;
    uint64_t bytesProcessed = 0;

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

        bytesProcessed += buffer->used();
        queues.emptyBufferQueue().enqueue(buffer);

        if (workers)
            workers->submit(deliverJob);
        else if (batch.size)
            deliver();

        if (progress)
            progress(bytesProcessed, bytesTotal);
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

    if (workers)
    {
        workers->finish(deliverJob);
        workers.reset();
    }

    batch.next() = extractor.endRunMessage();
    batch.commit();
    deliver();

    result.run.finish(result.parserCounters);
    result.splitterCounters = splitterState.counters;
    result.waveformDecoderMessages = decoderMessages->result().count;
    result.parserWarnings = libraryMessages->result();

    result.producerSeconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - tStart - sinkTime).count();

    return result;
}

}
