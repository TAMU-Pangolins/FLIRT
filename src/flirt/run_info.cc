#include "flirt/run_info.h"

namespace mvlc = mesytec::mvlc;
namespace parser = mesytec::mvlc::readout_parser;
using json = nlohmann::json;

namespace flirt
{

namespace
{

template <typename T>
json or_null(const std::optional<T> &value)
{
    return value ? json(*value) : json(nullptr);
}

json time_or_null(const std::optional<int64_t> &t)
{
    return t ? json(iso8601_local(*t)) : json(nullptr);
}

json text_or_null(const std::string &text)
{
    return text.empty() ? json(nullptr) : json(text);
}

template <typename T>
std::optional<T> optional_of(const json &object, const char *key)
{
    const auto it = object.find(key);
    if (it == object.end() || it->is_null())
        return std::nullopt;
    return it->get<T>();
}

}

RunFacts run_facts(const ListfileInfo &listfile, const parser::ReadoutParserCounters &counters,
                   const RunRecorder &recorder)
{
    RunFacts facts;
    facts.times = recorder.runTimes.result();
    facts.counters = counters;
    facts.events = readout_events(counters);
    facts.connection = listfile.format;
    return facts;
}

RunFacts run_facts_from_json(const json &run)
{
    RunFacts facts;
    auto &times = facts.times;
    times.start = optional_of<int64_t>(run, "startUnix");
    times.stop = optional_of<int64_t>(run, "stopUnix");
    times.startFrom = optional_of<std::string>(run, "startFrom").value_or("");
    times.stopFrom = optional_of<std::string>(run, "stopFrom").value_or("");
    times.duration = optional_of<int64_t>(run, "durationSeconds");
    times.pauses = optional_of<size_t>(run, "pauses").value_or(0);
    times.paused = optional_of<int64_t>(run, "pausedSeconds");
    times.live = optional_of<int64_t>(run, "liveSeconds");

    for (const auto &event: run.value("events", json::array()))
        facts.events += event.value("readoutEvents", uint64_t(0));
    return facts;
}

json run_json(const RunFacts &facts, const VMEConfig &vmeConfig)
{
    const auto &times = facts.times;
    const auto &pc = facts.counters;

    json events = json::array(), modules = json::array();
    for (size_t ei = 0; ei < vmeConfig.events.size(); ++ei)
    {
        const auto &event = vmeConfig.events[ei];
        const auto hits = pc.eventHits.find(static_cast<int>(ei));
        const size_t readoutEvents = hits == pc.eventHits.end() ? 0 : hits->second;
        events.push_back({ {"event", ei}, {"name", event.name.toStdString()},
                           {"readoutEvents", readoutEvents},
                           {"ratePerSecond", times.live && *times.live > 0
                                ? json(static_cast<double>(readoutEvents) / static_cast<double>(*times.live))
                                : json(nullptr)} });

        // The parser's part index is the index among the enabled modules, as
        // the pipeline maps it (make_module_index_map, vme_analysis_common.cc:444-469).
        const auto indexMap = make_module_index_map(event);
        for (size_t pmi = 0; pmi < indexMap.size(); ++pmi)
        {
            const auto part = pc.groupHits.find({ static_cast<int>(ei), static_cast<int>(pmi) });
            modules.push_back({ {"event", ei}, {"module", event.modules[indexMap[pmi]].name.toStdString()},
                                {"hits", part == pc.groupHits.end() ? size_t(0) : part->second} });
        }
    }

    return { {"start", time_or_null(times.start)}, {"stop", time_or_null(times.stop)},
             {"startUnix", or_null(times.start)}, {"stopUnix", or_null(times.stop)},
             {"startFrom", text_or_null(times.startFrom)}, {"stopFrom", text_or_null(times.stopFrom)},
             {"durationSeconds", or_null(times.duration)}, {"pauses", times.pauses},
             {"pausedSeconds", or_null(times.paused)}, {"liveSeconds", or_null(times.live)},
             {"events", events}, {"modules", modules} };
}

}
