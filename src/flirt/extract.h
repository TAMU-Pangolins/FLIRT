#ifndef FLIRT_EXTRACT_H
#define FLIRT_EXTRACT_H

#include <memory>
#include <mesytec-mvlc/mvlc_readout_parser.h>
#include <event_server/common/event_server_lib.h>

#include "flirt/analysis_config.h"
#include "flirt/vme_config.h"

namespace flirt
{

// Values mvme copies from RunInfo::infoDict into the BeginRun message
// (mvme_context.cc:1587-1594, 1812-1818; event_server.cc:451-455).
struct RunInfo
{
    std::string runId;
    std::string experimentName = "Experiment";
    std::string experimentTitle;
    std::string replaySourceFile;  // archive file name
    std::string listfileLogBuffer; // "messages.log" contents
};

// Runs the analysis data sources (liba2) on parsed readout events and encodes
// the results exactly as mvme's EventServer does, as protocol Messages.
class Extractor
{
    public:
        Extractor(const VMEConfig &vmeConfig, const AnalysisConfig &analysis);
        ~Extractor();

        // BeginRun message (EventServer::beginRun, event_server.cc:419-474).
        mvme::event_server::Message beginRunMessage(const RunInfo &runInfo) const;

        // Processes one parsed readout event. Returns false if EventServer
        // would not send an EventData message for it (event_server.cc:506-507).
        bool processEvent(int eventIndex,
                          const mesytec::mvlc::readout_parser::ModuleData *moduleDataList,
                          unsigned moduleCount,
                          mvme::event_server::Message &out);

        // EndRun message; mvme's EndRun info is informational only.
        mvme::event_server::Message endRunMessage() const;

        const mvme::event_server::EventDataDescriptions &eventDataDescriptions() const;
        const mvme::event_server::VMETree &vmeTree() const;

    private:
        struct Private;
        std::unique_ptr<Private> d;
};

}

#endif
