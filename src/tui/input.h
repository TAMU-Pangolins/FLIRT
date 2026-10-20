#ifndef FLIRT_TUI_INPUT_H
#define FLIRT_TUI_INPUT_H

// flirt-tui's event handling: a key or a mouse press -> a change of the
// state, and the few actions the program does for it (start, stop, quit,
// remember the settings).

#include <ftxui/component/event.hpp>
#include <cstdint>
#include <functional>
#include <string>

#include "info_queue.h"
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
    // The Waveforms tab: a request for flirt-waveforms (the latest replaces
    // one not sent yet), and no file shown any more (its reader stops).
    std::function<void(const json &request)> waveforms;
    std::function<void()> closeWaveforms;
    // The run information: flirt-info --json for these listfiles, the
    // cursor's row, the rows taken off, and whether a conversion runs (sent
    // after every event).
    std::function<void(const InfoRequests &requests)> info;
    int width = 80, height = 24;    // the terminal, for scrolling and mouse hits
    // Milliseconds of a steady clock, for double-clicks; unset: steady_clock.
    std::function<int64_t()> clockMs;
};

// Handles one event; true if it was used (Ctrl-C always is).
bool handle_event(Model &m, const ftxui::Event &event, Actions &actions);

// After an event or a new job state: the Waveforms tab's list and the file
// shown (its reader stopped when none is), the details following box 1's
// row, then the requests (refresh() sends them).
void refresh(Model &m, Actions &actions);
// Sends the waveform view's pending request, if any (after an event, and
// after an answer was applied: a tree list asks for a frame).
void send_waveform_request(Model &m, Actions &actions);
// Sends the run information requests (after an event).
void send_info_requests(Model &m, Actions &actions);

}

#endif
