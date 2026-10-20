#ifndef FLIRT_MESSAGES_LOG_H
#define FLIRT_MESSAGES_LOG_H

// Facts about the recording from the archive's messages.log (mvme's log of
// the run, settings_changes.h): the mvme version and system, and the MVLC's
// ETH settings. mvme logs them once per DAQ start; the first occurrence wins.
// An absent or unknown log gives unknowns, never an error. No Qt.

#include <cstdint>
#include <optional>
#include <string>

namespace flirt
{

struct MessagesLogFacts
{
    // "mvme 1.18.0 (64-bit) running on Debian GNU/Linux 12 (bookworm) (x86_64)"
    // (mvme_context.cc:1751-1755): the version, and the system with its CPU
    // architecture, "Debian GNU/Linux 12 (bookworm) (x86_64)".
    std::optional<std::string> mvmeVersion, system;
    // "  Enabling jumbo frame support" / "  Disabling ..." (mvlc_daq.cc:570-571).
    std::optional<bool> jumboFrames;
    // "  Eth receive buffer size: 208 kB" (mvlc_daq.cc:584-586), as logged and
    // in bytes: format_number with binary scaling and precision 0
    // (util/strings.cc:29-79), so the bytes are the logged value times the
    // prefix's power of 1024.
    std::optional<std::string> receiveBuffer;
    std::optional<uint64_t> receiveBufferBytes;
};

MessagesLogFacts read_messages_log_facts(const std::string &logText);

// "208 kB" -> 212992; none for text format_number cannot have written.
std::optional<uint64_t> parse_binary_size(const std::string &text);

}

#endif
