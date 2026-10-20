#include "flirt/messages_log.h"

#include <regex>
#include <sstream>

namespace flirt
{

namespace
{

bool is_digit(char c) { return c >= '0' && c <= '9'; }

// mvme's "HH:MM:SS: " prefix (MVMEContext::logMessage, mvme_context.cc:1924-1940)
// removed; other lines unchanged.
std::string without_time_prefix(const std::string &line)
{
    if (line.size() >= 10 && is_digit(line[0]) && is_digit(line[1]) && line[2] == ':'
        && is_digit(line[3]) && is_digit(line[4]) && line[5] == ':' && is_digit(line[6])
        && is_digit(line[7]) && line[8] == ':' && line[9] == ' ')
        return line.substr(10);
    return line;
}

}

std::optional<uint64_t> parse_binary_size(const std::string &text)
{
    // format_number(value, "B", Binary, 0, 'f', 0): "<integer> <prefix>B",
    // prefixes "", k, M, G, T, P, E (util/strings.cc:29-79).
    static const std::regex re(R"(^(\d{1,6}) ([kMGTPE]?)B$)");
    std::smatch m;
    if (!std::regex_match(text, m, re))
        return std::nullopt;
    static const std::string prefixes = "kMGTPE";
    uint64_t bytes = std::stoull(m[1].str());
    if (m[2].length())
        for (size_t i = 0; i <= prefixes.find(m[2].str()[0]); ++i)
            bytes *= 1024;
    return bytes;
}

MessagesLogFacts read_messages_log_facts(const std::string &logText)
{
    MessagesLogFacts facts;
    static const std::regex versionRe(R"(^mvme (\S+) \([^)]*\) running on (.+)$)");
    static const std::string bufferPrefix = "  Eth receive buffer size: ";

    std::istringstream in(logText);
    std::string raw;
    while (std::getline(in, raw))
    {
        if (!raw.empty() && raw.back() == '\r')
            raw.pop_back();
        const auto line = without_time_prefix(raw);

        // The log's first line (mvme_context.cc:1751-1755); the first such line.
        std::smatch m;
        if (!facts.mvmeVersion && line.compare(0, 5, "mvme ") == 0 && std::regex_match(line, m, versionRe))
        {
            facts.mvmeVersion = m[1].str();
            facts.system = m[2].str();
        }
        else if (!facts.jumboFrames && line == "  Enabling jumbo frame support")
            facts.jumboFrames = true;
        else if (!facts.jumboFrames && line == "  Disabling jumbo frame support")
            facts.jumboFrames = false;
        else if (!facts.receiveBuffer && line.compare(0, bufferPrefix.size(), bufferPrefix) == 0)
        {
            facts.receiveBuffer = line.substr(bufferPrefix.size());
            facts.receiveBufferBytes = parse_binary_size(*facts.receiveBuffer);
        }
    }
    return facts;
}

}
