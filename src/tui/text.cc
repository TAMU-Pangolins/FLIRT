#include "text.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ftxui/screen/string.hpp>

#include "common/utf8.h"

namespace flirt::tui
{

std::string printable(const std::string &text)
{
    return printable_text(text);
}

std::vector<std::string> cells(const std::string &text)
{
    return ftxui::Utf8ToGlyphs(printable(text));
}

int width(const std::string &text)
{
    return static_cast<int>(cells(text).size());
}

namespace
{

std::string join(const std::vector<std::string> &c, size_t begin, size_t end)
{
    std::string out;
    for (size_t i = begin; i < end && i < c.size(); ++i)
        out += c[i];
    return out;
}

}

std::string mid_short(const std::string &text, int max)
{
    const auto c = cells(text);
    const size_t n = c.size();
    if (max < 3 || static_cast<int>(n) <= max)
        return max < 3 ? tail_cells(text, max) : join(c, 0, n);
    const size_t tail = static_cast<size_t>(std::ceil((max - 1) * 0.45));
    size_t head = static_cast<size_t>(max - 1) - tail;
    // A wide character is never cut in two.
    if (head > 0 && head < n && c[head].empty())
        --head;
    size_t from = n - tail;
    while (from < n && c[from].empty())
        ++from;
    return join(c, 0, head) + "…" + join(c, from, n);
}

std::string start_short(const std::string &text, int max)
{
    const auto c = cells(text);
    if (static_cast<int>(c.size()) <= max)
        return join(c, 0, c.size());
    return "…" + tail_cells(text, max - 1);
}

std::string tail_cells(const std::string &text, int max)
{
    const auto c = cells(text);
    if (max <= 0)
        return {};
    size_t from = c.size() > static_cast<size_t>(max) ? c.size() - static_cast<size_t>(max) : 0;
    while (from < c.size() && c[from].empty())
        ++from;
    return join(c, from, c.size());
}

std::vector<std::string> wrap(const std::string &text, int max)
{
    const auto c = cells(text);
    std::vector<std::string> lines;
    if (max < 2)
        max = 2;
    size_t begin = 0;
    while (begin < c.size())
    {
        size_t end = begin + static_cast<size_t>(max);
        if (end >= c.size())
        {
            lines.push_back(join(c, begin, c.size()));
            break;
        }
        // At the last space that fits; a word longer than the line (a path)
        // is cut.
        size_t space = end;
        while (space > begin && c[space] != " ")
            --space;
        if (space > begin)
        {
            lines.push_back(join(c, begin, space));
            begin = space + 1;
            continue;
        }
        if (c[end].empty())
            --end;      // keep a wide character whole
        lines.push_back(join(c, begin, end));
        begin = end;
    }
    if (lines.empty())
        lines.emplace_back();
    return lines;
}

std::string format_size(int64_t bytes)
{
    if (bytes < 1000)
        return std::to_string(bytes) + " B";
    static const char *const units[] = { "kB", "MB", "GB", "TB" };
    double b = static_cast<double>(bytes);
    int i = -1;
    do
    {
        b /= 1000;
        ++i;
    } while (b >= 1000 && i < 3);
    char text[32];
    if (b < 10)
        std::snprintf(text, sizeof(text), "%.1f %s", b, units[i]);
    else
        std::snprintf(text, sizeof(text), "%.0f %s", std::floor(b + 0.5), units[i]);
    return text;
}

std::string format_seconds(const nlohmann::json &seconds)
{
    if (!seconds.is_number())
        return {};
    const double s = seconds.get<double>();
    char text[48];
    // Rounded first: one decimal while that stays under 60, then whole seconds.
    const double tenths = std::floor(s * 10 + 0.5) / 10;
    if (tenths < 60)
        std::snprintf(text, sizeof(text), "%.1f s", tenths);
    else
    {
        const long whole = static_cast<long>(std::floor(s + 0.5));
        std::snprintf(text, sizeof(text), "%ld min %ld s", whole / 60, whole % 60);
    }
    return text;
}

std::string base_name(const std::string &path)
{
    const auto slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string join_path(const std::string &dir, const std::string &name)
{
    return (dir == "/" ? "/" : dir + "/") + name;
}

}
