#include "progress.h"

#include "batch_text.h"
#include "common/estimate.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <sys/ioctl.h>
#include <unistd.h>

namespace flirt
{

int ProgressState::percent() const
{
    if (phase != Phase::Progress || total == 0)
        return -1;
    const uint64_t p = processed >= total ? total : processed;
    return static_cast<int>(p * 100 / total);
}

double ProgressState::fraction() const
{
    switch (phase)
    {
        case Phase::Starting: return 0;
        case Phase::Finishing: return 1;
        case Phase::Progress: break;
    }
    if (total == 0)
        return 0;
    return processed >= total ? 1.0 : static_cast<double>(processed) / static_cast<double>(total);
}

std::string ProgressState::text() const
{
    switch (phase)
    {
        case Phase::Starting: return "starting";
        case Phase::Finishing: return "finishing";
        case Phase::Progress: break;
    }
    return total == 0 ? "running" : std::to_string(percent()) + "%";
}

void ProgressReader::poll()
{
    if (fd_ < 0)
        return;

    char buf[4096];
    ssize_t n;
    while ((n = pread(fd_, buf, sizeof(buf), offset_)) > 0)
    {
        offset_ += n;
        partial_.append(buf, static_cast<size_t>(n));
    }

    // The last complete line decides.
    const auto end = partial_.rfind('\n');
    if (end == std::string::npos)
        return;
    const std::string complete = partial_.substr(0, end);
    partial_.erase(0, end + 1);
    const auto begin = complete.rfind('\n');
    const std::string last = begin == std::string::npos ? complete : complete.substr(begin + 1);

    std::istringstream in(last);
    std::string word;
    in >> word;
    if (word == "finishing")
        state_ = { ProgressState::Phase::Finishing, 0, 0 };
    else if (word == "progress")
    {
        uint64_t processed = 0, total = 0;
        if (in >> processed >> total)
            state_ = { ProgressState::Phase::Progress, processed, total };
    }
}

namespace
{

bool stderr_is_terminal()
{
    const char *term = std::getenv("TERM");
    return isatty(2) && term && *term && std::strcmp(term, "dumb") != 0;
}

size_t terminal_width()
{
    struct winsize ws = {};
    if (ioctl(2, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
    return 80;
}

// Characters, one per UTF-8 sequence.
size_t text_columns(const std::string &text)
{
    size_t n = 0;
    for (unsigned char c: text)
        if ((c & 0xC0) != 0x80)
            ++n;
    return n;
}

// The first `columns` characters, one per UTF-8 sequence, never cut inside one.
std::string cut(const std::string &text, size_t columns)
{
    size_t count = 0;
    for (size_t i = 0; i < text.size(); ++i)
        if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80 && count++ == columns)
            return text.substr(0, i);
    return text;
}

void write_stderr(const std::string &text)
{
    std::cerr << text << std::flush;
}

}

ProgressDisplay::ProgressDisplay()
    : terminal_(stderr_is_terminal())
{
    // https://no-color.org: set and not empty turns colour off.
    const char *noColor = std::getenv("NO_COLOR");
    colour_ = terminal_ && !(noColor && *noColor);
}

ProgressDisplay::~ProgressDisplay()
{
    finish();
}

size_t ProgressDisplay::width() const
{
    return terminal_ ? terminal_width() : 80;
}

void ProgressDisplay::lines(const std::vector<std::string> &texts)
{
    std::string text;
    for (const auto &t: texts)
        text += t + "\n";
    if (!terminal_)
        write_stderr(text);
    else
    {
        // Clears the live line; the next update() or idle() draws it again
        // below the new lines (a run that ended has left it).
        write_stderr("\r\033[K" + text);
        drawn_ = false;
    }
}

void ProgressDisplay::update(const std::vector<Run> &runs, double elapsed)
{
    if (finished_ || !terminal_)
        return;

    std::vector<EstimateRun> estimated;
    std::vector<const Run *> running;
    for (const auto &run: runs)
    {
        const auto phase = run.phase == Run::Phase::Pending ? EstimateRun::Phase::Pending
            : run.phase == Run::Phase::Running ? EstimateRun::Phase::Running
            : run.phase == Run::Phase::Converted ? EstimateRun::Phase::Converted : EstimateRun::Phase::Ended;
        estimated.push_back({ phase, run.bytes, run.state.fraction() });
        if (run.phase == Run::Phase::Running)
            running.push_back(&run);
    }
    if (running.empty())
    {
        status_.clear();
        clear();
        return;
    }
    const double left = estimate_seconds(estimated, elapsed);
    const std::string suffix = left < 0 ? ""
        : std::string("   ") + (runs.size() > 1 ? "batch: " : "") + "about " + format_duration(left) + " left";

    // "● <run>   converting  ━━━━───  64%   about 2 s left", or
    // "● 3 converting: a 40%, b 20%, c 5%   batch: about 2 min left".
    const Painter paint(colour_);
    const size_t columns = terminal_width() - 1;
    if (running.size() == 1)
    {
        const auto &run = *running[0];
        const auto &state = run.state;
        const std::string head = "● " + run.runId + "   ";
        std::string word = state.phase == ProgressState::Phase::Starting ? "starting"
            : state.phase == ProgressState::Phase::Finishing ? "finishing" : "converting";
        const int percent = state.percent();
        std::string bar, rest, tail;
        if (percent >= 0)
        {
            tail = "  " + std::to_string(percent) + "%";
            const size_t used = text_columns(head + word + "  " + tail + suffix);
            const size_t barWidth = used + 10 <= columns ? std::min<size_t>(40, columns - used) : 0;
            const size_t filled = barWidth * static_cast<size_t>(percent) / 100;
            for (size_t i = 0; i < barWidth; ++i)
                (i < filled ? bar : rest) += i < filled ? "━" : "─";
        }
        const std::string plain = head + word + (bar.empty() && rest.empty() ? tail : "  " + bar + rest + tail) + suffix;
        status_ = text_columns(plain) > columns ? paint("●", Tone::Busy) + cut(plain, columns).substr(3)
            : paint("●", Tone::Busy) + " " + paint.bold(run.runId) + "   " + paint(word, Tone::Busy)
                + (bar.empty() && rest.empty() ? tail : "  " + paint(bar, Tone::Busy) + paint(rest, Tone::Dim) + tail)
                + suffix;
    }
    else
    {
        std::string items;
        for (auto run: running)
            items += (items.empty() ? "" : ", ") + run->runId + " " + run->state.text();
        const std::string plain = "● " + std::to_string(running.size()) + " converting: " + items + suffix;
        status_ = paint("●", Tone::Busy) + cut(plain, columns).substr(3);
    }

    if (!drawn_ || std::chrono::steady_clock::now() - lastDraw_ >= std::chrono::seconds(1))
        draw();
}

void ProgressDisplay::idle(const std::string &text)
{
    if (finished_ || !terminal_)
        return;
    status_ = cut(text, terminal_width() - 1);
    if (!drawn_ || std::chrono::steady_clock::now() - lastDraw_ >= std::chrono::seconds(1))
        draw();
}

void ProgressDisplay::finish()
{
    clear();
    finished_ = true;
}

void ProgressDisplay::draw()
{
    // status_ is cut to the width already (with colour codes it cannot be
    // cut by characters here).
    write_stderr("\r" + status_ + "\033[K");
    drawn_ = true;
    lastDraw_ = std::chrono::steady_clock::now();
}

void ProgressDisplay::clear()
{
    if (terminal_ && drawn_)
        write_stderr("\r\033[K");
    drawn_ = false;
}

}
