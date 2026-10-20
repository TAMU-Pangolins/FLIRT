#include "batch_text.h"

namespace flirt
{

std::string Painter::operator()(const std::string &text, Tone tone) const
{
    if (!on_ || text.empty())
        return text;
    const char *code = "";
    switch (tone)
    {
        case Tone::Plain: return text;
        case Tone::Ok: code = "\033[32m"; break;
        case Tone::Skip: code = "\033[34m"; break;
        case Tone::Warning: code = "\033[33m"; break;
        case Tone::Error: code = "\033[31m"; break;
        case Tone::Busy: case Tone::Label: code = "\033[36m"; break;
        case Tone::Dim: code = "\033[2m"; break;
    }
    return code + text + "\033[0m";
}

std::string Painter::bold(const std::string &text) const
{
    return on_ ? "\033[1m" + text + "\033[0m" : text;
}

}
