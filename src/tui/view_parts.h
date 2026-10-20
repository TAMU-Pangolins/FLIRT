#ifndef FLIRT_TUI_VIEW_PARTS_H
#define FLIRT_TUI_VIEW_PARTS_H

// The pieces flirt-tui's screen is drawn with (view*.cc, wave_view.cc).

#include <ftxui/dom/elements.hpp>
#include <string>
#include <vector>

#include "model.h"
#include "plot_layout.h"
#include "view.h"

namespace flirt::tui
{

// The terminal's own colours (its palette decides the shades): cyan for the
// focus and the primary action, magenta for the box the right box shows,
// grey for a selected row outside the focus.
inline const ftxui::Color Teal = ftxui::Color::Cyan;
inline const ftxui::Color Violet = ftxui::Color::Magenta;
inline const ftxui::Color Grey = ftxui::Color::GrayDark;
// The labels of box 3's details ("Run", "Lost", "Note"...), one per line; a
// line in warning or error keeps its level's colour for its label.
inline const ftxui::Color LabelBlue = ftxui::Color::BlueLight;

// Coloured by a level: ok, warning, error; heading bold, muted dim.
ftxui::Element styled(ftxui::Element e, const std::string &level);
ftxui::Element focus_style(ftxui::Element e, bool isFocused);
// Text that may hold names or messages (printable()).
ftxui::Element txt(const std::string &s);
// "[label]", the quit dialog's buttons.
ftxui::Element button(const std::string &label, bool isFocused, bool enabled = true, bool primary = false);
// " label " on a bar: cyan for the primary action, grey for the others.
ftxui::Element bar_button(const std::string &label, bool primary, bool enabled);

// `e` as the part `target` of `hits`, if any.
ftxui::Element hit(ftxui::Element e, Hits *hits, const Target &target);
// A key bar: a space, then entries joined by two spaces, the key names bold
// cyan and the words dim; with `hits`, an entry with one key name is its
// target, an entry with several has one target per key name (key_names()).
ftxui::Element key_bar_line(const std::string &keys, Hits *hits);
// An entry's leading key names ("Left/Right frame": Left, Right; "r/Esc hide": r, Esc; "↑↓ move":
// ↑, ↓; "[ ] tabs": [, ]; "? help": ?).
std::vector<std::string> key_names(const std::string &entry);

// A box's edge: the box you are in, the left box the right box shows, others;
// FocusThin: the box you were in while the quit dialog is open (cyan, thin:
// only the dialog is heavy).
enum class Edge { Dim, Focus, Chosen, FocusThin };
ftxui::Decorator edge_style(Edge edge);
// Box `box`'s edge (1, 2 or 3; view.cc): the focus, the left box the right
// box shows while the focus is elsewhere, the others.
Edge edge_of(const Model &m, int box);
// The left box whose content (or view) box 3 shows, 1 or 2, else 0: the
// details, the log, the form, the plot and the channel picker (the box it
// was opened from); the browser, the help and the command have none.
int source_box(const Model &m);
// A rule cell of an edge: heavy on the focused box's edge, else thin.
std::string edge_rule(Edge edge);
// A piece of an edge, with its width in cells.
struct Part
{
    ftxui::Element e;
    int width = 0;
};
Part part(const std::string &text, ftxui::Decorator style = ftxui::nothing);
// A w x h box with rounded edges, heavy ones (┏━┓┃┗┛) on the box with the
// focus: the title after "╭─", `right` before "─╮", `bottom` before "─╯";
// `inner` fills (w - 2) x (h - 2). (Not in an ftxui::frame: a frame gives a
// child exactly as high as itself one row more, FTXUI 6.1.9 Frame::SetBox.)
ftxui::Element frame_box(int w, int h, Edge edge, const std::vector<Part> &title, std::vector<Part> right,
                         std::vector<Part> bottom, ftxui::Element inner);
// "[3] Details ─ Log ": the number dim (on the focused box a badge,
// black on cyan), the active name bold in the edge's colour, the others dim;
// each name the target of the same index if `targets` has one. The number
// and the space after it are its first two parts.
std::vector<Part> box_title(const std::string &number, const std::vector<std::string> &names, int active, Edge edge,
                            Hits *hits = nullptr, const std::vector<Target> &targets = {});

// The selected row: of the box you are in, a solid cyan bar (black, bold);
// of another box, a grey bar.
enum class Bar { None, Focus, Grey };
ftxui::Element bar(ftxui::Element row, Bar b);
// `path` with the home folder as "~".
std::string tilde(const Model &m, const std::string &path);

// The boxes (view_boxes.cc): box 1's tabs, box 2's, the bottom box.
ftxui::Element listfiles_box(const Model &m, const Layout &l, Edge edge, Hits *hits);
ftxui::Element waveforms_box(const Model &m, const Layout &l, Edge edge, Hits *hits);
ftxui::Element options_box(const Model &m, const Layout &l, Edge edge, Hits *hits);
ftxui::Element plot_options_box(const Model &m, const Layout &l, Edge edge, Hits *hits);
ftxui::Element bottom_box(const Model &m, const Layout &l, Hits *hits);
// A listfile row's mark and right column (view_boxes.cc), from its run.
struct RowMark
{
    std::string mark, right;
    std::string level;      // ok, warning, error, busy, or empty (dim)
    bool rightDim = true;   // the size, seconds or "skipped"
};
RowMark row_mark(const FileEntry &f, const json *run);

// The right box (view_right.cc).
ftxui::Element right_box(const Model &m, const Layout &l, Edge edge, Hits *hits);

// The line views' contents and the browser (view_dialogs.cc).
std::vector<Line> detail_lines(const Model &m, int width);
std::vector<Line> log_lines(const Model &m, int width);
std::vector<Line> help_lines(const Model &m, int width);
std::vector<Line> command_lines(const Model &m, int width);
ftxui::Element browser_inner(const Model &m, int width, int height, Hits *hits);
std::string browser_title(const Model &m);
ftxui::Element quit_dialog(const Model &m, Hits *hits = nullptr);

// The key bar's text for what has the focus (view.cc).
// `rows`: the terminal's height, for the grid's pages (else the last known
// one).
std::string key_bar_text(const Model &m, int columns, int rows = -1);

// The plot (wave_view.cc): a channel's colour, the same in every frame; the
// module and filter drawn; the drawn traces' channels and "frame P of F";
// the right box's inside, `width` x `height` cells: the plot(s), the play
// bar and the controls.
ftxui::Color channel_color(int channel);
int shown_module(const Model &m);
json shown_channel(const Model &m);
std::vector<int> frame_channels(const Model &m, const PlotArea &a);
std::string frame_label(const Model &m);
ftxui::Element plot_inner(const Model &m, int width, int height, Hits *hits);
// A plot of the drawn traces (of one channel, or all) on these ranges,
// columns x rows cells of braille, each trace in `colour` (null: its
// channel's), older ones dim, cut at the edges; `zero`: a dim line at 0.
ftxui::Element trace_canvas(const Drawn &d, const int *channel, const PlotRanges &r, int columns, int rows,
                            const ftxui::Color *colour, bool zero);
// The grid (wave_grid.cc): the page's panels with their axes, a.width x the
// rows it needs; each row of four in its colour.
ftxui::Element grid_element(const Drawn &d, const PlotArea &a);
ftxui::Color row_color(int row);
// The grid's page on an edge: "ch 0-7 of 16" (`brief`: "0-7 of 16").
std::string page_label(const Drawn &d, bool brief = false);
// The pieces the full window shares (wave_view.cc): the plots in `rows` x
// `width` cells (the grid, or overlaid with its ssh note on the label row),
// the play bar row, the event's channels ("5, 6").
ftxui::Element plots_element(const Model &m, const Drawn &d, const PlotArea &a, Hits *hits);
ftxui::Element bar_row(const Model &m, int width, Hits *hits);
// The ssh note: "over ssh: last 3 events" or "over ssh: this event only";
// `brief`: "ssh: last 3", "ssh: 1 event".
std::string ssh_note(const Drawn &d, bool brief);
// The cells the ssh note takes on a row with `room` cells (its long form if
// it fits, else the short one; two spaces each side), 0 if none: optional
// words on its row are chosen from what is left.
int note_room(const Drawn &d, int room);
// A row of `width` cells: `left`, the ssh note (of `d`, if any) centred if it fits (the long
// form, else the short one, else none), `right`.
ftxui::Element note_row(const std::vector<Part> &left, const std::vector<Part> &right, const Drawn *d, int width);
// "5, 6" if it fits in `room` cells, else "8 picked" (`words`: "8 channels").
std::string channel_list(const std::vector<int> &channels, int room, const std::string &words);
// The frame's events per second label, and the play button (►, ‖).
std::string speed_label(const Model &m);
// Samples as nanoseconds: "5000.0 ns" (12.5 ns per sample).
std::string ns_label(double samples);
bool waves_have_frame(const Model &m);

// The full window (wave_full.cc; `z`): the whole screen over the key bar.
ftxui::Element full_window(const Model &m, int width, int height, Hits *hits);

// The channel picker in the right box (picker_view.cc).
ftxui::Element picker_inner(const Model &m, int width, int height, Hits *hits);

}

#endif
