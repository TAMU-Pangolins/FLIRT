#include "flirt/listfile_scan.h"

#include "flirt/library_log.h"

#include <mesytec-mvlc/mvlc_command_builders.h>
#include <mesytec-mvlc/mvlc_listfile_zip.h>
#include <mesytec-mvlc/mvlc_readout_config.h>
#include <mesytec-mvlc/mvlc_replay_worker.h>
#include <mesytec-mvlc/readout_buffer_queues.h>

namespace mvlc = mesytec::mvlc;
namespace parser = mesytec::mvlc::readout_parser;

namespace flirt
{

namespace
{

// mvme_mvlc::sanitize_readout_stacks (mvlc_daq.cc:455-474): drop command
// groups that produce no output so parser and mvme module indexes line up.
std::vector<mvlc::StackCommandBuilder> sanitize_readout_stacks(
    const std::vector<mvlc::StackCommandBuilder> &inputStacks)
{
    std::vector<mvlc::StackCommandBuilder> result;

    for (const auto &srcStack: inputStacks)
    {
        mvlc::StackCommandBuilder dstStack;
        dstStack.setName(srcStack.getName());

        for (const auto &srcGroup: srcStack.getGroups())
            if (mvlc::produces_output(srcGroup))
                dstStack.addGroup(srcGroup);

        result.emplace_back(dstStack);
    }

    return result;
}

// mvme_context.cc:81-82, as run_pipeline.
constexpr size_t ReadoutBufferCount = 4;
constexpr size_t ReadoutBufferSize = 1024 * 1024;

}

uint64_t readout_events(const parser::ReadoutParserCounters &counters)
{
    uint64_t events = 0;
    for (const auto &hits: counters.eventHits)
        events += hits.second;
    return events;
}

void RunRecorder::systemEvent(const uint32_t *header, uint32_t size, const parser::ReadoutParserCounters &counters)
{
    const uint8_t subtype = mvlc::system_event::extract_subtype(header[0]);

    if (size != 3 || !is_run_time_subtype(subtype))
        return;

    const uint64_t seconds = header[1] | (static_cast<uint64_t>(header[2]) << 32);
    runTimes.record(subtype, static_cast<int64_t>(seconds));
    timeline.record(subtype, static_cast<int64_t>(seconds), readout_events(counters), counters.ethPacketLoss);
}

void RunRecorder::finish(const parser::ReadoutParserCounters &counters)
{
    if (counters.ethPacketLoss != lastLoss_)
    {
        ++lossBursts;
        lastLoss_ = counters.ethPacketLoss;
    }
    timeline.finish(readout_events(counters), counters.ethPacketLoss);
}

parser::ReadoutParserState make_listfile_parser(const ListfileInfo &listfile, const VMEConfig &vmeConfig)
{
    // The readout structure comes from the recorded crate config; mvme derives
    // the same stacks from the VME config (mvlc_stream_worker.cc:549-562).
    auto crateConfig = mvlc::crate_config_from_yaml(listfile.crateConfigYaml);

    if (crateConfig.stacks.size() != vmeConfig.events.size())
        throw unsupported_input("crate config has " + std::to_string(crateConfig.stacks.size())
                                + " readout stacks but the VME config has "
                                + std::to_string(vmeConfig.events.size()) + " events");

    auto parserState = parser::make_readout_parser(sanitize_readout_stacks(crateConfig.stacks));

    for (size_t ei = 0; ei < vmeConfig.events.size(); ++ei)
    {
        auto enabled = make_module_index_map(vmeConfig.events[ei]).size();
        if (parserState.readoutStructure.at(ei).size() != enabled)
            throw unsupported_input("event '" + vmeConfig.events[ei].name.toStdString()
                                    + "': readout stack has "
                                    + std::to_string(parserState.readoutStructure.at(ei).size())
                                    + " output groups but " + std::to_string(enabled)
                                    + " modules are enabled");
    }

    return parserState;
}

ScanResult scan_listfile(const ListfileInfo &listfile, const VMEConfig &vmeConfig)
{
    ScanResult result;
    auto parserState = make_listfile_parser(listfile, vmeConfig);
    // mesytec-mvlc's messages are counted and kept, never printed (library_log.h).
    auto libraryMessages = std::make_shared<MessageSink>(KeptLibraryMessages);
    ScopedLoggerSinks libraryLog(replay_loggers(), libraryMessages);
    auto &pc = result.parserCounters;

    parser::ReadoutParserCallbacks callbacks;
    callbacks.eventData = [&](void *, int, int, const parser::ModuleData *, unsigned)
    {
        result.run.eventData(pc);
    };
    callbacks.systemEvent = [&](void *, int, const uint32_t *header, uint32_t size)
    {
        result.run.systemEvent(header, size, pc);
    };

    // The replay as run_pipeline does it (pipeline.cc, mirroring
    // mvlc_listfile_worker.cc:230-259), through a handle that checks what it
    // delivers (I2).
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

        // MVLC_StreamWorker::processBuffer (mvlc_stream_worker.cc:904-950):
        // exceptions escaping the parser are skipped, as in run_pipeline.
        try
        {
            parser::parse_readout_buffer(static_cast<mvlc::ConnectionType>(buffer->type()), parserState,
                                         callbacks, pc, buffer->bufferNumber(), view.data(), view.size());
        }
        catch (const std::exception &)
        {
        }

        queues.emptyBufferQueue().enqueue(buffer);
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
        replay.stop();
        throw;
    }

    auto counters = replay.counters();
    result.listfileEntry = readHandle.check();

    // As run_pipeline: a read error is recorded (I2), anything else thrown.
    if (counters.eptr && result.listfileEntry.readError.empty())
        std::rethrow_exception(counters.eptr);
    if (counters.ec)
        throw std::runtime_error("replay error: " + counters.ec.message());

    result.run.finish(pc);
    result.parserWarnings = libraryMessages->result();
    return result;
}

}
