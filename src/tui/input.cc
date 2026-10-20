#include "input.h"

#include <algorithm>
#include <climits>

#include "input_parts.h"
#include "text.h"
#include "view.h"
#include "view_parts.h"

namespace flirt::tui
{

using ftxui::Event;

namespace
{

bool same(const Settings &a, const Settings &b)
{
    return a.outputDir == b.outputDir && a.analysis == b.analysis && a.analysisPath == b.analysisPath
        && a.waveforms == b.waveforms && a.hitFlags == b.hitFlags && a.lastDir == b.lastDir;
}

void request_quit(Model &m, Actions &a)
{
    m.editing = false;
    m.browser.editing = false;
    if (busy(m))
    {
        m.dialog = Dialog::Quit;
        m.quitChoice = 0;
    }
    else
        a.quit();
}

void remove_file(Model &m, int row)
{
    if (busy(m) || row < 0 || row >= static_cast<int>(m.files.size()))
        return;
    m.files.erase(m.files.begin() + row);
    m.fileRow = m.fileCursor = std::max(0, std::min(row, static_cast<int>(m.files.size()) - 1));
}

// Up/Down, PgUp/PgDn (10), Home/End on a list of n rows; false if not one of them.
bool list_key(int &row, int &cursor, int n, const Event &e)
{
    int to = row;
    if (e == Event::ArrowDown) ++to;
    else if (e == Event::ArrowUp) --to;
    else if (e == Event::PageDown) to += 10;
    else if (e == Event::PageUp) to -= 10;
    else if (e == Event::Home) to = 0;
    else if (e == Event::End) to = n - 1;
    else
        return false;
    row = cursor = std::max(0, std::min(to, n - 1));
    return true;
}

// [ and ]: box 3's tabs, Details and Log, toggled; a box without tabs
// ignores them (one key rule for tabs; Left and Right never switch tabs).
void tab_key(Model &m)
{
    if (m.box == 3 && m.chosen == 1)
    {
        m.logTab = !m.logTab;
        if (m.logTab)
            m.logScroll = INT_MAX / 2;      // the end, clamped where it is drawn
    }
}

void show_log(Model &m)
{
    m.chosen = 1;
    m.box = 3;
    m.logTab = true;
    m.logScroll = INT_MAX / 2;      // the end, clamped where it is drawn
}

void open_help(Model &m)
{
    m.dialog = Dialog::Help;
    m.helpScroll = 0;
}

// The details' report: shown or hidden, and scrolled to it.
void toggle_report(Model &m, const Actions &a)
{
    const auto *f = selected_file(m);
    const auto *run = f ? run_for(m, f->path) : nullptr;
    if (!run || !run->value("report", json()).is_string())
        return;
    m.following = false;
    m.reportShown = !m.reportShown;
    if (m.reportShown)
        load_report(m, run->at("report").get<std::string>());
    const auto lines = detail_lines(m, layout(a.width, a.height).rightText());
    for (size_t i = 0; i < lines.size(); ++i)
        if (lines[i].text.rfind("Report", 0) == 0)
            m.detailScroll = static_cast<int>(i);
}

bool form_event(Model &m, const Event &e, Actions &a)
{
    const auto &items = form_items();
    const int at = static_cast<int>(std::find(items.begin(), items.end(), m.focus) - items.begin());
    auto to = [&](int i) { m.focus = items[static_cast<size_t>(std::clamp(i, 0, static_cast<int>(items.size()) - 1))]; };
    if (e == Event::ArrowDown) to(at + 1);
    else if (e == Event::ArrowUp) to(at - 1);
    else if (e == Event::Home) to(0);
    else if (e == Event::End) to(static_cast<int>(items.size()) - 1);
    else if (e == Event::Character(' ')) activate(m, a, false);
    else if (e == Event::Return) activate(m, a, true);
    else if (e == Event::Escape) set_box(m, 2);
    else
        return false;
    return true;
}

bool details_event(Model &m, const Event &e, Actions &a)
{
    // Esc hides a shown report first, then goes back.
    if (e == Event::Escape && m.reportShown && !m.logTab)
        toggle_report(m, a);
    else if (e == Event::Escape)
        set_box(m, 1);
    else if (e == Event::Character('r') && !m.logTab)
        toggle_report(m, a);
    else
    {
        const auto l = layout(a.width, a.height);
        if (scroll_lines(*right_scroll(m), e, static_cast<int>(right_lines(m, l.rightText()).size()), l.rightRows())
            && !m.logTab)
            m.following = false;
    }
    return true;
}

// The keys of the focused box; the global ones before them.
bool box_event(Model &m, const Event &e, Actions &a)
{
    if (m.editing)
    {
        bool done, cancelled;
        if (!edit_line(m.editText, e, is_number_field(m.focus), done, cancelled))
            return true;
        // Enter and Tab keep the text (Tab stays on the field), Esc cancels.
        if (done)
        {
            field_value(m, m.focus) = m.editText;
            m.editing = false;
            m.goError.clear();
        }
        else if (cancelled)
            m.editing = false;
        return true;
    }

    if (e == Event::Tab) set_box(m, m.box % 3 + 1);
    else if (e == Event::TabReverse) set_box(m, (m.box + 1) % 3 + 1);
    else if (e == Event::Character('1')) set_box(m, 1);
    else if (e == Event::Character('2')) set_box(m, 2);
    else if (e == Event::Character('3')) m.box = 3;
    else if (e == Event::Character('[') || e == Event::Character(']')) tab_key(m);
    else if (e == Event::Character('s')) go(m, a);
    else if (e == Event::Character('c'))
    {
        m.dialog = Dialog::Command;
        m.commandScroll = 0;
    }
    else if (e == Event::Character('l')) show_log(m);
    else if (e == Event::Character('?')) open_help(m);
    else if (e == Event::Character('a')) { if (!busy(m)) open_browser(m, "listfile"); }
    else if (e == Event::Character('o')) { if (!busy(m)) open_browser(m, "output"); }
    else if (m.box == 1)
    {
        if (list_key(m.fileRow, m.fileCursor, static_cast<int>(m.files.size()), e))
            m.following = false;
        else if (e == Event::Delete || e == Event::Backspace) remove_file(m, m.fileRow);
        else if (e == Event::Return)
        {
            m.box = 3;
            m.logTab = false;
        }
        else
            return false;
    }
    else if (m.box == 2)
    {
        if (e == Event::Return)
            m.box = 3;
        else
            return false;
    }
    else if (m.chosen == 2)
        return form_event(m, e, a);
    else
        return details_event(m, e, a);
    return true;
}

// Keys some terminals (tmux, the Linux console, rxvt) send differently from
// the sequences FTXUI names.
Event normalized(const Event &e)
{
    const auto &in = e.input();
    if (in == "\x1b[1~" || in == "\x1b[7~")
        return Event::Home;
    if (in == "\x1b[4~" || in == "\x1b[8~")
        return Event::End;
    if (in == "\x08")
        return Event::Backspace;
    return e;
}

// While following (a conversion started with s, the user has not moved or
// scrolled box 1's list or the details, nor shown or hidden a report since): each run that starts (its phase leaves "pending",
// job.cc) becomes the selected row, the last one in the plan's order if
// several started since the last look (-j N); the list keeps it in view.
void follow_runs(Model &m)
{
    if (!m.following || !m.job.is_object() || !m.job.value("runs", json()).is_array())
        return;
    std::string input;
    for (const auto &run: m.job["runs"])
    {
        const auto phase = run.value("phase", json());
        if (!phase.is_string() || phase == "pending" || phase == "ended"
            || !m.runsStarted.insert(run.value("runId", "")).second)
            continue;
        input = run.value("input", "");
    }
    for (size_t i = 0; !input.empty() && i < m.files.size(); ++i)
        if (m.files[i].path == input)
            m.fileRow = m.fileCursor = static_cast<int>(i);
}

// The details follow box 1's row while they are shown (Listfiles, box 1
// chosen, the Details tab).
void follow_details(Model &m)
{
    const auto *f = m.chosen == 1 && !m.logTab ? selected_file(m) : nullptr;
    if (!f)
    {
        return;
    }
    if (f->path != m.detailPath)
    {
        m.detailPath = f->path;
        m.detailScroll = 0;
        m.reportShown = false;
    }
}

}

void go(Model &m, Actions &a)
{
    m.goError.clear();
    if (m.job.is_object() && m.job.value("state", "") == "running")
    {
        a.cancel();
        return;
    }
    if (busy(m) || !convert_reason(m).empty())
        return;
    std::string error;
    const auto body = start_body(m, error);
    if (body.is_null())
    {
        m.goError = error;
        return;
    }
    error = a.start(body);
    if (!error.empty())
    {
        m.goError = error;
        return;
    }
    m.reports.clear();
    m.reportErrors.clear();
    m.reportShown = false;
    m.following = true;
    m.runsStarted.clear();
}

void activate(Model &m, Actions &, bool)
{
    if (busy(m))
        return;
    auto &o = m.opts;
    switch (m.focus)
    {
        case Item::Output:
            open_browser(m, "output");
            break;
        case Item::AnalysisArchive:
            o.analysis = "archive";
            break;
        case Item::AnalysisDefault:
            o.analysis = "default-filters";
            break;
        case Item::AnalysisFile:
        {
            // Choosing it without a file opens the browser; chosen again,
            // it changes the file.
            const bool again = o.analysis == "file";
            o.analysis = "file";
            if (again || o.analysisPath.empty())
                open_browser(m, "analysis");
            break;
        }
        case Item::Waveforms: o.waveforms = !o.waveforms; break;
        case Item::HitFlags: o.hitFlags = !o.hitFlags; break;
        case Item::Official: o.official = !o.official; break;
        case Item::Damaged: o.acceptDamaged = !o.acceptDamaged; break;
        case Item::Compression: o.compression = next_compression(o.compression); break;
        default:
            m.editing = true;
            m.editText = field_value(m, m.focus);
    }
}

void view_changes(Model &m, Dialog before)
{
    if (!is_view(before) && is_view(m.dialog))
    {
        m.back = m.box;
        m.box = 3;
    }
    else if (is_view(before) && !is_view(m.dialog))
    {
        if (m.back)
            m.box = m.back;
        m.back = 0;
    }
}

bool dispatch_key(Model &m, const Event &e, Actions &a)
{
    if (e == Event::CtrlC)
        request_quit(m, a);
    else if (e == Event::Character('q') && !m.editing && !m.browser.editing
             && m.dialog != Dialog::Quit)
        request_quit(m, a);
    else
        switch (m.dialog)
        {
            case Dialog::Browser: return browser_event(m, e, a);
            case Dialog::Quit: return quit_dialog_event(m, e, a);
            case Dialog::None: return box_event(m, e, a);
            default: return lines_view_event(m, e, a);
        }
    return true;
}

bool handle_event(Model &m, const Event &event, Actions &a)
{
    const auto e = normalized(event);
    const auto before = to_settings(m);
    const Dialog dialog = m.dialog;
    bool used = true;
    // Every mouse event is used, so FTXUI's own selection never starts
    // (ScreenInteractive::HandleSelection starts one on an unused left press).
    if (e.is_mouse())
        mouse_event(m, e, a);
    else
        used = dispatch_key(m, e, a);
    if (m.dialog != Dialog::Browser)
        m.browser.editing = false;
    view_changes(m, dialog);
    refresh(m, a);
    if (!same(before, to_settings(m)))
        a.save();
    return used;
}

void refresh(Model &m, Actions &)
{
    m.fileRow = std::max(0, std::min(m.fileRow, static_cast<int>(m.files.size()) - 1));
    follow_runs(m);
    follow_details(m);
}

}
