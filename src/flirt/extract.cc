#include "flirt/extract.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <stdexcept>

#include "analysis/a2/memory.h"
#include "common/utf8.h"
#include "util/variablify.h"

namespace es = mvme::event_server;
namespace parser = mesytec::mvlc::readout_parser;

namespace flirt
{

namespace
{

// event_server_util.cc:28-40
es::StorageType get_storage_type(unsigned bits)
{
    static const std::array<unsigned, 4> typewidths = {{8, 16, 32, 64}};

    for (size_t i = 0; i < typewidths.size(); i++)
        if (bits <= typewidths[i])
            return static_cast<es::StorageType>(i);

    return es::StorageType::st_uint64_t;
}

std::string variablified(const QString &name)
{
    return variablify(name).toStdString();
}

using SampleEvents = std::deque<mesytec::mvlc::Protected<
    mesytec::mvme::mdpp_sampling::DecodedMdppSampleEvent>>;

// a2_adapter_build / a2_adapter_build_datasources (a2_adapter.cc:1643-1680,
// 1194-1296): one DataSource per configured source, in a2 order. dsIndexes
// receives, per event, the original index of each a2 data source.
a2::A2 *build_a2(memory::Arena &arena, const AnalysisConfig &analysis,
                 SampleEvents &sampleEvents, std::vector<std::vector<u32>> &dsIndexes)
{
    auto a2 = arena.pushObject<a2::A2>(&arena);
    dsIndexes.assign(analysis.dataSources.size(), {});

    for (size_t ei = 0; ei < analysis.dataSources.size(); ++ei)
    {
        const auto &sources = analysis.dataSources[ei];
        a2->dataSources[ei] = arena.pushArray<a2::DataSource>(sources.size());

        for (size_t dsIndex = 0; dsIndex < sources.size(); ++dsIndex)
        {
            const auto &src = sources[dsIndex];
            a2::DataSource ds = {};

            switch (src.kind)
            {
                case DataSourceConfig::Kind::Extractor:
                    ds = a2::make_datasource_extractor(
                        &arena, src.filter, src.requiredCompletionCount, src.rngSeed,
                        src.moduleIndex, src.options);
                    break;
                case DataSourceConfig::Kind::ListFilterExtractor:
                    ds = a2::make_datasource_listfilter_extractor(
                        &arena, src.listFilter, src.repetitions, src.rngSeed,
                        src.moduleIndex, src.options);
                    break;
                case DataSourceConfig::Kind::MdppSampleDecoder:
                    ds = a2::make_datasource_mdpp_sample_decoder(
                        &arena, src.moduleTypeName, src.maxChannels, src.maxSamples,
                        sampleEvents.emplace_back(), src.rngSeed, src.moduleIndex, src.options);
                    break;
            }

            a2->dataSources[ei][a2->dataSourceCounts[ei]++] = ds;
            dsIndexes[ei].push_back(dsIndex);
        }
    }

    return a2;
}

// make_event_data_descriptions (event_server_util.cc:94-163), from an a2 that
// holds all sources.
es::EventDataDescriptions describe(const a2::A2 *a2, const VMEConfig &vmeConfig,
                                   const AnalysisConfig &analysis)
{
    es::EventDataDescriptions edds;

    for (int ei = 0; ei < static_cast<int>(vmeConfig.events.size()); ++ei)
    {
        es::EventDataDescription edd;
        edd.eventIndex = ei;
        const u32 dsCount = a2->dataSourceCounts[ei];

        for (u32 dsIndex = 0; dsIndex < dsCount; ++dsIndex)
        {
            const auto &src = analysis.dataSources[ei][dsIndex];
            const auto &a2ds = a2->dataSources[ei][dsIndex];
            u32 outputSize = a2ds.outputs[0].size;
            u32 outputBits = std::ceil(std::log2(outputSize));

            es::DataSourceDescription dsd;
            dsd.name = variablified(src.name);
            dsd.moduleIndex = a2ds.moduleIndex;
            dsd.size = outputSize;
            dsd.lowerLimit = a2ds.outputLowerLimits[0][0];
            dsd.upperLimit = a2ds.outputUpperLimits[0][0];
            dsd.bits = src.dataBits;
            dsd.indexType = get_storage_type(outputBits);
            dsd.valueType = get_storage_type(src.dataBits);

            // Parameter names are only sent for analysis::Extractor.
            if (src.kind == DataSourceConfig::Kind::Extractor)
            {
                size_t paramCount = std::min(static_cast<size_t>(src.parameterNames.size()),
                                             static_cast<size_t>(dsd.size));
                for (size_t pi = 0; pi < paramCount; ++pi)
                    dsd.paramNames.push_back(variablified(src.parameterNames[pi]));
            }

            edd.dataSources.push_back(dsd);
        }

        edds.push_back(edd);
    }

    return edds;
}

}

struct Extractor::Private
{
    memory::Arena arena;
    a2::A2 *a2 = nullptr;
    // Per event: the original data-source index of each a2 data source.
    std::vector<std::vector<u32>> dsIndexes;
    // Per waveform source; mvme keeps one in each DataSourceMdppSampleDecoder
    // for its UI (analysis.h).
    SampleEvents sampleEvents;
    std::vector<std::vector<int>> moduleIndexMaps;
    es::EventDataDescriptions edds;
    es::VMETree vmeTree;
    std::vector<uint8_t> outBuf = std::vector<uint8_t>(10 * 1024); // event_server.cc:68
};

Extractor::Extractor(const VMEConfig &vmeConfig, const AnalysisConfig &analysis)
    : d(std::make_unique<Private>())
{
    d->a2 = build_a2(d->arena, analysis, d->sampleEvents, d->dsIndexes);
    a2::a2_begin_run(d->a2, {});

    for (const auto &event: vmeConfig.events)
        d->moduleIndexMaps.push_back(make_module_index_map(event));

    // make_vme_tree_description (event_server_util.cc:62-91)
    for (int ei = 0; ei < static_cast<int>(vmeConfig.events.size()); ++ei)
    {
        const auto &event = vmeConfig.events[ei];
        es::VMEEvent vmeEvent;
        vmeEvent.eventIndex = ei;
        vmeEvent.name = variablified(event.name);

        for (int mi = 0; mi < static_cast<int>(event.modules.size()); ++mi)
        {
            es::VMEModule vmeModule;
            vmeModule.moduleIndex = mi;
            vmeModule.name = variablified(event.modules[mi].name);
            vmeModule.type = event.modules[mi].typeName.toStdString();
            vmeEvent.modules.push_back(vmeModule);
        }

        d->vmeTree.events.push_back(vmeEvent);
    }

    d->edds = describe(d->a2, vmeConfig, analysis);
}

Extractor::~Extractor()
{
    // The A2 object lives in the arena; destroy it before the arena goes.
    if (d->a2)
        d->a2->~A2();
}

const es::EventDataDescriptions &Extractor::eventDataDescriptions() const { return d->edds; }
const es::VMETree &Extractor::vmeTree() const { return d->vmeTree; }

es::Message Extractor::beginRunMessage(const RunInfo &runInfo) const
{
    es::json info;
    info["vmeTree"] = es::to_json(d->vmeTree);
    info["eventDataSources"] = es::to_json(d->edds);
    info["runId"] = runInfo.runId;
    info["isReplay"] = true;
    info["runInProgress"] = false;
    info["ExperimentName"] = runInfo.experimentName;
    info["ExperimentTitle"] = runInfo.experimentTitle;
    info["MVMEWorkspace"] = std::string();
    info["replaySourceFile"] = runInfo.replaySourceFile;
    info["listfileLogBuffer"] = runInfo.listfileLogBuffer;

    es::Message msg;
    msg.type = es::MessageType::BeginRun;
    // Cleaned: a run ID, title or log that is not valid UTF-8 would make
    // dump() throw (common/utf8.h). The writer names its outputs from
    // RootOutputOptions::runId, the real one.
    auto text = dump_valid_utf8(info);
    msg.contents.assign(text.begin(), text.end());
    return msg;
}

es::Message Extractor::endRunMessage() const
{
    es::Message msg;
    msg.type = es::MessageType::EndRun;
    auto text = es::json::object().dump();
    msg.contents.assign(text.begin(), text.end());
    return msg;
}

bool Extractor::processEvent(int eventIndex, const parser::ModuleData *moduleDataList,
                             unsigned moduleCount, es::Message &out)
{
    if (eventIndex < 0 || eventIndex >= static_cast<int>(d->edds.size()))
        return false;

    // mvlc_stream_worker.cc:195-246 with Analysis::beginEvent/processModuleData/
    // endEvent (analysis.cc:6240-6274).
    a2::a2_begin_event(d->a2, eventIndex);

    const auto &indexMap = d->moduleIndexMaps[eventIndex];

    for (unsigned pmi = 0; pmi < moduleCount; ++pmi)
    {
        const auto &moduleData = moduleDataList[pmi];
        int moduleIndex = pmi < indexMap.size() ? indexMap[pmi] : -1;

        if (moduleData.data.size && moduleIndex >= 0)
        {
            a2::a2_process_module_data(d->a2, eventIndex, moduleIndex,
                                       moduleData.data.data, moduleData.data.size);
        }
    }

    a2::a2_end_event(d->a2, eventIndex);

    // EventServer::endEvent (event_server.cc:478-632). The message is sent if
    // the event has data sources (:500-507).
    const auto &edd = d->edds[eventIndex];

    if (edd.dataSources.empty())
        return false;

    const u32 a2Count = d->a2->dataSourceCounts[eventIndex];

    while (true)
    {
        try
        {
            es::BufferIterator buf(d->outBuf.data(), d->outBuf.size());
            buf.push(static_cast<u8>(eventIndex));

            for (u32 a2Index = 0; a2Index < a2Count; ++a2Index)
            {
                const u32 dsIndex = d->dsIndexes[eventIndex][a2Index];
                buf.push(static_cast<u8>(dsIndex));
                u16 *countPtr = buf.push(static_cast<u16>(0u));

                const a2::DataSource *ds = d->a2->dataSources[eventIndex] + a2Index;
                const auto &data = ds->outputs[0];
                const auto &dsd = edd.dataSources[dsIndex];
                u16 count = 0u;

                for (s32 paramIndex = 0; paramIndex < data.size; ++paramIndex)
                {
                    double dParamValue = data[paramIndex];

                    if (!a2::is_param_valid(dParamValue))
                        continue;

                    switch (dsd.indexType)
                    {
                        case es::StorageType::st_uint8_t: buf.push(static_cast<u8>(paramIndex)); break;
                        case es::StorageType::st_uint16_t: buf.push(static_cast<u16>(paramIndex)); break;
                        case es::StorageType::st_uint32_t: buf.push(static_cast<u32>(paramIndex)); break;
                        case es::StorageType::st_uint64_t: buf.push(static_cast<u64>(paramIndex)); break;
                    }

                    // Strip the random added by the data source (event_server.cc:567-570).
                    // mvme converts floor(value) to u64 directly, which C++ leaves
                    // undefined for negative values (waveform samples can be negative).
                    // x86-64 performs that conversion through a signed 64-bit integer;
                    // do the same explicitly. Identical for every non-negative value.
                    u64 iParamValue = static_cast<u64>(static_cast<s64>(std::floor(dParamValue)));

                    switch (dsd.valueType)
                    {
                        case es::StorageType::st_uint8_t: buf.push(static_cast<u8>(iParamValue)); break;
                        case es::StorageType::st_uint16_t: buf.push(static_cast<u16>(iParamValue)); break;
                        case es::StorageType::st_uint32_t: buf.push(static_cast<u32>(iParamValue)); break;
                        case es::StorageType::st_uint64_t: buf.push(static_cast<u64>(iParamValue)); break;
                    }

                    ++count;
                }

                *countPtr = count;
            }

            out.type = es::MessageType::EventData;
            out.contents.assign(d->outBuf.data(), buf.buffp);
            return true;
        }
        catch (const es::end_of_buffer &)
        {
            d->outBuf.resize(d->outBuf.size() * 2); // event_server.cc:609-616
        }
    }
}

}
