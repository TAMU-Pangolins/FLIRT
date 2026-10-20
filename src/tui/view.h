#ifndef FLIRT_TUI_VIEW_H
#define FLIRT_TUI_VIEW_H

// flirt-tui's drawing: a state -> an ftxui::Element, nothing else, so tests
// render the real screen into an ftxui::Screen without a terminal.

#include <deque>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>
#include <string>
#include <utility>
#include <vector>

#include "model.h"

namespace flirt::tui
{

// What a mouse press can hit (docs/design.md, "TUI", "Mouse").
struct Target
{
    enum class Kind
    {
        Box,            // box 1, 2 or 3 (`index`); every part inside it is smaller
        Tab,            // a tab name: 0 Listfiles, 1 Waveforms, 2 Details, 3 Log
        Add,            // box 1's "+ add" or "+ open"
        FileRow,        // a listfile row (`index`)
        FileList,       // the listfiles: its box gives the visible rows
        WaveRow,        // a waveforms file row (`index`)
        WaveList,       // the waveforms files
        Summary,        // box 2's options summary
        PlotOption,     // box 2's plot option row (`index`)
        Item,           // an options form item (`item`)
        Convert,        // Convert or Stop
        Close,          // "Esc close" on the right box's edge
        FramePrev,      // the plot's ‹
        FrameNext,      // the plot's ›
        GoTo,           // the plot's "go to"
        Key,            // a key name on the key bar (`key`)
        BrowserPath,    // the browser's path line
        BrowserRow,     // a browser entry (`index`)
        BrowserButton,  // a browser button (`index`)
        QuitButton,     // a quit dialog button (`index`)
        EditLine,       // the browser's new folder name, the event being typed
        // The waveform view:
        Full,           // " z full window " on the plot's edge (as z)
        Pages,          // the grid's "ch 0-7 of 16" on the edge (the next page, wrapping)
        Play,           // ► or ‖ (Space)
        Speed,          // the speed label (faster, wrapping)
        PlayBar,        // the play bar: the frame under the pointer, and a drag
        ShowChoice,     // box 2's Show: 0 grid, 1 overlaid
        PickerChannel,  // the channel picker's channel (`index`)
        PickerButton,   // its button (`index`: 0 All, 1 None, 2 Done)
        CompressionChoice, // a name on the form's Compression row (`index` in compressions())
        RangeChoice,    // box 2's Y range: 0 typical, 1 own (its input line)
    };
    Kind kind = Kind::Box;
    int index = 0;
    Item item = Item::Output;
    std::string key;

    Target() = default;
    Target(Kind k, int n = 0, Item i = Item::Output, std::string name = {})
        : kind(k), index(n), item(i), key(std::move(name)) {}
};

// The clickable parts of one rendered screen: FTXUI fills each box when the
// element is rendered (ftxui::reflect), clipped to the visible area, so a row
// scrolled out of a yframe gets an empty box. A deque: the boxes' addresses
// stay valid while parts are added.
struct Hits
{
    std::deque<std::pair<Target, ftxui::Box>> parts;
    ftxui::Decorator add(const Target &target);
    // The smallest part that holds the cell (a row wins over its box).
    const Target *at(int x, int y) const;
    // The box (1, 2, 3) that holds the cell, else 0.
    int box_at(int x, int y) const;
    // The first part of this kind that holds the cell, else null.
    const ftxui::Box *part_at(Target::Kind kind, int x, int y) const;
    // The first part of this kind drawn, wherever it is, else null.
    const ftxui::Box *first(Target::Kind kind) const;
};

// The screen's boxes for a terminal of width x height cells (docs/design.md,
// "TUI", "Layout"): the left column L = clamp(W*2/5, 42, 72) wide, box 1 H - 12
// rows high over box 2's 8; the right box W - L wide and H - 4 high; the bottom
// box 3 rows at full width; the key bar last. 80x24 is the smallest size
// designed for; below it the layout stays and FTXUI clips.
struct Layout
{
    int width = 80, height = 24;
    int left = 42, right = 38;      // the columns' widths
    int top = 20;                   // the left column's and the right box's height
    int box1 = 12, box2 = 8;
    int rightRows() const { return top - 2; }
    // The right box's text width, inside its edges and one-cell margins.
    int rightText() const { return right - 4; }
};
Layout layout(int width, int height);

// The whole screen; with `hits`, its clickable parts.
ftxui::Element render(const Model &m, int width, int height, Hits *hits = nullptr);

// What the right box shows.
enum class RightView { Details, Log, Form, Plot, Browser, Help, Command, Channels };
RightView right_view(const Model &m);

// The right box's lines where it shows lines (details, log, help, command)
// in `width` cells, and how many of them fit: the event handling scrolls with
// the same numbers.
struct Line
{
    std::string text;
    std::string level;  // ok, warning, error, heading, muted, or empty
    int label = 0;      // its first cells are a label, drawn blue (in warning, error: its colour)
    int run = 0;        // or its first cells are the Log's "<run>: ", drawn blue
};
std::vector<Line> right_lines(const Model &m, int width);
int *right_scroll(Model &m);

// "To convert the same way without flirt-tui, run this in a terminal on
// <host>" ("on <host>" left out when unknown): the terminal command's first
// line, and what flirt-tui prints with the command when it ends.
std::string command_intro(const std::string &host);

// A line of flirt-convert's output in the Log tab: its level (ok, warning,
// error, muted or empty) and how many leading cells are its run's name
// ("<run>: ", or a block header's "<mark> <run>"), drawn blue. `before` is
// the previous line's style: a block's why and to do rows take the level of
// its header, a wrapped line that of the line it continues.
struct LogStyle
{
    std::string level;
    int prefix = 0;
    std::string block;  // the level of the block header above
    std::string row;    // the level a wrapped continuation line takes
    int value = 0;      // a row's value column ("    <label>   <text>": its text's first cell), else 0
};
LogStyle log_style(const std::string &line, const LogStyle &before = {});

}

#endif
