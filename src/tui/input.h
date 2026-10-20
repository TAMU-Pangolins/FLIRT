#ifndef FLIRT_TUI_INPUT_H
#define FLIRT_TUI_INPUT_H

// flirt-tui's event handling: a key or a mouse press -> a change of the
// state, and the few actions the program does for it (start, stop, quit,
// remember the settings).

#include <ftxui/component/event.hpp>
#include <cstdint>
#include <functional>
#include <string>

#include "model.h"

namespace flirt::tui
{

struct Actions
{
    // Starts a job with the start body; the error if it did not start.
    std::function<std::string(const json &body)> start;
    std::function<void()> cancel;
    std::function<void()> quit;
    std::function<void()> save;     // a remembered setting changed
    int width = 80, height = 24;    // the terminal, for scrolling and mouse hits
    // Milliseconds of a steady clock, for double-clicks; unset: steady_clock.
    std::function<int64_t()> clockMs;
};

// Handles one event; true if it was used (Ctrl-C always is).
bool handle_event(Model &m, const ftxui::Event &event, Actions &actions);

// After an event or a new job state: the details following box 1's
// row.
void refresh(Model &m, Actions &actions);

}

#endif
