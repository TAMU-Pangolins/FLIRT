#ifndef FLIRT_MESSAGE_BATCH_H
#define FLIRT_MESSAGE_BATCH_H

#include <vector>
#include <event_server/common/event_server_lib.h>
#include "mdpp-sampling/mdpp_decode.h"

namespace flirt
{

// The waveform traces of one module in one event: the event mvme's
// decoder produced from that module's data, as an MDPP sample decoder source
// moved it into its shared event (a2.cc:917-922) or FLIRT's own call returned it.
struct ModuleTraces
{
    int moduleIndex = -1;   // VME config module index within the message's event
    mesytec::mvme::mdpp_sampling::DecodedMdppSampleEvent decoded;
    // False: an analysis decoder's traces of a module without a tree, only
    // counted for the waveform config check.
    bool inFile = true;
};

// Per EventData message: the modules with at least one trace.
using EventTraces = std::vector<ModuleTraces>;

// The interface between the producer (replay, parse, extract: run_pipeline)
// and the consumer (ROOT output: RootWriter::handleBatch). Event-server
// messages in stream order: BeginRun alone, then the EventData messages of
// each readout buffer, then EndRun alone. Only the first `size` messages are
// part of the batch; the others keep their allocations for reuse.
//
// traces[i] belongs to messages[i]; it is only filled when waveform traces are
// collected, and next() empties it, so a recycled batch carries no stale traces.
struct MessageBatch
{
    std::vector<mvme::event_server::Message> messages;
    std::vector<EventTraces> traces;
    size_t size = 0;

    void clear() { size = 0; }

    // The slot for the next message; commit() adds it to the batch.
    mvme::event_server::Message &next()
    {
        if (size == messages.size())
        {
            messages.emplace_back();
            traces.emplace_back();
        }
        traces[size].clear();
        return messages[size];
    }

    // The traces of the slot next() returned.
    EventTraces &nextTraces() { return traces[size]; }

    void commit() { ++size; }
};

}

#endif
