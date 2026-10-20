#include "flirt/run_info.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

#include "common/failures.h"

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

// mesytec-mvlc asks the OS for this socket receive buffer
// (DesiredSocketReceiveBufferSize, mvlc_impl_eth.cc:83).
constexpr uint64_t DesiredReceiveBufferBytes = 10u * 1024 * 1024;

std::string plural(uint64_t n, const char *word)
{
    return std::to_string(n) + " " + word + (n == 1 ? "" : "s");
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

json flag_json(const RateFlag &flag)
{
    switch (flag.kind)
    {
        case RateFlag::Kind::Paused:
            return { {"kind", "paused"}, {"from", iso8601_local(flag.from)}, {"to", time_or_null(flag.to)} };
        case RateFlag::Kind::LowRate:
            return { {"kind", "lowRate"}, {"from", iso8601_local(flag.from)}, {"to", time_or_null(flag.to)} };
        case RateFlag::Kind::Step:
            return { {"kind", "step"}, {"at", iso8601_local(flag.from)}, {"before", flag.before},
                     {"after", flag.after} };
    }
    return nullptr;
}

std::string hh_mm(int64_t t)
{
    return format_local_time(t, "%H:%M");
}

// The top 3 of the minutes with loss by lost packets (the earlier on ties),
// in time order.
std::vector<size_t> top_loss_minutes(const std::vector<MinuteBin> &bins, std::vector<size_t> top)
{
    std::stable_sort(top.begin(), top.end(),
                     [&](size_t a, size_t b) { return bins[a].lostPackets > bins[b].lostPackets; });
    if (top.size() > 3)
        top.resize(3);
    std::sort(top.begin(), top.end());
    return top;
}

// The receive buffer was below what mesytec-mvlc asks for, and packets were lost.
bool low_receive_buffer(const RunFacts &facts)
{
    return facts.counters.ethPacketLoss && facts.log.receiveBufferBytes && facts.log.receiveBuffer
        && *facts.log.receiveBufferBytes < DesiredReceiveBufferBytes;
}

// "1,073,420": thousands separated (common/failures.h).
std::string number(uint64_t n)
{
    return separate_thousands(std::to_string(n));
}

// "2026-03-03T14:31:00-06:00" (iso8601_local) -> Unix seconds; nullopt if
// not that form.
std::optional<int64_t> parse_iso8601(const json &value)
{
    if (!value.is_string())
        return std::nullopt;
    int y, mo, d, h, mi, sec, oh, om;
    char sign;
    if (std::sscanf(value.get<std::string>().c_str(), "%d-%d-%dT%d:%d:%d%c%d:%d", &y, &mo, &d, &h, &mi, &sec,
                    &sign, &oh, &om) != 9 || (sign != '+' && sign != '-'))
        return std::nullopt;
    struct tm tm = {};
    tm.tm_year = y - 1900, tm.tm_mon = mo - 1, tm.tm_mday = d, tm.tm_hour = h, tm.tm_min = mi, tm.tm_sec = sec;
    const int64_t offset = (sign == '-' ? -1 : 1) * (oh * 3600 + om * 60);
    return static_cast<int64_t>(timegm(&tm)) - offset;
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
    facts.rate = recorder.timeline.analyze(facts.times);
    facts.counters = counters;
    facts.events = readout_events(counters);
    facts.lossBursts = recorder.lossBursts;
    facts.connection = listfile.format;
    // As the settings-changes check: only an intact messages.log.
    if (!listfile.messagesLogEntry.entry.empty() && listfile.messagesLogEntry.result() == "pass")
        facts.log = read_messages_log_facts(listfile.messagesLog);
    facts.settings = settings_changes(listfile);
    return facts;
}

uint64_t events_lost_estimate(uint64_t lost, uint64_t received, uint64_t events, uint64_t bursts)
{
    const double perPacket = received ? double(events) / double(received) : 0.0;
    return static_cast<uint64_t>(std::llround(double(lost) * perPacket + double(bursts)));
}

std::string share_text(uint64_t part, uint64_t whole)
{
    const double percent = whole ? 100.0 * double(part) / double(whole) : 0.0;
    if (percent < 0.0001)
        return part ? "< 0.0001 %" : "0 %";

    // Two significant digits, then as many decimals as they need.
    int e = static_cast<int>(std::floor(std::log10(percent)));
    const double unit = std::pow(10.0, e - 1);
    const double rounded = std::round(percent / unit) * unit;
    if (rounded >= std::pow(10.0, e + 1))
        ++e;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f %%", std::max(0, 1 - e), rounded);
    return buf;
}

size_t minutes_with_loss(const RateAnalysis &rate)
{
    return static_cast<size_t>(std::count_if(rate.minutes.begin(), rate.minutes.end(),
                                             [](const MinuteBin &b) { return b.lostPackets > 0; }));
}

std::string loss_text(const RunFacts &facts)
{
    const uint64_t lost = facts.counters.ethPacketLoss, received = facts.counters.ethPacketsProcessed;
    if (!lost)
        return {};

    std::string text = std::to_string(lost) + " of " + std::to_string(received + lost) + " ("
        + share_text(lost, received + lost) + "), about "
        + plural(events_lost_estimate(lost, received, facts.events, facts.lossBursts), "event") + ", in "
        + plural(facts.lossBursts, "burst");

    const auto &bins = facts.rate.minutes;
    if (bins.empty())
        return text;

    const size_t live = static_cast<size_t>(std::count_if(bins.begin(), bins.end(),
                                                          [](const MinuteBin &b) { return b.liveSeconds > 0; }));
    std::vector<size_t> withLoss;
    for (size_t i = 0; i < bins.size(); ++i)
        if (bins[i].lostPackets)
            withLoss.push_back(i);

    const auto top = top_loss_minutes(bins, withLoss);

    text += "; in " + std::to_string(withLoss.size()) + " of " + std::to_string(live) + " minutes, "
        + (withLoss.size() > 3 ? "most in " : "in ");
    for (size_t j = 0; j < top.size(); ++j)
        text += (j ? ", " : "") + hh_mm(bins[top[j]].start) + " (" + std::to_string(bins[top[j]].lostPackets) + ")";
    return text;
}

std::string receive_buffer_note(const RunFacts &facts)
{
    // mesytec-mvlc asks for 10 MiB (mvlc_impl_eth.cc:83); the OS grants at
    // most net.core.rmem_max (doubled), and mvme's own note: "Setting
    // rmem_max can help a lot ... net.core.rmem_max=104857600"
    // (src/mvlc/mvlc-TODO.txt:97-98). mvme logs what it got (mvlc_daq.cc:584-586).
    if (!low_receive_buffer(facts))
        return {};
    return "note: the DAQ PC's receive buffer for the MVLC was " + *facts.log.receiveBuffer
        + ", below the 10 MB mesytec-mvlc asks for; mesytec's MVLC notes suggest raising net.core.rmem_max"
          " (e.g. to 104857600) on the DAQ PC";
}

std::string receive_buffer_text(const RunFacts &facts)
{
    if (!low_receive_buffer(facts))
        return {};
    return "the DAQ PC's receive buffer for the MVLC was " + *facts.log.receiveBuffer
        + ", below the 10 MB mesytec-mvlc asks for; mesytec's MVLC notes suggest raising net.core.rmem_max"
          " on the DAQ PC (e.g. to 104857600)";
}

std::string loss_block_text(const RunFacts &facts)
{
    const uint64_t lost = facts.counters.ethPacketLoss, received = facts.counters.ethPacketsProcessed;
    if (!lost)
        return {};

    std::string text = number(lost) + " of " + number(received + lost) + " packets ("
        + share_text(lost, received + lost) + ") in " + number(facts.lossBursts)
        + (facts.lossBursts == 1 ? " burst" : " bursts") + ", about "
        + number(events_lost_estimate(lost, received, facts.events, facts.lossBursts))
        + (events_lost_estimate(lost, received, facts.events, facts.lossBursts) == 1 ? " event" : " events");

    const auto &bins = facts.rate.minutes;
    if (bins.empty())
        return text;

    const size_t live = static_cast<size_t>(std::count_if(bins.begin(), bins.end(),
                                                          [](const MinuteBin &b) { return b.liveSeconds > 0; }));
    std::vector<size_t> withLoss;
    for (size_t i = 0; i < bins.size(); ++i)
        if (bins[i].lostPackets)
            withLoss.push_back(i);
    const auto top = top_loss_minutes(bins, withLoss);
    if (top.empty())
        return text;

    // "14:31, 14:52 and 15:03 (3 each)", or each with its count.
    const bool same = std::all_of(top.begin(), top.end(),
                                  [&](size_t i) { return bins[i].lostPackets == bins[top[0]].lostPackets; });
    text += "; in " + std::to_string(withLoss.size()) + " of " + std::to_string(live) + " minutes, "
        + (withLoss.size() > 3 ? "most at " : "at ");
    for (size_t j = 0; j < top.size(); ++j)
        text += (j == 0 ? "" : j + 1 == top.size() ? " and " : ", ") + hh_mm(bins[top[j]].start)
            + (same ? "" : " (" + number(bins[top[j]].lostPackets) + ")");
    if (same)
        text += " (" + number(bins[top[0]].lostPackets) + (top.size() > 1 ? " each)" : ")");
    return text;
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

    const auto packets = run.value("packets", json());
    facts.connection = packets.is_object() ? mvlc::ConnectionType::ETH : mvlc::ConnectionType::USB;
    if (packets.is_object())
    {
        facts.counters.ethPacketsProcessed = packets.value("received", uint64_t(0));
        facts.counters.ethPacketLoss = packets.value("lost", uint64_t(0));
        facts.lossBursts = packets.value("bursts", uint64_t(0));
    }

    facts.log.receiveBuffer = optional_of<std::string>(run, "receiveBuffer");
    facts.log.receiveBufferBytes = optional_of<uint64_t>(run, "receiveBufferBytes");
    facts.log.jumboFrames = optional_of<bool>(run, "jumboFrames");
    const auto mvme = run.value("mvme", json::object());
    facts.log.mvmeVersion = optional_of<std::string>(mvme, "version");
    facts.log.system = optional_of<std::string>(mvme, "system");

    for (const auto &bin: run.value("minutes", json::array()))
    {
        MinuteBin b;
        b.start = parse_iso8601(bin.value("start", json())).value_or(0);
        b.liveSeconds = bin.value("liveSeconds", int64_t(0));
        b.events = bin.value("events", uint64_t(0));
        b.lostPackets = bin.value("lostPackets", uint64_t(0));
        facts.rate.minutes.push_back(b);
    }
    const auto rate = run.value("rate", json::object());
    facts.rate.median = optional_of<double>(rate, "medianPerSecond");
    for (const auto &f: rate.value("flags", json::array()))
    {
        RateFlag flag;
        const auto kind = f.value("kind", std::string());
        flag.kind = kind == "lowRate" ? RateFlag::Kind::LowRate : kind == "step" ? RateFlag::Kind::Step
                                                                                 : RateFlag::Kind::Paused;
        flag.from = parse_iso8601(f.value(flag.kind == RateFlag::Kind::Step ? "at" : "from", json())).value_or(0);
        flag.to = parse_iso8601(f.value("to", json()));
        flag.before = f.value("before", 0.0);
        flag.after = f.value("after", 0.0);
        facts.rate.flags.push_back(flag);
    }

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

    json packets = nullptr;
    if (facts.connection == mvlc::ConnectionType::ETH)
    {
        const uint64_t lost = pc.ethPacketLoss, received = pc.ethPacketsProcessed;
        packets = { {"received", received}, {"lost", lost},
                    {"lostShare", received + lost ? double(lost) / double(received + lost) : 0.0},
                    {"bursts", facts.lossBursts},
                    {"eventsLostEstimate", events_lost_estimate(lost, received, facts.events, facts.lossBursts)},
                    {"minutesWithLoss", facts.rate.minutes.empty() ? json(nullptr)
                                                                   : json(minutes_with_loss(facts.rate))} };
    }

    json flags = json::array();
    for (const auto &flag: facts.rate.flags)
        flags.push_back(flag_json(flag));

    json minutes = json::array();
    for (const auto &bin: facts.rate.minutes)
        minutes.push_back({ {"start", iso8601_local(bin.start)}, {"liveSeconds", bin.liveSeconds},
                            {"events", bin.events}, {"lostPackets", bin.lostPackets} });

    return { {"start", time_or_null(times.start)}, {"stop", time_or_null(times.stop)},
             {"startUnix", or_null(times.start)}, {"stopUnix", or_null(times.stop)},
             {"startFrom", text_or_null(times.startFrom)}, {"stopFrom", text_or_null(times.stopFrom)},
             {"durationSeconds", or_null(times.duration)}, {"pauses", times.pauses},
             {"pausedSeconds", or_null(times.paused)}, {"liveSeconds", or_null(times.live)},
             {"events", events}, {"modules", modules},
             {"settingsChanges", settings_changes_json(facts.settings)},
             {"packets", packets},
             {"receiveBuffer", or_null(facts.log.receiveBuffer)},
             {"receiveBufferBytes", or_null(facts.log.receiveBufferBytes)},
             {"jumboFrames", or_null(facts.log.jumboFrames)},
             {"mvme", { {"version", or_null(facts.log.mvmeVersion)}, {"system", or_null(facts.log.system)} }},
             {"rate", { {"medianPerSecond", or_null(facts.rate.median)}, {"flags", flags} }},
             {"minutes", minutes} };
}

}
