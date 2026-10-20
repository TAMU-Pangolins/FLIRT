#ifndef FLIRT_TUI_VIEW_PARTS_H
#define FLIRT_TUI_VIEW_PARTS_H

// The pieces flirt-tui's screen is drawn with (view*.cc).

#include <ftxui/dom/elements.hpp>
#include <string>
#include <vector>

#include "model.h"
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
// details, the log, the form; the browser, the help and the command have none.
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

// The boxes (view_boxes.cc): box 1, box 2, the bottom box.
ftxui::Element listfiles_box(const Model &m, const Layout &l, Edge edge, Hits *hits);
ftxui::Element options_box(const Model &m, const Layout &l, Edge edge, Hits *hits);
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
std::string key_bar_text(const Model &m, int columns);

}

#endif
