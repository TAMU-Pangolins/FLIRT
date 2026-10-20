// flirt-tui's mouse (docs/design.md, "TUI", "Mouse"): a left press or the wheel
// -> what the key it stands for does. Every press renders the current state
// into an off-screen screen with its hit boxes, so the layout is always this
// state's and no terminal is needed.

#include <algorithm>
#include <chrono>
#include <climits>
#include <ftxui/screen/screen.hpp>

#include "input_parts.h"
#include "view.h"

namespace flirt::tui
{

using ftxui::Event;
using ftxui::Mouse;
using Kind = Target::Kind;

namespace
{

// Two presses on one target within this time are a double-click; a press
// this soon after one that changed the screen, at its cell, is ignored.
constexpr int64_t DoubleClickMs = 500;
constexpr int WheelStep = 3;

int64_t now_ms(const Actions &a)
{
    if (a.clockMs)
        return a.clockMs();
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// A key bar's key name as the key's event (view_parts.cc key_names()).
Event key_event(const std::string &name)
{
    if (name == "Enter") return Event::Return;
    if (name == "Esc") return Event::Escape;
    if (name == "Space") return Event::Character(' ');
    if (name == "Tab") return Event::Tab;
    if (name == "Del") return Event::Delete;
    if (name == "Backspace") return Event::Backspace;
    if (name == "Left") return Event::ArrowLeft;
    if (name == "Right") return Event::ArrowRight;
    if (name == "Up") return Event::ArrowUp;
    if (name == "Down") return Event::ArrowDown;
    if (name == "↑") return Event::ArrowUp;
    if (name == "↓") return Event::ArrowDown;
    if (name == "←") return Event::ArrowLeft;
    if (name == "→") return Event::ArrowRight;
    if (name == "PgUp") return Event::PageUp;
    if (name == "PgDn") return Event::PageDown;
    if (name == "Home") return Event::Home;
    if (name == "End") return Event::End;
    return Event::Character(name);
}

// A click goes there; a double-click is Enter: a
// listfile's details, box 2's options, a browser
// row (a folder opened, a file added or chosen).
bool has_double_click(const Model &m, const Target &t)
{
    if (t.kind == Kind::FileRow || t.kind == Kind::Summary)
        return true;
    return t.kind == Kind::BrowserRow && m.dialog == Dialog::Browser && t.index >= 0
        && t.index < static_cast<int>(m.browser.entries.size());
}

bool same_target(const Model::Press &p, const Target &t)
{
    return p.valid && p.kind == static_cast<int>(t.kind) && p.item == t.item && p.index == t.index;
}

// A click with no view open: the box under it gets the focus, then the part
// clicked acts as its key does.
void main_click(Model &m, const Target *t, int box, bool doubleClick, Actions &a)
{
    if (m.editing)
    {
        // The edited field itself: nothing. Elsewhere the edit ends as Enter
        // ends it (the text kept), then the click acts.
        if (t && t->kind == Kind::Item && t->item == m.focus)
            return;
        field_value(m, m.focus) = m.editText;
        m.editing = false;
        m.goError.clear();
    }
    if (box)
        set_box(m, box);
    if (!t)
        return;
    switch (t->kind)
    {
        case Kind::Tab:
            {
                m.logTab = t->index == 3;
                if (m.logTab)
                    m.logScroll = INT_MAX / 2;
            }
            break;
        case Kind::Add:
            if (!busy(m))
                open_browser(m, "listfile");
            break;
        case Kind::FileRow:
            m.fileRow = m.fileCursor = t->index;
            m.following = false;
            if (doubleClick)
            {
                m.box = 3;
                m.logTab = false;
            }
            break;
        case Kind::Summary:
            // A click: box 2, as the key 2 (set_box above); a double-click:
            // then Enter, the options.
            if (doubleClick)
                dispatch_key(m, Event::Return, a);
            break;
        case Kind::Item:
            m.focus = t->item;
            activate(m, a, false);
            break;
        case Kind::Convert:
            go(m, a);
            break;
        case Kind::CompressionChoice:
            // As the row's Space, straight to the name clicked.
            m.focus = Item::Compression;
            if (!busy(m))
                m.opts.compression = compressions().at(static_cast<size_t>(t->index)).name;
            break;
        default:
            break;
    }
}

// The browser: a row, its path, a button.
void browser_click(Model &m, const Target *t, bool doubleClick)
{
    auto &b = m.browser;
    if (b.editing)
    {
        // The edited line: nothing. Elsewhere the edit ends as Esc ends it.
        if (t && ((t->kind == Kind::BrowserPath && b.zone == Browser::Zone::Path) || t->kind == Kind::EditLine))
            return;
        b.editing = false;
        if (b.zone == Browser::Zone::NewFolder)
            b.zone = Browser::Zone::List;
    }
    if (!t)
        return;
    switch (t->kind)
    {
        case Kind::BrowserPath:
            b.zone = Browser::Zone::Path;
            b.editing = true;
            b.editText = b.path;
            break;
        case Kind::BrowserRow:
        {
            if (t->index < 0 || t->index >= static_cast<int>(b.entries.size()))
                break;
            b.zone = Browser::Zone::List;
            b.cursor = t->index;
            // A double-click is Enter; a click only moves there, and picks a
            // file as Space does (its row is a tick box).
            if (doubleClick)
                browser_activate(m, true);
            else if (b.entries[static_cast<size_t>(t->index)].type != "dir")
                browser_activate(m, false);
            break;
        }
        case Kind::BrowserButton:
            b.zone = Browser::Zone::Buttons;
            b.button = t->index;
            browser_button(m, t->index);
            break;
        default:
            break;
    }
}

const Target *target_at(const Model &m, int x, int y, Actions &a, Hits &hits, int &box)
{
    ftxui::Screen screen(a.width, a.height);
    ftxui::Render(screen, render(m, a.width, a.height, &hits));
    box = hits.box_at(x, y);
    return hits.at(x, y);
}

void click(Model &m, const Target *t, int box, int x, int y, bool doubleClick, Actions &a)
{
    // A key bar entry does what its key does, through the same code.
    if (t && t->kind == Kind::Key)
    {
        dispatch_key(m, key_event(t->key), a);
        return;
    }
    if (m.dialog == Dialog::Quit)
    {
        if (t && t->kind == Kind::QuitButton)
        {
            m.quitChoice = t->index;
            dispatch_key(m, Event::Return, a);
        }
        return;
    }
    if (is_view(m.dialog) && box == 3)
    {
        if (t && t->kind == Kind::Close)
            dispatch_key(m, Event::Escape, a);
        else if (m.dialog == Dialog::Browser)
            browser_click(m, t, doubleClick);
        return;
    }
    if (is_view(m.dialog))
    {
        // Outside the view: it closes as Esc would, then the click acts on
        // what is there now.
        const Dialog before = m.dialog;
        m.dialog = Dialog::None;
        m.browser.editing = false;
        view_changes(m, before);
        Hits hits;
        t = target_at(m, x, y, a, hits, box);
        if (t && t->kind == Kind::Key)
            return;     // the key bar changed with the view
        main_click(m, t, box, false, a);
        view_changes(m, Dialog::None);
        return;
    }
    main_click(m, t, box, doubleClick, a);
}

void scroll_by(int &offset, int delta, int lines, int rows)
{
    const int max = std::max(0, lines - rows);
    offset = std::max(0, std::min(std::max(0, std::min(offset, max)) + delta, max));
}

// A list's view scrolled without moving its selection: yframe keeps `cursor`
// centred (FTXUI Frame::SetBox), so its top row is
// clamp(cursor - (rows-1)/2, 0, n - rows).
void scroll_list(int &cursor, int n, const ftxui::Box &box, int direction)
{
    const int rows = box.y_max - box.y_min + 1;
    if (n <= rows)
        return;
    const int top = std::max(0, std::min(cursor - (rows - 1) / 2, n - rows));
    const int to = std::max(0, std::min(top + direction * WheelStep, n - rows));
    cursor = std::min(n - 1, to + (rows - 1) / 2);
}

void wheel(Model &m, const Mouse &mouse, Actions &a)
{
    if (m.editing || m.browser.editing || m.dialog == Dialog::Quit)
        return;
    const int direction = mouse.button == Mouse::WheelDown ? 1 : -1;
    Hits hits;
    int box = 0;
    target_at(m, mouse.x, mouse.y, a, hits, box);
    if (box == 1)
    {
        // The list under it: its view, not its selection (no more
        // following).
        if (const auto *list = hits.part_at(Kind::FileList, mouse.x, mouse.y))
        {
            scroll_list(m.fileCursor, static_cast<int>(m.files.size()), *list, direction);
            m.following = false;
        }
        return;
    }
    if (box != 3)
        return;
    const auto view = right_view(m);
    if (view == RightView::Browser)
    {
        auto &b = m.browser;
        const int n = static_cast<int>(b.entries.size());
        b.zone = Browser::Zone::List;
        b.cursor = n ? std::max(0, std::min(b.cursor + direction * WheelStep, n - 1)) : 0;
    }
    else if (auto *offset = right_scroll(m))
    {
        if (view == RightView::Details)
            m.following = false;
        const auto l = layout(a.width, a.height);
        scroll_by(*offset, direction * WheelStep, static_cast<int>(right_lines(m, l.rightText()).size()),
                  l.rightRows());
    }
}

}

void mouse_event(Model &m, const Event &event, Actions &a)
{
    Event e = event;    // Event::mouse() has no const overload
    const Mouse mouse = e.mouse();
    if (mouse.motion != Mouse::Pressed)
        return;
    if (mouse.button == Mouse::WheelUp || mouse.button == Mouse::WheelDown)
        return wheel(m, mouse, a);
    if (mouse.button != Mouse::Left)
        return;

    const int64_t now = now_ms(a);
    auto &last = m.lastPress;
    // What a changed screen put under the pointer is not clicked by the
    // second press of a double-click.
    if (last.changed && now - last.ms < DoubleClickMs && last.x == mouse.x && last.y == mouse.y)
        return;

    Hits hits;
    int box = 0;
    const Target *found = target_at(m, mouse.x, mouse.y, a, hits, box);
    const bool doubleClick = found && has_double_click(m, *found) && same_target(last, *found)
        && now - last.ms < DoubleClickMs;

    const Dialog dialog = m.dialog;
    const std::string folder = m.browser.path;
    click(m, found, box, mouse.x, mouse.y, doubleClick, a);

    last = Model::Press();
    // After a double-click the next press starts again.
    last.valid = found && !doubleClick;
    if (found)
    {
        last.kind = static_cast<int>(found->kind);
        last.item = found->item;
        last.index = found->index;
    }
    last.x = mouse.x;
    last.y = mouse.y;
    last.ms = now;
    last.changed = m.dialog != dialog
        || (dialog == Dialog::Browser && m.dialog == Dialog::Browser && m.browser.path != folder);
}

}
