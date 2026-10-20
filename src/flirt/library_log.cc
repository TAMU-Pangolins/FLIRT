#include "flirt/library_log.h"

#include <mesytec-mvlc/util/logging.h>

namespace mvlc = mesytec::mvlc;

namespace flirt
{

void MessageSink::sink_it_(const spdlog::details::log_msg &msg)
{
    ++messages_.count;
    if (messages_.messages.size() >= keep_)
        return;
    std::string text(msg.logger_name.data(), msg.logger_name.size());
    if (!text.empty())
        text += ": ";
    text.append(msg.payload.data(), msg.payload.size());
    messages_.messages.push_back(std::move(text));
}

ScopedLoggerSinks::ScopedLoggerSinks(std::vector<std::shared_ptr<spdlog::logger>> loggers, spdlog::sink_ptr sink)
    : loggers_(std::move(loggers))
{
    for (const auto &logger: loggers_)
    {
        saved_.push_back(logger->sinks());
        logger->sinks() = { sink };
    }
}

ScopedLoggerSinks::~ScopedLoggerSinks()
{
    for (size_t i = 0; i < loggers_.size(); ++i)
        loggers_[i]->sinks() = saved_[i];
}

std::vector<std::shared_ptr<spdlog::logger>> replay_loggers()
{
    return { mvlc::get_logger("readout_parser"), mvlc::get_logger("event_builder"), mvlc::default_logger() };
}

}
