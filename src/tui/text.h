#ifndef FLIRT_TUI_TEXT_H
#define FLIRT_TUI_TEXT_H

// flirt-tui's text helpers: what may reach the terminal, and the formats of
// sizes, seconds and shortened names.

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace flirt::tui
{

// The text as it may be shown: invalid UTF-8 bytes and every C0 or C1 control
// character (newline and tab too) and DEL replaced by '?'. File names and
// messages go through it, so no escape sequence ever reaches the terminal.
std::string printable(const std::string &text);

// The terminal cells of printable text: one entry per cell, a wide character's
// second cell an empty string (ftxui::Utf8ToGlyphs).
std::vector<std::string> cells(const std::string &text);
int width(const std::string &text);

// Shortened in the middle to at most `max` cells, so the run number stays
// visible.
std::string mid_short(const std::string &text, int max);

// Shortened from the start, so the end of a path stays visible.
std::string start_short(const std::string &text, int max);

// The last `max` cells (a line being edited).
std::string tail_cells(const std::string &text, int max);

// Lines of at most `max` cells, broken at spaces; a word longer than a line
// is cut.
std::vector<std::string> wrap(const std::string &text, int max);

// 1000-based: "999 B", "1.5 kB", "12 MB".
std::string format_size(int64_t bytes);

// A run's seconds: "4.2 s", "2 min 13 s" (rounded
// first, so never "60.0 s" or "1 min 60 s"); empty for null.
std::string format_seconds(const nlohmann::json &seconds);

std::string base_name(const std::string &path);
std::string join_path(const std::string &dir, const std::string &name);

}

#endif
