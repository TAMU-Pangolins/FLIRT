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

json settings_changes_json(const SettingsChanges &sc)
{
    json changes = json::array();
    for (const auto &c: sc.changes)
    {
        json writes = json::array();
        for (const auto &w: c.writes)
            writes.push_back({ {"address", w.address}, {"value", w.value} });
        changes.push_back({ {"time", c.time}, {"source", c.source}, {"writes", writes},
                            {"writesListed", c.writesListed}, {"commands", or_null(c.commands)},
                            {"error", or_null(c.error)} });
    }
    return { {"checked", sc.checked}, {"reason", sc.checked ? json(nullptr) : json(sc.reason)},
             {"mvmeVersion", sc.mvmeVersion}, {"changes", changes} };
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

SettingsChanges settings_changes(const ListfileInfo &listfile)
{
    const auto &entry = listfile.messagesLogEntry;
    SettingsChanges result;
    if (entry.entry.empty())
        result.reason = "the archive has no messages.log";
    else if (entry.result() != "pass")
        result.reason = "messages.log is damaged or unreadable: " + entry.detail();
    else
        result = find_settings_changes(listfile.messagesLog);
    return result;
}

RunFacts run_facts(const ListfileInfo &listfile, const parser::ReadoutParserCounters &counters,
                   const RunRecorder &recorder)
{
    RunFacts facts;
    facts.times = recorder.runTimes.result();
    facts.counters = counters;
    facts.events = readout_events(counters);
    facts.connection = listfile.format;
    facts.settings = settings_changes(listfile);
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

    const auto sc = run.value("settingsChanges", json::object());
    facts.settings.checked = sc.value("checked", false);
    facts.settings.reason = optional_of<std::string>(sc, "reason").value_or("");
    facts.settings.mvmeVersion = sc.value("mvmeVersion", std::string());
    for (const auto &c: sc.value("changes", json::array()))
    {
        SettingsChange change;
        change.time = c.value("time", std::string());
        change.source = c.value("source", std::string());
        for (const auto &w: c.value("writes", json::array()))
            change.writes.push_back({ w.value("address", std::string()), w.value("value", std::string()) });
        change.writesListed = c.value("writesListed", true);
        change.commands = optional_of<int64_t>(c, "commands");
        change.error = optional_of<std::string>(c, "error");
        facts.settings.changes.push_back(change);
    }
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
             {"events", events}, {"modules", modules},
             {"settingsChanges", settings_changes_json(facts.settings)} };
}

}
