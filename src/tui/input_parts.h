#ifndef FLIRT_TUI_INPUT_PARTS_H
#define FLIRT_TUI_INPUT_PARTS_H

// The pieces of flirt-tui's event handling input*.cc and mouse.cc share.

#include <ftxui/component/event.hpp>
#include <string>

#include "input.h"

namespace flirt::tui
{

// A line being edited: true if the event was one for it. `done` is set by
// Enter (and Tab), `cancelled` by Esc.
bool edit_line(std::string &text, const ftxui::Event &e, bool digitsOnly, bool &done, bool &cancelled);

// A key, as handle_event dispatches it (Ctrl-C, q, then the open view's or
// the focused box's keys); a key bar click sends its key through it.
bool dispatch_key(Model &m, const ftxui::Event &e, Actions &a);
// Enter (`enter`) or Space on the options form's focused item.
void activate(Model &m, Actions &a, bool enter);
// `s`: Convert, or Stop the running conversion.
void go(Model &m, Actions &a);
// A view opened or closed in the right box since `before`: the focus to it,
// or back where it was opened from.
void view_changes(Model &m, Dialog before);

// A mouse event (mouse.cc): a left press or the wheel; the rest do nothing.
void mouse_event(Model &m, const ftxui::Event &e, Actions &a);

// Up/Down, PgUp/PgDn, Home/End (and Space as PgDn) on lines of which `rows`
// fit; false if `e` is none of them.
bool scroll_lines(int &offset, const ftxui::Event &e, int lines, int rows);

// The views' keys (input_dialogs.cc).
bool lines_view_event(Model &m, const ftxui::Event &e, Actions &a);
bool quit_dialog_event(Model &m, const ftxui::Event &e, Actions &a);
bool browser_event(Model &m, const ftxui::Event &e, Actions &a);

}

#endif
