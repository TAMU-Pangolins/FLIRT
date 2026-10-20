// flirt-tui's waveform view, the logic that does not draw (docs/design.md,
// "TUI", "Waveform view"): the requests to flirt-waveforms and what
// its answers change. The requests leave through the outbox (input.cc sends
// it with Actions::waveforms).

#include <algorithm>
#include <sys/stat.h>

#include "model.h"
#include "plot_layout.h"

namespace flirt::tui
{

namespace
{

const std::string WaveformsSuffix = "_waveforms.root";

bool ends_with(const std::string &s, const std::string &suffix)
{
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

void send(Model &m, json request)
{
    auto &w = m.waves;
    request["id"] = ++m.waveIds;
    request["file"] = w.file;
    w.lastId = m.waveIds;
    w.outbox = std::move(request);
}

}

std::string run_waveforms(const json *run)
{
    if (!run || !run->is_object() || run->find("outputs") == run->end() || !(*run)["outputs"].is_array())
        return {};
    for (const auto &path: (*run)["outputs"])
        if (path.is_string() && ends_with(path.get<std::string>(), WaveformsSuffix))
            return path.get<std::string>();
    return {};
}

void note_waveforms(Model &m)
{
    if (!m.job.is_object() || !m.job.value("runs", json()).is_array())
        return;
    const auto job = m.job.value("started", json()).dump();
    for (const auto &run: m.job["runs"])
    {
        // A run skipped as already converted has its existing files in its
        // outputs too (src/ui/job.cc, the "result" event).
        const auto result = run.value("result", json());
        if (result != "converted" && result != "skipped")
            continue;
        const auto path = run_waveforms(&run);
        if (path.empty() || !m.wavesNoted.insert(job + "\n" + path).second)
            continue;
        const bool listed = std::any_of(m.waveFiles.begin(), m.waveFiles.end(),
                                        [&](const WaveEntry &e) { return e.path == path; });
        if (listed)
            continue;
        // After the published ones, before the ones opened; the selection
        // stays on its file.
        const auto at = std::find_if(m.waveFiles.begin(), m.waveFiles.end(), [](const WaveEntry &e) { return e.opened; });
        const int index = static_cast<int>(at - m.waveFiles.begin());
        struct stat st;
        const int64_t size = stat(path.c_str(), &st) == 0 ? static_cast<int64_t>(st.st_size) : -1;
        m.waveFiles.insert(at, { path, size, false });
        if (m.waveFiles.size() > 1 && index <= m.waveRow)
            ++m.waveRow;
        if (m.waveFiles.size() > 1 && index <= m.waveCursor)
            ++m.waveCursor;
    }
}

void open_waveforms_file(Model &m, const std::string &path, int64_t size)
{
    auto at = std::find_if(m.waveFiles.begin(), m.waveFiles.end(), [&](const WaveEntry &e) { return e.path == path; });
    if (at == m.waveFiles.end())
        at = m.waveFiles.insert(m.waveFiles.end(), { path, size, true });
    m.waveRow = m.waveCursor = static_cast<int>(at - m.waveFiles.begin());
    m.wavesTab = true;
}

void sync_waveforms(Model &m)
{
    const auto *entry = m.wavesTab ? selected_wave(m) : nullptr;
    if (!entry)
    {
        if (m.waves.open)
            close_waveforms(m);
        return;
    }
    if (!m.waves.open || m.waves.file != entry->path)
        show_waveforms(m, entry->path);
}

void show_waveforms(Model &m, const std::string &file)
{
    if (m.waves.open)
        close_waveforms(m);
    const auto saved = m.waveViews.find(file);
    if (saved == m.waveViews.end() || saved->second.trees.empty())
    {
        m.waves = Waves();
        m.waves.open = true;
        m.waves.file = file;
        m.waves.firstId = m.waveIds + 1;
        send(m, { {"op", "trees"} });
        return;
    }
    // As it was left: its module, filter and own y ranges; its frame drawn until the
    // answer for its position comes (older answers are another showing's).
    m.waves = saved->second;
    auto &w = m.waves;
    w.open = true;
    w.firstId = m.waveIds + 1;
    w.going = false;
    w.ranging = false;
    w.error.clear();
    const int64_t position = w.wantPosition >= 0 ? w.wantPosition
        : w.frame.is_object() ? w.frame.value("position", int64_t(-1)) : -1;
    const auto event = w.frame.is_object() ? w.frame.value("event", json()) : json();
    request_frame(m, position >= 0 ? json({ {"position", position} })
                     : event.is_number() ? json({ {"event", event} }) : json({ {"position", 0} }));
}

void close_waveforms(Model &m)
{
    if (!m.waves.open)
        return;
    // Another file, or the tab left: the play pauses.
    pause(m);
    m.dragging = false;
    m.waves.open = false;
    m.waves.outbox = nullptr;
    m.waves.going = false;
    m.waves.ranging = false;
    m.waves.rangeError.clear();
    m.waveViews[m.waves.file] = m.waves;
}

bool waves_reading(const Model &m)
{
    return m.waves.shownId < m.waves.lastId;
}

void request_frame(Model &m, const json &fields)
{
    auto &w = m.waves;
    if (w.module < 0 || w.module >= static_cast<int>(w.trees.size()))
        return;
    json request = { {"op", "frame"}, {"tree", w.trees[static_cast<size_t>(w.module)].value("name", "")},
                     {"channel", w.channel}, {"depth", request_depth(m)} };
    for (auto it = fields.begin(); it != fields.end(); ++it)
        request[it.key()] = it.value();
    w.wantPosition = request.find("position") != request.end() ? request["position"].get<int64_t>() : -1;
    send(m, request);
}

void apply_waveform_answer(Model &m, const json &answer)
{
    auto &w = m.waves;
    if (!w.open || !answer.is_object() || answer.value("file", json()) != json(w.file))
        return;
    const auto id = answer.value("id", json());
    if (!id.is_number_unsigned() && !id.is_number_integer())
        return;
    const auto n = id.get<uint64_t>();
    if (n < w.firstId || n <= w.shownId)
        return;
    w.shownId = n;
    const auto error = answer.find("error");
    if (error != answer.end())
    {
        w.error = error->is_string() ? error->get<std::string>() : error->dump();
        pause(m);
        // The keys go on from the frame shown.
        if (n == w.lastId)
            w.wantPosition = -1;
        return;
    }
    w.error.clear();
    const auto trees = answer.find("trees");
    if (trees != answer.end())
    {
        w.trees = trees->is_array() ? *trees : json::array();
        // The first module with traces (an empty one would greet with "No traces").
        w.module = 0;
        for (size_t i = 0; i < w.trees.size(); ++i)
            if (w.trees[i].is_object() && w.trees[i].value("traces", json()).is_number()
                && w.trees[i]["traces"].get<double>() > 0)
            {
                w.module = static_cast<int>(i);
                break;
            }
        w.channel = nullptr;
        request_frame(m, { {"position", 0} });
        return;
    }
    w.frame = answer;
    if (n == w.lastId && answer.value("position", json()).is_number_integer())
        w.wantPosition = answer["position"].get<int64_t>();
}

void start_range(Model &m)
{
    auto &w = m.waves;
    const auto *tree = shown_tree(m);
    if (!w.open || !tree)
        return;
    // From the module's own numbers, else the range drawn.
    const auto own = w.own.find(tree->value("name", ""));
    const auto r = y_range(m);
    w.rangeText = own != w.own.end() && own->second.hi > own->second.lo
        ? std::to_string(own->second.lo) + " to " + std::to_string(own->second.hi)
        : std::to_string(r.lo) + " to " + std::to_string(r.hi);
    w.rangeError.clear();
    w.ranging = true;
    w.going = false;
}

void choose_typical(Model &m)
{
    auto &w = m.waves;
    const auto *tree = shown_tree(m);
    w.ranging = false;
    w.rangeError.clear();
    if (!tree)
        return;
    const auto own = w.own.find(tree->value("name", ""));
    if (own != w.own.end())
        own->second.on = false;
}

bool finish_range(Model &m)
{
    auto &w = m.waves;
    const auto *tree = shown_tree(m);
    int lo = 0, hi = 0;
    std::string why;
    if (!tree)
    {
        w.ranging = false;
        return false;
    }
    if (!parse_range(w.rangeText, lo, hi, why))
    {
        w.rangeError = why;
        return false;
    }
    w.own[tree->value("name", "")] = { lo, hi, true };
    w.ranging = false;
    w.rangeError.clear();
    return true;
}

}
