#include "view_parts.h"

#include <algorithm>
#include <ftxui/screen/string.hpp>

#include "text.h"

namespace flirt::tui
{

using namespace ftxui;

Element styled(Element e, const std::string &level)
{
    if (level == "ok") return e | color(Color::Green);
    if (level == "warning") return e | color(Color::Yellow);
    if (level == "error") return e | color(Color::Red);
    if (level == "heading") return e | bold;
    if (level == "muted") return e | dim;
    return e;
}

Element focus_style(Element e, bool isFocused)
{
    return isFocused ? e | bgcolor(Teal) | color(Color::Black) : e;
}

Element txt(const std::string &s)
{
    return text(printable(s));
}

Element button(const std::string &label, bool isFocused, bool enabled, bool primary)
{
    auto e = text("[" + label + "]");
    if (!enabled && !isFocused)
        e = e | dim;
    else if (primary && !isFocused)
        e = e | color(Teal) | bold;
    return focus_style(e, isFocused);
}

Element bar_button(const std::string &label, bool primary, bool enabled)
{
    auto e = text(" " + label + " ");
    if (!enabled)
        return e | bgcolor(Grey) | dim;
    return primary ? e | bgcolor(Teal) | color(Color::Black) | bold : e | bgcolor(Grey);
}

ftxui::Decorator Hits::add(const Target &target)
{
    // Empty until the element is rendered: a part never drawn holds no cell.
    parts.emplace_back(target, Box{ 0, -1, 0, -1 });
    return reflect(parts.back().second);
}

const Target *Hits::at(int x, int y) const
{
    const Target *best = nullptr;
    long bestArea = 0;
    for (const auto &[target, box]: parts)
    {
        if (!box.Contain(x, y))
            continue;
        const long area = static_cast<long>(box.x_max - box.x_min + 1) * (box.y_max - box.y_min + 1);
        if (!best || area < bestArea)
        {
            best = &target;
            bestArea = area;
        }
    }
    return best;
}

int Hits::box_at(int x, int y) const
{
    const auto *b = part_at(Target::Kind::Box, x, y);
    if (!b)
        return 0;
    for (const auto &[target, box]: parts)
        if (&box == b)
            return target.index;
    return 0;
}

const Box *Hits::part_at(Target::Kind kind, int x, int y) const
{
    for (const auto &[target, box]: parts)
        if (target.kind == kind && box.Contain(x, y))
            return &box;
    return nullptr;
}

const Box *Hits::first(Target::Kind kind) const
{
    for (const auto &[target, box]: parts)
        if (target.kind == kind && box.x_min <= box.x_max && box.y_min <= box.y_max)
            return &box;
    return nullptr;
}

Element hit(Element e, Hits *hits, const Target &target)
{
    return hits ? e | hits->add(target) : e;
}

namespace
{

bool is_arrow(const std::string &glyph)
{
    return glyph == "↑" || glyph == "↓" || glyph == "←" || glyph == "→";
}

}

std::vector<std::string> key_names(const std::string &entry)
{
    static const std::vector<std::string> named = { "Enter", "Esc", "Space", "Tab", "Del", "Backspace", "Left",
                                                    "Right", "Up", "Down", "PgUp", "PgDn", "Home", "End" };
    std::vector<std::string> names;
    size_t begin = 0;
    while (begin < entry.size())
    {
        auto end = entry.find(' ', begin);
        if (end == std::string::npos)
            end = entry.size();
        const auto word = entry.substr(begin, end - begin);
        const auto glyphs = ftxui::Utf8ToGlyphs(word);
        std::vector<std::string> parts;
        if (!glyphs.empty() && std::all_of(glyphs.begin(), glyphs.end(), is_arrow))
            parts = glyphs;     // "↑↓": one key each
        else if (word.size() == 1 && word[0] > ' ' && word[0] < 0x7f)
            parts = { word };
        else
        {
            size_t from = 0;
            for (;;)
            {
                const auto slash = word.find('/', from);
                parts.push_back(word.substr(from, slash == std::string::npos ? std::string::npos : slash - from));
                if (slash == std::string::npos)
                    break;
                from = slash + 1;
            }
            // Named keys, or one printable character ("r/Esc": r, Esc).
            for (const auto &p: parts)
                if (std::find(named.begin(), named.end(), p) == named.end()
                    && !(p.size() == 1 && p[0] > ' ' && p[0] < 0x7f))
                    return names;
        }
        names.insert(names.end(), parts.begin(), parts.end());
        begin = end + 1;
    }
    return names;
}

Element key_bar_line(const std::string &keys, Hits *hits)
{
    // The key names bold cyan, the rest dim.
    auto key = [](const std::string &name) { return text(name) | bold | color(Teal); };
    Elements parts = { text(" ") };
    size_t begin = 0;
    while (begin <= keys.size())
    {
        auto end = keys.find("  ", begin);
        if (end == std::string::npos)
            end = keys.size();
        const auto entry = keys.substr(begin, end - begin);
        const auto names = key_names(entry);
        Elements drawn;
        size_t at = 0;
        for (const auto &name: names)
        {
            const auto pos = entry.find(name, at);
            if (pos > at)
                drawn.push_back(text(entry.substr(at, pos - at)) | dim);
            // Several key names: each its own target, the words around them not.
            drawn.push_back(names.size() > 1 ? hit(key(name), hits, { Target::Kind::Key, 0, Item::Output, name })
                                             : key(name));
            at = pos + name.size();
        }
        if (at < entry.size())
            drawn.push_back(text(entry.substr(at)) | dim);
        // One key name: the whole entry is its target.
        parts.push_back(names.size() == 1 ? hit(hbox(drawn), hits, { Target::Kind::Key, 0, Item::Output, names[0] })
                                          : hbox(drawn));
        if (end == keys.size())
            break;
        parts.push_back(text("  "));
        begin = end + 2;
    }
    // Wider than the screen: cut at the right edge, as one text line would
    // be, rather than FTXUI shrinking every entry.
    return hbox(parts) | xframe;
}

Decorator edge_style(Edge edge)
{
    if (edge == Edge::Focus || edge == Edge::FocusThin)
        return color(Teal);
    if (edge == Edge::Chosen)
        return color(Violet);
    return dim;
}

Part part(const std::string &s, Decorator style)
{
    return { text(printable(s)) | style, width(s) };
}

namespace
{

int parts_width(const std::vector<Part> &parts)
{
    int n = 0;
    for (const auto &p: parts)
        n += p.width;
    return n;
}

std::string rule(int n, Edge edge)
{
    std::string s;
    for (int i = 0; i < n; ++i)
        s += edge_rule(edge);
    return s;
}

}

std::string edge_rule(Edge edge)
{
    return edge == Edge::Focus ? "━" : "─";
}

Element frame_box(int w, int h, Edge edge, const std::vector<Part> &title, std::vector<Part> right,
                  std::vector<Part> bottom, Element inner)
{
    w = std::max(w, 4);
    h = std::max(h, 2);
    const auto es = edge_style(edge);
    // The box with the focus: heavy lines (the focus moving between boxes 1
    // and 2 was easy to miss).
    const bool heavy = edge == Edge::Focus;
    const std::string r = edge_rule(edge);
    // ╭─ title ─── right ─╮
    int fill = w - 3 - parts_width(title) - (right.empty() ? 0 : parts_width(right) + 1);
    if (fill < 0)
    {
        right.clear();
        fill = w - 3 - parts_width(title);
    }
    Elements top = { text(heavy ? "┏━" : "╭─") | es };
    for (const auto &p: title)
        top.push_back(p.e);
    top.push_back(text(rule(std::max(0, fill), edge)) | es);
    for (const auto &p: right)
        top.push_back(p.e);
    if (!right.empty())
        top.push_back(text(r) | es);
    top.push_back(text(heavy ? "┓" : "╮") | es);
    // ╰─── bottom ─╯
    int bottomFill = w - 2 - (bottom.empty() ? 0 : parts_width(bottom) + 1);
    if (bottomFill < 0)
    {
        bottom.clear();
        bottomFill = w - 2;
    }
    Elements low = { text(heavy ? "┗" : "╰") | es, text(rule(bottomFill, edge)) | es };
    for (const auto &p: bottom)
        low.push_back(p.e);
    if (!bottom.empty())
        low.push_back(text(r) | es);
    low.push_back(text(heavy ? "┛" : "╯") | es);
    // Each side its own elements: an element drawn twice is drawn once.
    auto side = [&] {
        Elements cells;
        for (int i = 0; i < h - 2; ++i)
            cells.push_back(text(heavy ? "┃" : "│") | es);
        return vbox(cells);
    };
    return vbox({ hbox(top) | xframe | size(WIDTH, EQUAL, w),
                  hbox({ side(), inner | size(WIDTH, EQUAL, w - 2) | size(HEIGHT, EQUAL, h - 2), side() }),
                  hbox(low) | xframe | size(WIDTH, EQUAL, w) })
        | size(WIDTH, EQUAL, w) | size(HEIGHT, EQUAL, h);
}

std::vector<Part> box_title(const std::string &number, const std::vector<std::string> &names, int active, Edge edge,
                            Hits *hits, const std::vector<Target> &targets)
{
    const auto es = edge_style(edge);
    // The focused box's number a badge, black on cyan as focus_style().
    std::vector<Part> out = { edge == Edge::Focus ? part(number, [](Element e) { return focus_style(e, true); })
                                                  : part(number, dim),
                              part(" ") };
    for (size_t i = 0; i < names.size(); ++i)
    {
        if (i)
            out.push_back(part(" " + edge_rule(edge) + " ", es));
        auto p = static_cast<int>(i) == active
            ? part(names[i], edge == Edge::Dim ? bold : bold | edge_style(edge)) : part(names[i], dim);
        if (i < targets.size())
            p.e = hit(p.e, hits, targets[i]);
        out.push_back(p);
    }
    out.push_back(part(" ", es));
    return out;
}

Element bar(Element row, Bar b)
{
    if (b == Bar::Focus)
        return row | bgcolor(Teal) | color(Color::Black) | bold;
    if (b == Bar::Grey)
        return row | bgcolor(Grey);
    return row;
}

std::string tilde(const Model &m, const std::string &path)
{
    if (!m.home.empty() && m.home != "/" && (path == m.home || path.rfind(m.home + "/", 0) == 0))
        return "~" + path.substr(m.home.size());
    return path;
}

}
