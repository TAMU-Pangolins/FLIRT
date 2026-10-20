#ifndef FLIRT_TUI_INPUT_PARTS_H
#define FLIRT_TUI_INPUT_PARTS_H

// The pieces of flirt-tui's event handling input*.cc, mouse.cc and
// wave_input.cc share.

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

// The Waveforms tab's keys (wave_input.cc): the event being typed; the frame
// keys (Left/Right, PgUp/PgDn, Home/End, g; PgUp/PgDn the grid's pages
// while it is shown) of boxes 1, 2 and 3 and the full window; box 2's rows;
// `-` `+` `=` the speed and `z` the full window (play_key); Show, the pages;
// the own range typed (Enter keeps it if valid, Esc cancels), Y range's
// choice (own: its input line).
bool wave_going_event(Model &m, const ftxui::Event &e);
bool frame_key(Model &m, const ftxui::Event &e);
void step_frame(Model &m, int delta);
void start_going(Model &m);
void plot_option(Model &m, int row);
bool play_key(Model &m, const ftxui::Event &e);
void set_show(Model &m, bool grid);
bool grid_shown(const Model &m);
// The full window opened or closed (z, its button, Esc); over ssh the same
// frame asked for again when the depth to ask for changes.
void set_full(Model &m, bool full);
bool wave_range_event(Model &m, const ftxui::Event &e);
void choose_range(Model &m, bool own);
// The channel filter (null: all; else a sorted list), the same event (the play pauses).
void set_channels(Model &m, const json &filter);
int page_count(const Model &m);
void change_page(Model &m, int delta, bool wrap);
// The play bar: the frame under column `x` of a bar `width` cells wide
// starting at `x0` (a click or a drag; the play pauses).
void bar_to(Model &m, int x, int x0, int width);

// The full window's keys (wave_input.cc): Esc and z back to the boxes; ?,
// Tab, 1, 2, 3, [ ] back, then what they do; Space, - + =, the frame keys.
bool full_window_event(Model &m, const ftxui::Event &e, Actions &a);

// The channel picker (picker_input.cc): opened from box 2's Channels row;
// its keys and clicks.
void open_picker(Model &m);
bool picker_event(Model &m, const ftxui::Event &e);
void picker_toggle(Model &m, int channel);
void picker_button(Model &m, int button);

}

#endif
