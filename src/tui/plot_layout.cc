// The waveform view's numbers: presets, the y range, the grid's layout, ticks
// and pages, the depth drawn and asked for (see plot_layout.h).

#include "plot_layout.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <regex>

#include "view.h"

namespace flirt::tui
{

const std::vector<int> &traces_presets()
{
    static const std::vector<int> presets = { 1, 2, 5, 10, 20, 50, 100 };
    return presets;
}

const std::vector<int> &speed_presets(bool ssh)
{
    static const std::vector<int> local = { 1, 2, 5, 10, 20, 50 }, remote = { 1, 2, 5, 10 };
    return ssh ? remote : local;
}

namespace
{

int preset(const std::vector<int> &presets, int index)
{
    return presets[static_cast<size_t>(std::clamp(index, 0, static_cast<int>(presets.size()) - 1))];
}

bool has_frame(const Waves &w)
{
    return w.frame.is_object() && w.frame.value("position", int64_t(-1)) >= 0;
}

// The channels a frame answer was asked for: empty for all.
std::vector<int> picked_channels(const json &channel)
{
    std::vector<int> out;
    if (channel.is_number_integer())
        out.push_back(channel.get<int>());
    else if (channel.is_array())
        for (const auto &c: channel)
            if (c.is_number_integer())
                out.push_back(c.get<int>());
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

int digits(int v)
{
    return static_cast<int>(std::to_string(v).size());
}

std::string format_ns(double ns)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%g", ns);
    return text;
}

}

int trace_channel(const json &trace)
{
    const auto it = trace.find("channel");
    return it != trace.end() && it->is_number_integer() ? it->get<int>() : 0;
}

int trace_age(const json &trace)
{
    const auto it = trace.find("age");
    return it != trace.end() && it->is_number_integer() ? it->get<int>() : 0;
}

const json &trace_samples(const json &trace)
{
    static const json none = json::array();
    const auto it = trace.find("samples");
    return it != trace.end() && it->is_array() ? *it : none;
}

int traces_setting(const Model &m)
{
    return preset(traces_presets(), m.tracesPreset);
}

int ssh_request_depth(int traces, int channels)
{
    return std::min(traces, std::max(1, SshMaxTraces / std::max(1, channels)));
}

int drawn_depth(const std::vector<int> &ages, int limit, bool ssh)
{
    limit = std::max(1, limit);
    if (!ssh)
        return limit;
    // counts[a]: the traces of age a.
    std::vector<int> counts(static_cast<size_t>(limit), 0);
    for (const int a: ages)
        if (a >= 0 && a < limit)
            ++counts[static_cast<size_t>(a)];
    int d = 1, total = counts[0];
    while (d < limit && total + counts[static_cast<size_t>(d)] <= SshMaxTraces)
        total += counts[static_cast<size_t>(d++)];
    return d;
}

PlotArea plot_area(const Model &m, int screenWidth, int screenHeight)
{
    PlotArea a;
    a.full = m.fullPlot;
    if (a.full)
    {
        // The whole screen one box over the key bar.
        a.width = std::max(4, screenWidth - 2);
        a.rows = std::max(4, screenHeight - 3);
        return a;
    }
    const auto l = layout(screenWidth, screenHeight);
    a.width = l.right - 2;
    a.rows = l.rightRows();
    return a;
}

// ---------- the y range ----------

double nice_step(double span, int n)
{
    const double raw = span / std::max(1, n);
    if (!(raw > 0))
        return 1;
    const double p = std::pow(10.0, std::floor(std::log10(raw)));
    for (const double k: { 1.0, 2.0, 5.0, 10.0 })
        if (k * p >= raw * (1 - 1e-12))
            return k * p;
    return 10 * p;
}

double next_step(double step)
{
    const double p = std::pow(10.0, std::floor(std::log10(step) + 1e-9));
    const double k = std::round(step / p);
    return k < 1.5 ? 2 * p : k < 3.5 ? 5 * p : 10 * p;
}

YRange round_range(double lo, double hi)
{
    const double step = nice_step(hi - lo, 4);
    // Whole numbers: step / 2 if whole, else the step, at least 1.
    double half = step / 2;
    if (half < 1 || half != std::floor(half))
        half = std::max(1.0, std::ceil(step));
    YRange r;
    r.lo = static_cast<int>(std::floor(lo / half) * half);
    r.hi = static_cast<int>(std::ceil(hi / half) * half);
    if (r.hi <= r.lo)
        r.hi = r.lo + static_cast<int>(half);
    return r;
}

YRange typical_range(const json &tree)
{
    const auto typical = tree.is_object() ? tree.value("typical", json()) : json();
    if (!typical.is_object() || !typical.value("low", json()).is_number() || !typical.value("high", json()).is_number())
    {
        YRange r;
        r.lo = 0;
        r.hi = 100;
        r.note = "No traces to measure: y 0 to 100";
        return r;
    }
    const double lo = typical["low"].get<double>(), hi = typical["high"].get<double>();
    if (hi <= lo)
    {
        // One value: a step around it.
        auto r = round_range(lo - 1, hi + 1);
        char text[96];
        std::snprintf(text, sizeof(text), "Every typical trace is flat at %g", lo);
        r.note = text;
        return r;
    }
    return round_range(lo, hi);
}

bool parse_range(const std::string &text, int &lo, int &hi, std::string &why)
{
    static const std::regex number("[-+]?[0-9]+");
    std::vector<long long> values;
    for (std::sregex_iterator it(text.begin(), text.end(), number), end; it != end; ++it)
    {
        if (it->str().size() > 10)
        {
            why = "The numbers are too large";
            return false;
        }
        values.push_back(std::strtoll(it->str().c_str(), nullptr, 10));
    }
    // Only the numbers, spaces, "to" and a comma.
    const auto other = std::regex_replace(text, std::regex("[-+]?[0-9]+|to|,|\\s"), "");
    if (values.size() != 2 || !other.empty())
    {
        why = "Two whole numbers, e.g. -50 to 350";
        return false;
    }
    if (values[0] >= values[1])
    {
        why = "The first number must be the lower";
        return false;
    }
    if (std::llabs(values[0]) > 1000000000 || std::llabs(values[1]) > 1000000000)
    {
        why = "The numbers are too large";
        return false;
    }
    lo = static_cast<int>(values[0]);
    hi = static_cast<int>(values[1]);
    return true;
}

const json *shown_tree(const Model &m)
{
    const auto &w = m.waves;
    std::string name;
    if (w.frame.is_object())
        name = w.frame.value("tree", "");
    for (const auto &t: w.trees)
        if (t.is_object() && !name.empty() && t.value("name", "") == name)
            return &t;
    if (w.module >= 0 && w.module < static_cast<int>(w.trees.size()))
        return &w.trees[static_cast<size_t>(w.module)];
    return nullptr;
}

bool own_chosen(const Model &m)
{
    const auto *tree = shown_tree(m);
    if (!tree)
        return false;
    const auto own = m.waves.own.find(tree->value("name", ""));
    return own != m.waves.own.end() && own->second.on;
}

YRange y_range(const Model &m)
{
    const auto *tree = shown_tree(m);
    if (own_chosen(m))
    {
        const auto &own = m.waves.own.at(tree->value("name", ""));
        YRange r;
        r.lo = own.lo;
        r.hi = own.hi;
        return r;
    }
    return typical_range(tree ? *tree : json());
}

int module_channels(const json &channels)
{
    int highest = -1;
    if (channels.is_array())
        for (const auto &c: channels)
            if (c.is_number_integer())
                highest = std::max(highest, c.get<int>());
    return highest < 0 ? 0 : (highest + 1 + 3) / 4 * 4;
}

int time_samples(const Model &m)
{
    const auto *tree = shown_tree(m);
    const auto typical = tree ? tree->value("typical", json()) : json();
    if (typical.is_object() && typical.value("samples", json()).is_number_integer())
        return std::max(2, typical["samples"].get<int>());
    size_t longest = 0;
    if (m.waves.frame.is_object())
        for (const auto &t: m.waves.frame.value("traces", json::array()))
            longest = std::max(longest, trace_samples(t).size());
    return std::max<int>(2, static_cast<int>(longest));
}

// ---------- the grid ----------

GridLayout grid_layout(const PlotArea &a, const YRange &y, int rows)
{
    GridLayout g;
    g.labelWidth = std::max(digits(y.lo), digits(y.hi)) + 1;
    g.gap = a.width >= 98 ? 2 : 1;
    g.pw = (a.width - 1 - g.labelWidth - 3 * g.gap) / 4 - 1;
    // Each grid row: its title, ph plot rows, the time axis; the time
    // numbers once under the page.
    const int room = a.region() - 1;
    g.fit = std::max(0, room / (3 + 2));
    g.fits = rows > 0 && g.pw >= 8 && g.fit >= 1;
    if (!g.fits)
        return g;
    g.pages = (rows + g.fit - 1) / g.fit;
    g.rowsPerPage = (rows + g.pages - 1) / g.pages;
    g.ph = room / g.rowsPerPage - 2;
    return g;
}

std::vector<Tick> y_ticks(const YRange &y, int ph)
{
    const double span = y.hi - y.lo;
    const int dots = std::max(1, 4 * ph - 1);
    std::vector<Tick> out;
    for (double step = std::max(1.0, nice_step(span, 4));; step = next_step(step))
    {
        out.clear();
        const auto s = static_cast<long long>(std::llround(step));
        const long long first = static_cast<long long>(std::ceil(y.lo / static_cast<double>(s))) * s;
        long long highest = first;
        while (highest + s <= y.hi)
            highest += s;
        for (long long v = highest; v >= first; v -= s)
        {
            const int row = static_cast<int>(std::lround((y.hi - v) / span * dots)) / 4;
            if (out.empty() || out.back().row != row)
                out.push_back({ row, static_cast<int>(v) });
        }
        bool apart = true;
        for (size_t i = 1; i < out.size(); ++i)
            apart = apart && out[i].row - out[i - 1].row >= 2;
        if (apart || step > span)
            return out;
    }
}

std::vector<TimeTick> time_ticks(int samples, int pw)
{
    const double last = (std::max(2, samples) - 1) * SamplePeriodNs;
    const double step = nice_step(last, 3);
    std::vector<TimeTick> out;
    for (int k = 0; k * step <= last * (1 + 1e-12); ++k)
    {
        const int column = static_cast<int>(std::lround(k * step / last * (2 * pw - 1))) / 2;
        if (out.empty() || out.back().column != column)
            out.push_back({ column, k * step });
    }
    return out;
}

std::string time_labels(int samples, int pw)
{
    std::string cell(static_cast<size_t>(1 + pw), ' ');
    const auto ticks = time_ticks(samples, pw);
    for (size_t i = 0; i < ticks.size(); ++i)
    {
        auto text = format_ns(ticks[i].ns);
        const int pos = 1 + ticks[i].column;
        if (i + 1 == ticks.size() && pos + static_cast<int>(text.size()) + 3 <= 1 + pw)
            text += " ns";
        const int end = pos + static_cast<int>(text.size());
        if (end > 1 + pw)
            continue;
        // Not touching the number before.
        if (cell.substr(static_cast<size_t>(std::max(0, pos - 1)), static_cast<size_t>(end - std::max(0, pos - 1)))
                .find_first_not_of(' ') != std::string::npos)
            continue;
        cell.replace(static_cast<size_t>(pos), text.size(), text);
    }
    return cell;
}

// ---------- what is drawn ----------

std::vector<int> grid_panels(const Model &m)
{
    const auto &w = m.waves;
    if (!has_frame(w) || !w.error.empty())
        return {};
    auto panels = picked_channels(w.frame.value("channel", json()));
    if (!panels.empty())
        return panels;
    const int n = module_channels(w.frame.value("channels", json::array()));
    for (int c = 0; c < n; ++c)
        panels.push_back(c);
    return panels;
}

Drawn drawn(const Model &m, const PlotArea &a)
{
    Drawn d;
    const auto &w = m.waves;
    d.gridChosen = m.grid;
    d.y = y_range(m);
    d.samples = time_samples(m);
    if (!has_frame(w) || !w.error.empty())
        return d;
    const auto found = w.frame.find("traces");
    if (found == w.frame.end() || !found->is_array())
        return d;
    const json &traces = *found;
    d.all = &traces;
    if (m.grid)
    {
        d.panels = grid_panels(m);
        const int n = static_cast<int>(d.panels.size());
        d.layout = grid_layout(a, d.y, (n + 3) / 4);
        d.grid = d.layout.fits;
        if (d.grid)
        {
            d.pages = d.layout.pages;
            d.page = std::clamp(w.page, 0, d.pages - 1);
            d.firstRow = d.page * d.layout.rowsPerPage;
            for (int i = d.firstRow * 4; i < n && i < (d.firstRow + d.layout.rowsPerPage) * 4; ++i)
                d.onScreen.push_back(d.panels[static_cast<size_t>(i)]);
        }
    }
    auto on_screen = [&](const json &t) {
        return !d.grid || std::find(d.onScreen.begin(), d.onScreen.end(), trace_channel(t)) != d.onScreen.end();
    };
    const int traceSetting = traces_setting(m);
    const int limit = std::min(w.frame.value("depth", 1), traceSetting);
    std::vector<int> ages;
    for (const auto &t: traces)
        if (on_screen(t))
            ages.push_back(trace_age(t));
    d.depth = drawn_depth(ages, limit, m.ssh);
    const int64_t position = w.frame.value("position", int64_t(0));
    d.lowered = m.ssh && d.depth < std::min<int64_t>(traceSetting, position + 1);
    for (size_t i = 0; i < traces.size(); ++i)
        if (on_screen(traces[i]) && trace_age(traces[i]) < d.depth)
            d.traces.push_back(i);
    return d;
}

std::vector<int> drawn_channels(const Drawn &d)
{
    std::vector<int> out;
    for (size_t i = 0; i < d.traces.size(); ++i)
        out.push_back(trace_channel(d.trace(i)));
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::string beyond_mark(const Drawn &d, const int *channel)
{
    bool any = false;
    int lo = 0, hi = 0;
    for (size_t i = 0; i < d.traces.size(); ++i)
    {
        const auto &t = d.trace(i);
        if (channel && trace_channel(t) != *channel)
            continue;
        for (const auto &v: trace_samples(t))
        {
            const int value = v.get<int>();
            lo = any ? std::min(lo, value) : value;
            hi = any ? std::max(hi, value) : value;
            any = true;
        }
    }
    if (any && hi > d.y.hi)
        return "▲" + std::to_string(hi);
    if (any && lo < d.y.lo)
        return "▼" + std::to_string(lo);
    return {};
}

int request_depth(const Model &m)
{
    const int traces = traces_setting(m);
    if (!m.ssh)
        return traces;
    const auto &w = m.waves;
    const auto d = drawn(m, plot_area(m, m.screenWidth, m.screenHeight));
    if (d.grid)
        return ssh_request_depth(traces, std::max<int>(1, static_cast<int>(d.onScreen.size())));
    int count = static_cast<int>(picked_channels(w.channel).size());
    if (count == 0)
    {
        // All: the module's channels (1 before any answer).
        const auto channels = w.frame.is_object() ? w.frame.value("channels", json::array()) : json::array();
        count = std::max<int>(1, static_cast<int>(channels.size()));
    }
    return ssh_request_depth(traces, count);
}

}
