#ifndef FLIRT_LIBRARY_LOG_H
#define FLIRT_LIBRARY_LOG_H

// mesytec-mvlc's log messages, counted and kept instead of printed. Its
// loggers are made by create_logger with spdlog::stdout_color_mt
// (util/logging.cc:14-43), so the readout parser's warnings would reach
// stdout, before flirt-info's JSON or between flirt-convert's lines. For the
// replay, FLIRT points them at a MessageSink and restores their sinks after
// (ScopedLoggerSinks), without touching mesytec-mvlc or mvme code. The loggers
// must be taken through mesytec-mvlc's own functions: the shared library has
// its own spdlog registry.

#include <memory>
#include <mutex>
#include <spdlog/logger.h>
#include <spdlog/sinks/base_sink.h>
#include <string>
#include <vector>

namespace flirt
{

// A replay's library messages: how many, and the first ones' texts.
struct LibraryMessages
{
    size_t count = 0;
    std::vector<std::string> messages;
};

// The report and flirt-info keep the first 100 texts.
constexpr size_t KeptLibraryMessages = 100;

// Counts every message and keeps the first `keep` texts, each the message
// prefixed with its logger's name ("readout_parser: got an empty stack
// frame ..."; no date or level; the default logger's, which has no name,
// without a prefix). Locks per message (base_sink<std::mutex>).
class MessageSink: public spdlog::sinks::base_sink<std::mutex>
{
    public:
        explicit MessageSink(size_t keep = 0): keep_(keep) { }

        LibraryMessages result()
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return messages_;
        }

    protected:
        void sink_it_(const spdlog::details::log_msg &msg) override;
        void flush_() override { }

    private:
        const size_t keep_;
        LibraryMessages messages_;
};

// Points the loggers at the sink; the destructor gives them their own sinks back.
class ScopedLoggerSinks
{
    public:
        ScopedLoggerSinks(std::vector<std::shared_ptr<spdlog::logger>> loggers, spdlog::sink_ptr sink);
        ~ScopedLoggerSinks();
        ScopedLoggerSinks(const ScopedLoggerSinks &) = delete;
        ScopedLoggerSinks &operator=(const ScopedLoggerSinks &) = delete;

    private:
        std::vector<std::shared_ptr<spdlog::logger>> loggers_;
        std::vector<std::vector<spdlog::sink_ptr>> saved_;
};

// The loggers that write during a replay: mvlc::get_logger("readout_parser")
// (mvlc_readout_parser.cc, mvlc_readout_parser_util.cc),
// mvlc::get_logger("event_builder") and mvlc::default_logger()
// (event_builder.cc:185, 372, 403, 461 and event_builder2.cc use spdlog::warn).
std::vector<std::shared_ptr<spdlog::logger>> replay_loggers();

}

#endif
