// flirt-tui's details: the listfile's run information from flirt-info --json
// (docs/design.md, "TUI").

#include <algorithm>
#include <cmath>
#include <set>
#include <sys/stat.h>

#include "common/platform.h"
#include "model.h"

namespace flirt::tui
{

namespace
{

// rate_rules::UsableMinuteSeconds (src/flirt/run_timeline.h): fewer live
// seconds make a minute's rate too uncertain to count.
constexpr double UsableSeconds = 10;

bool same_time(const timespec &a, const timespec &b)
{
    return a.tv_sec == b.tv_sec && a.tv_nsec == b.tv_nsec;
}

// "2026-03-03T14:10:00-06:00" -> "14:10".
std::string hh_mm(const json &minute)
{
    const auto start = minute.value("start", json());
    return start.is_string() && start.get<std::string>().size() >= 16 ? start.get<std::string>().substr(11, 5)
                                                                       : std::string("??:??");
}

double number(const json &minute, const char *key)
{
    const auto v = minute.value(key, json());
    return v.is_number() ? v.get<double>() : 0;
}

}

void request_info(Model &m, const std::string &path, bool again)
{
    struct stat st;
    if (stat(path.c_str(), &st) != 0)
    {
        // flirt-info reports the problem; nothing to compare the next time.
        if (!again && m.infos.count(path))
            return;
        m.infos[path] = RunInfo{ false, -1, {}, true, nullptr };
        m.infoOutbox.push_back(path);
        return;
    }
    const auto size = static_cast<int64_t>(st.st_size);
    const auto mtime = flirt::stat_mtime(st);
    const auto found = m.infos.find(path);
    if (found != m.infos.end() && found->second.known && found->second.size == size
        && same_time(found->second.mtime, mtime) && (found->second.reading || !found->second.answer.is_null()))
        return;
    m.infos[path] = RunInfo{ true, size, mtime, true, nullptr };
    m.infoOutbox.push_back(path);
}

void apply_info_answer(Model &m, const std::string &path, int64_t size, const timespec &mtime, const json &answer)
{
    m.infos[path] = RunInfo{ size >= 0, size, mtime, false, answer };
    m.infoReplan = true;
}

void plan_info(Model &m)
{
    const auto *cursor = selected_file(m);
    const std::string cursorPath = cursor ? cursor->path : "";
    const bool converting = busy(m);
    if (!m.infoReplan && cursorPath == m.infoPlannedCursor && converting == m.infoPlannedBusy
        && m.infoPlannedRows.size() == m.files.size()
        && std::equal(m.files.begin(), m.files.end(), m.infoPlannedRows.begin(),
                      [](const FileEntry &f, const std::string &p) { return f.path == p; }))
        return;
    m.infoReplan = false;
    m.infoPlannedCursor = cursorPath;
    m.infoPlannedBusy = converting;
    m.infoPlannedRows.clear();
    for (const auto &f: m.files)
        m.infoPlannedRows.push_back(f.path);
    ++m.infoPlans;
    if (cursor)
        request_info(m, cursor->path, false);
    std::set<std::string> rows;
    for (const auto &f: m.files)
    {
        rows.insert(f.path);
        if (!cursor || f.path != cursor->path)
            request_info(m, f.path, false);
    }
    for (auto it = m.infos.begin(); it != m.infos.end();)
        if (it->second.reading && !rows.count(it->first))
        {
            m.infoDrops.push_back(it->first);
            it = m.infos.erase(it);
        }
        else
            ++it;
}

RateGraph rate_graph(const json &minutes, int width)
{
    if (!minutes.is_array() || width < 10)
        return {};
    const size_t n = minutes.size();
    const size_t w = static_cast<size_t>(width);
    const size_t g = n <= w ? 1 : (n + w - 1) / w;
    const size_t cells = (n + g - 1) / g;
    if (cells < 2)
        return {};

    std::vector<double> rates(cells, -1), live(cells, 0);     // -1: paused
    for (size_t c = 0; c < cells; ++c)
    {
        double events = 0;
        for (size_t i = c * g; i < std::min(n, (c + 1) * g); ++i)
        {
            events += number(minutes[i], "events");
            live[c] += number(minutes[i], "liveSeconds");
        }
        if (live[c] > 0)
            rates[c] = events / live[c];
    }
    double top = 0, topAll = 0;
    bool usable = false;
    for (size_t c = 0; c < cells; ++c)
    {
        topAll = std::max(topAll, rates[c]);
        if (live[c] >= UsableSeconds)
        {
            top = usable ? std::max(top, rates[c]) : rates[c];
            usable = true;
        }
    }
    if (!usable)
        top = topAll;

    static const char *const levels[] = { "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█" };
    RateGraph graph;
    for (size_t c = 0; c < cells; ++c)
    {
        if (rates[c] < 0)
            graph.bars += " ";
        else
        {
            const int level = top > 0 ? static_cast<int>(std::floor(rates[c] / top * 8)) : 0;
            graph.bars += levels[std::clamp(level, 0, 7)];
        }
    }
    if (cells >= 12)
    {
        const auto first = hh_mm(minutes[0]), last = hh_mm(minutes[n - 1]);
        graph.times = first + std::string(cells - first.size() - last.size(), ' ') + last;
    }
    return graph;
}

}
