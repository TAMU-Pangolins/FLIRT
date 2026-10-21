// "Find from a file..." in the options form (docs/design.md, "TUI"):
// flirt-tui runs `flirt-convert
// --find-dither-offset --json-events <file>` (src/ui/dither_find.cc) and
// applies its events here (docs/usage.md "Events for programs"); it never
// opens
// the ROOT file itself.

#include <cmath>

#include "model.h"

namespace flirt::tui
{

void find_dither(Model &m, const std::string &file)
{
    auto &f = m.find;
    f.running = true;
    ++f.id;
    f.percent = 0;
    f.answered = false;
    f.note.clear();
    f.error.clear();
    f.outbox = file;
}

void stop_find(Model &m)
{
    auto &f = m.find;
    if (!f.running)
        return;
    f.running = false;
    f.outbox.clear();
    f.note = "Search stopped; the dither offset is unchanged";
    // Its later events are another search's.
    ++f.id;
}

void apply_find_event(Model &m, uint64_t id, const json &event)
{
    auto &f = m.find;
    if (id != f.id || !f.running || !event.is_object())
        return;
    const auto kind = event.value("event", "");
    if (kind == "progress")
    {
        const double done = event.value("done", 0.0), total = event.value("total", 0.0);
        if (total > 0)
            f.percent = static_cast<int>(std::floor(100 * done / total));
    }
    else if (kind == "dither-offset")
    {
        f.answered = true;
        const auto offset = event.value("offset", json());
        if (offset.is_number_unsigned() || offset.is_number_integer())
        {
            m.opts.ditherOffset = offset.dump();
            if (event.value("heldTwice", false))
                f.note = "The file holds the run twice: this is the first copy's offset";
        }
        else
        {
            const auto problem = event.value("problem", json());
            f.error = "No dither offset: " + (problem.is_string() ? problem.get<std::string>() : std::string("unknown"));
        }
    }
    else if (kind == "usage-error")
    {
        f.answered = true;
        f.error = "No dither offset: " + event.value("message", std::string("flirt-convert refused the file"));
    }
    else if (kind == "end" || kind == "exit")
    {
        // "exit": the helper's process ended (dither_find.cc), with or without "end".
        f.running = false;
        if (!f.answered)
            f.error = kind == "end" ? "No dither offset: flirt-convert gave no answer (exit code "
                    + std::to_string(event.value("exitCode", -1)) + ")"
                : "No dither offset: " + event.value("error", std::string("flirt-convert stopped unexpectedly"));
    }
}

}
