#ifndef FLIRT_COMMON_UTF8_H
#define FLIRT_COMMON_UTF8_H

#include <string>

namespace flirt
{

// The text with each byte that is not part of valid UTF-8 replaced by '?':
// nlohmann 3.3.0 throws on invalid UTF-8 when it writes JSON.
std::string valid_utf8(const std::string &text);

// The text as it may be shown on a terminal: valid_utf8, then every C0 or C1
// control character (newline and tab too) and DEL replaced by '?', so no
// escape sequence is printed (flirt-tui's printable(), src/tui/text.h).
std::string printable_text(const std::string &text);

// The JSON value with every string and every object key passed through
// valid_utf8 (docs/design.md, "Text that is not valid UTF-8"). A template on
// the json type, so that common/ needs no nlohmann of its own.
template <typename Json>
Json valid_utf8_json(const Json &value)
{
    if (value.is_string())
        return Json(valid_utf8(value.template get<std::string>()));
    if (value.is_array())
    {
        Json out = Json::array();
        for (const auto &element: value)
            out.push_back(valid_utf8_json(element));
        return out;
    }
    if (value.is_object())
    {
        Json out = Json::object();
        for (auto it = value.begin(); it != value.end(); ++it)
            out[valid_utf8(it.key())] = valid_utf8_json(it.value());
        return out;
    }
    return value;
}

// Every JSON text FLIRT writes or sends: the cleaned value's dump().
template <typename Json>
std::string dump_valid_utf8(const Json &value, int indent = -1)
{
    return valid_utf8_json(value).dump(indent);
}

}

#endif
