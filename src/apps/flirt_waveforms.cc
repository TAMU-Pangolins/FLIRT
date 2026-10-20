// flirt-waveforms (docs/design.md, "TUI", "Waveform view";
// README "Events for programs"): reads FLIRT's <runId>_waveforms.root for
// flirt-tui's waveform view, in a process of its own, so that ROOT's signal
// handlers and messages never reach the screen and a damaged file cannot take
// the screen down. One JSON request per line on stdin, one JSON answer per
// line on stdout, flushed; exit 0 at the end of stdin. It only reads files.

#include <cstdio>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <unistd.h>

#include "common/utf8.h"
#include "waveform_reader.h"

int main(int argc, char *[])
{
    if (argc > 1)
    {
        std::cerr << "usage: flirt-waveforms (no arguments)\n"
                  << "Reads FLIRT waveforms files for flirt-tui: one JSON request per line on stdin,\n"
                  << "one JSON answer per line on stdout (README, \"Events for programs\").\n";
        return 2;
    }

    // The answers go to a copy of the original stdout; fd 1 then points at
    // stderr, so that stray output from a library cannot corrupt them.
    const int answersFd = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3);
    if (answersFd < 0 || dup2(STDERR_FILENO, STDOUT_FILENO) < 0)
    {
        std::perror("flirt-waveforms: cannot set up stdout");
        return 1;
    }
    FILE *answers = fdopen(answersFd, "w");
    if (!answers)
    {
        std::perror("flirt-waveforms: cannot set up stdout");
        return 1;
    }

    flirt::collect_root_messages();
    flirt::WaveformFileReader reader;
    using json = nlohmann::json;
    std::string line;
    while (std::getline(std::cin, line))
    {
        json answer;
        try
        {
            answer = reader.handle(json::parse(line));
        }
        catch (const std::exception &e)
        {
            answer = { {"id", nullptr}, {"error", std::string("not a JSON request: ") + e.what()} };
        }
        const auto text = flirt::dump_valid_utf8(answer) + "\n";
        if (std::fwrite(text.data(), 1, text.size(), answers) != text.size() || std::fflush(answers) != 0)
            return 1;
    }
    return 0;
}
