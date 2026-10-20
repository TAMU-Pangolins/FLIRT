// flirt-tui's keys in the views of the right box (the file browser, the help,
// the command), the quit dialog, and an edited line.

#include <algorithm>

#include "input_parts.h"
#include "text.h"
#include "view.h"

namespace flirt::tui
{

using ftxui::Event;

namespace
{

// A typed character that may go into a field: printable, one glyph.
bool typed(const Event &e, std::string &c)
{
    if (!e.is_character())
        return false;
    c = e.character();
    return !c.empty() && printable(c) == c;
}

void erase_last_glyph(std::string &s)
{
    while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80)
        s.pop_back();
    if (!s.empty())
        s.pop_back();
}

void browser_move(Browser &b, int to)
{
    const int n = static_cast<int>(b.entries.size());
    b.cursor = n ? std::max(0, std::min(to, n - 1)) : 0;
}

}

// A line being edited: true if the event was one for it. `done` is set by
// Enter (and Tab), `cancelled` by Esc.
bool edit_line(std::string &text, const Event &e, bool digitsOnly, bool &done, bool &cancelled)
{
    done = cancelled = false;
    std::string c;
    if (e == Event::Return || e == Event::Tab)
        done = true;
    else if (e == Event::Escape)
        cancelled = true;
    else if (e == Event::Backspace)
        erase_last_glyph(text);
    else if (e == Event::CtrlU)
        text.clear();
    else if (typed(e, c))
    {
        if (digitsOnly && (c.size() != 1 || c[0] < '0' || c[0] > '9' || text.size() >= 18))
            return true;
        text += c;
    }
    else
        return false;
    return true;
}

bool scroll_lines(int &offset, const Event &e, int lines, int rows)
{
    const int max = std::max(0, lines - rows);
    offset = std::max(0, std::min(offset, max));
    bool used = true;
    if (e == Event::ArrowDown) ++offset;
    else if (e == Event::ArrowUp) --offset;
    else if (e == Event::PageDown || e == Event::Character(' ')) offset += std::max(1, rows - 1);
    else if (e == Event::PageUp) offset -= std::max(1, rows - 1);
    else if (e == Event::Home) offset = 0;
    else if (e == Event::End) offset = max;
    else
        used = false;
    offset = std::max(0, std::min(offset, max));
    return used;
}

bool lines_view_event(Model &m, const Event &e, Actions &a)
{
    if (e == Event::Escape || e == Event::Return || (m.dialog == Dialog::Help && e == Event::Character('?')))
    {
        m.dialog = Dialog::None;
        return true;
    }
    const auto l = layout(a.width, a.height);
    scroll_lines(*right_scroll(m), e, static_cast<int>(right_lines(m, l.rightText()).size()), l.rightRows());
    return true;
}

bool quit_dialog_event(Model &m, const Event &e, Actions &a)
{
    if (e == Event::ArrowLeft || e == Event::ArrowRight || e == Event::Tab || e == Event::TabReverse)
        m.quitChoice = 1 - m.quitChoice;
    else if (e == Event::Escape)
        m.dialog = Dialog::None;
    else if (e == Event::Return || e == Event::Character(' '))
    {
        if (m.quitChoice == 1)
            a.quit();
        else
            m.dialog = Dialog::None;
    }
    return true;
}

bool browser_event(Model &m, const Event &e, Actions &a)
{
    auto &b = m.browser;
    if (b.editing)
    {
        bool done, cancelled;
        if (!edit_line(b.editText, e, false, done, cancelled))
            return true;
        if (cancelled)
        {
            b.editing = false;
            if (b.zone == Browser::Zone::NewFolder)
                b.zone = Browser::Zone::List;
        }
        else if (done && e == Event::Return)
        {
            if (b.zone == Browser::Zone::NewFolder)
                browser_create_folder(m, b.editText);
            else
            {
                auto path = b.editText;
                path.erase(0, path.find_first_not_of(" \t"));
                path.erase(path.find_last_not_of(" \t") + 1);
                if (browse(m, path))
                {
                    b.editing = false;
                    b.zone = Browser::Zone::List;
                }
            }
        }
        return true;
    }

    const int buttons = static_cast<int>(browser_buttons(m).size());
    if (e == Event::Escape)
    {
        if (b.zone == Browser::Zone::NewFolder)
            b.zone = Browser::Zone::List;
        else
            m.dialog = Dialog::None;
        return true;
    }
    if (e == Event::Tab || e == Event::TabReverse)
    {
        // List, Path, then each button.
        const int stops = 2 + buttons;
        int at = b.zone == Browser::Zone::List ? 0 : b.zone == Browser::Zone::Path ? 1
            : b.zone == Browser::Zone::Buttons ? 2 + b.button : 0;
        at = ((at + (e == Event::Tab ? 1 : -1)) % stops + stops) % stops;
        b.zone = at == 0 ? Browser::Zone::List : at == 1 ? Browser::Zone::Path : Browser::Zone::Buttons;
        b.button = std::max(0, at - 2);
        return true;
    }
    if (e == Event::Character('/') || e == Event::Character('~'))
    {
        b.zone = Browser::Zone::Path;
        b.editing = true;
        b.editText = e.character() == "~" ? m.home + "/" : "/";
        return true;
    }
    if (e == Event::Backspace || (e == Event::ArrowLeft && b.zone == Browser::Zone::List))
    {
        if (b.hasParent)
            browse(m, b.parent);
        return true;
    }
    if (e == Event::Character('a') && b.mode == "listfile")
    {
        browser_add_all(m);
        return true;
    }
    if (e == Event::Character('n') && b.mode == "output")
    {
        browser_button(m, 1);
        return true;
    }
    switch (b.zone)
    {
        case Browser::Zone::Path:
            if (e == Event::Return)
            {
                b.editing = true;
                b.editText = b.path;
            }
            else if (e == Event::ArrowDown)
                b.zone = Browser::Zone::List;
            return true;
        case Browser::Zone::Buttons:
            if (e == Event::ArrowLeft)
                b.button = std::max(0, b.button - 1);
            else if (e == Event::ArrowRight)
                b.button = std::min(buttons - 1, b.button + 1);
            else if (e == Event::ArrowUp)
                b.zone = Browser::Zone::List;
            else if (e == Event::Return || e == Event::Character(' '))
                browser_button(m, b.button);
            return true;
        default:
            break;
    }
    const int page = std::max(1, layout(a.width, a.height).rightRows() - 6);
    if (e == Event::ArrowDown) browser_move(b, b.cursor + 1);
    else if (e == Event::ArrowUp) browser_move(b, b.cursor - 1);
    else if (e == Event::PageDown) browser_move(b, b.cursor + page);
    else if (e == Event::PageUp) browser_move(b, b.cursor - page);
    else if (e == Event::Home) browser_move(b, 0);
    else if (e == Event::End) browser_move(b, static_cast<int>(b.entries.size()) - 1);
    else if (e == Event::Return) browser_activate(m, true);
    else if (e == Event::ArrowRight)
    {
        if (b.cursor < static_cast<int>(b.entries.size()) && b.entries[static_cast<size_t>(b.cursor)].type == "dir")
            browser_activate(m, true);
    }
    else if (e == Event::Character(' '))
    {
        if (b.cursor < static_cast<int>(b.entries.size()) && b.entries[static_cast<size_t>(b.cursor)].type != "dir")
            browser_activate(m, false);
    }
    return true;
}

}
