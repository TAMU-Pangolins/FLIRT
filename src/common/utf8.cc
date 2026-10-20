#include "utf8.h"

namespace flirt
{

std::string valid_utf8(const std::string &text)
{
    std::string out;
    const auto *p = reinterpret_cast<const unsigned char *>(text.data());
    const size_t n = text.size();
    for (size_t i = 0; i < n;)
    {
        const unsigned c = p[i];
        size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
        bool ok = len > 0 && i + len <= n && !(len == 2 && c < 0xC2) && !(len == 4 && c > 0xF4);
        for (size_t k = 1; ok && k < len; ++k)
            ok = (p[i + k] & 0xC0) == 0x80;
        if (ok && len == 3)
            ok = !(c == 0xE0 && p[i + 1] < 0xA0) && !(c == 0xED && p[i + 1] >= 0xA0);
        if (ok && len == 4)
            ok = !(c == 0xF0 && p[i + 1] < 0x90) && !(c == 0xF4 && p[i + 1] >= 0x90);
        if (ok)
        {
            out.append(text, i, len);
            i += len;
        }
        else
        {
            out += '?';
            ++i;
        }
    }
    return out;
}

std::string printable_text(const std::string &text)
{
    const auto valid = valid_utf8(text);
    std::string out;
    out.reserve(valid.size());
    for (size_t i = 0; i < valid.size();)
    {
        const auto c = static_cast<unsigned char>(valid[i]);
        const size_t len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        // C1 controls are U+0080..U+009F: 0xC2 0x80..0x9F in UTF-8.
        const bool control = c < 0x20 || c == 0x7F
            || (c == 0xC2 && static_cast<unsigned char>(valid[i + 1]) < 0xA0);
        if (control)
            out += '?';
        else
            out.append(valid, i, len);
        i += len;
    }
    return out;
}

}
