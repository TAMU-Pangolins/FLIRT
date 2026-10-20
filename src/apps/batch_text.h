#ifndef FLIRT_APPS_BATCH_TEXT_H
#define FLIRT_APPS_BATCH_TEXT_H

// flirt-convert's human-readable text (README "What flirt-convert prints"):
// its colours.

#include <string>

namespace flirt
{

// The colours.
enum class Tone { Plain, Ok, Skip, Warning, Error, Busy, Label, Dim };

// Text with the terminal's colour codes, or as it is when off.
class Painter
{
    public:
        explicit Painter(bool on = false): on_(on) { }
        bool on() const { return on_; }
        std::string operator()(const std::string &text, Tone tone) const;
        std::string bold(const std::string &text) const;

    private:
        bool on_;
};

}

#endif
