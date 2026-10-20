#include "model.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>

#include "text.h"
#include "ui/files.h"

namespace flirt::tui
{

bool busy(const Model &m)
{
    return m.job.is_object() && (m.job.value("state", "") == "running" || m.job.value("state", "") == "stopping");
}

int checked_count(const Model &m)
{
    return static_cast<int>(std::count_if(m.files.begin(), m.files.end(), [](const FileEntry &f) { return f.checked; }));
}

const json *run_for(const Model &m, const std::string &path)
{
    if (!m.job.is_object() || m.job.find("runs") == m.job.end() || !m.job["runs"].is_array())
        return nullptr;
    for (const auto &run: m.job["runs"])
        if (run.value("input", "") == path)
            return &run;
    return nullptr;
}

std::string convert_label(const Model &m)
{
    if (m.job.is_object() && m.job.value("state", "") == "running")
        return "Stop";
    if (m.job.is_object() && m.job.value("state", "") == "stopping")
        return "Stopping...";
    const int n = checked_count(m);
    return n == 1 ? "Convert 1 run" : "Convert " + std::to_string(n) + " runs";
}

std::string convert_reason(const Model &m)
{
    if (busy(m))
        return {};
    if (m.files.empty())
        return "Add listfiles";
    if (!checked_count(m))
        return "Select at least one listfile";
    if (m.outputDir.empty())
        return "Choose an output folder";
    if (m.opts.analysis == "file" && m.opts.analysisPath.empty())
        return "Choose the .analysis file";
    return {};
}

namespace
{

// A whole number: empty is 0.
bool whole_number(const std::string &text, int64_t &value)
{
    if (text.empty())
    {
        value = 0;
        return true;
    }
    if (text.size() > 18 || text.find_first_not_of("0123456789") != std::string::npos)
        return false;
    value = std::strtoll(text.c_str(), nullptr, 10);
    return true;
}

}

json start_body(const Model &m, std::string &error)
{
    const auto &o = m.opts;
    json inputs = json::array();
    for (const auto &f: m.files)
        if (f.checked)
            inputs.push_back(f.path);
    int64_t jobs = 0, threads = 0, dither = 0;
    if (!whole_number(o.jobs, jobs) || !whole_number(o.threads, threads) || !whole_number(o.ditherOffset, dither))
    {
        error = "Advanced options: whole numbers only";
        return nullptr;
    }
    return {
        {"inputs", inputs}, {"outputDir", m.outputDir},
        {"analysis", o.analysis == "file" ? json({ {"mode", "file"}, {"path", o.analysisPath} })
                                          : json({ {"mode", o.analysis} })},
        {"waveforms", o.waveforms}, {"extractHitFlags", o.hitFlags}, {"officialWaveformBranches", o.official},
        {"jobs", jobs}, {"threads", threads}, {"acceptDamagedInput", o.acceptDamaged},
        {"experimentName", o.experimentName}, {"experimentTitle", o.experimentTitle}, {"ditherOffset", dither},
        {"compression", o.compression} };
}

bool is_view(Dialog d)
{
    return d == Dialog::Browser || d == Dialog::Help || d == Dialog::Command;
}

int chosen_box(const Model &m)
{
    return m.chosen;
}

void set_box(Model &m, int box)
{
    m.box = box;
    if (box == 1 || box == 2)
        m.chosen = box;
}

const FileEntry *selected_file(const Model &m)
{
    return m.fileRow >= 0 && m.fileRow < static_cast<int>(m.files.size()) ? &m.files[static_cast<size_t>(m.fileRow)]
                                                                          : nullptr;
}

const std::vector<Item> &form_items()
{
    static const std::vector<Item> items = {
        Item::Output, Item::AnalysisArchive, Item::AnalysisDefault, Item::AnalysisFile, Item::Waveforms,
        Item::HitFlags, Item::Official, Item::Damaged, Item::Compression, Item::Jobs, Item::Threads, Item::Name,
        Item::Title, Item::Dither };
    return items;
}

std::string next_compression(const std::string &name)
{
    const auto &all = compressions();
    for (size_t i = 0; i < all.size(); ++i)
        if (name == all[i].name)
            return all[(i + 1) % all.size()].name;
    return all.front().name;
}

bool focused(const Model &m, Item item)
{
    return m.focus == item;
}

bool is_number_field(Item item)
{
    return item == Item::Jobs || item == Item::Threads || item == Item::Dither;
}

bool is_text_field(Item item)
{
    return is_number_field(item) || item == Item::Name || item == Item::Title;
}

std::string &field_value(Model &m, Item item)
{
    switch (item)
    {
        case Item::Jobs: return m.opts.jobs;
        case Item::Threads: return m.opts.threads;
        case Item::Dither: return m.opts.ditherOffset;
        case Item::Name: return m.opts.experimentName;
        default: return m.opts.experimentTitle;
    }
}

const std::string &field_value(const Model &m, Item item)
{
    return field_value(const_cast<Model &>(m), item);
}

void add_files(Model &m, const std::vector<FileEntry> &entries)
{
    std::set<std::string> known;
    for (const auto &f: m.files)
        known.insert(f.id.empty() ? f.path : f.id);
    for (auto e: entries)
    {
        if (known.count(e.id) || known.count(e.path))
            continue;
        known.insert(e.id.empty() ? e.path : e.id);
        e.checked = true;
        m.files.push_back(e);
    }
}

Settings to_settings(const Model &m)
{
    Settings s;
    s.outputDir = m.outputDir;
    s.analysis = m.opts.analysis;
    s.analysisPath = m.opts.analysisPath;
    s.waveforms = m.opts.waveforms;
    s.hitFlags = m.opts.hitFlags;
    s.lastDir = m.lastDir;
    return s;
}

void apply_settings(Model &m, const Settings &s)
{
    m.outputDir = s.outputDir;
    m.opts.analysis = s.analysis;
    m.opts.analysisPath = s.analysisPath;
    m.opts.waveforms = s.waveforms;
    m.opts.hitFlags = s.hitFlags;
    m.lastDir = s.lastDir;
}

void load_report(Model &m, const std::string &path)
{
    if (m.reports.count(path) || m.reportErrors.count(path))
        return;
    std::string contents;
    ApiResult error;
    if (read_report(path, contents, error) != 200)
    {
        m.reportErrors[path] = error.body.value("error", "Cannot read the report");
        return;
    }
    std::vector<std::string> lines;
    try
    {
        const auto text = json::parse(contents).dump(2);
        size_t begin = 0, end;
        while ((end = text.find('\n', begin)) != std::string::npos)
        {
            lines.push_back(text.substr(begin, end - begin));
            begin = end + 1;
        }
        lines.push_back(text.substr(begin));
    }
    catch (const std::exception &e)
    {
        m.reportErrors[path] = std::string("Cannot read the report: ") + e.what();
        return;
    }
    m.reports[path] = lines;
}

}
