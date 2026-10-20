#ifndef FLIRT_MESSAGE_BATCH_H
#define FLIRT_MESSAGE_BATCH_H

#include <vector>
#include <event_server/common/event_server_lib.h>

namespace flirt
{

// The interface between the producer (replay, parse, extract: run_pipeline)
// and the consumer (ROOT output: RootWriter::handleBatch). Event-server
// messages in stream order: BeginRun alone, then the EventData messages of
// each readout buffer, then EndRun alone. Only the first `size` messages are
// part of the batch; the others keep their allocations for reuse.
struct MessageBatch
{
    std::vector<mvme::event_server::Message> messages;
    size_t size = 0;

    void clear() { size = 0; }

    // The slot for the next message; commit() adds it to the batch.
    mvme::event_server::Message &next()
    {
        if (size == messages.size())
        {
            messages.emplace_back();
        }
        return messages[size];
    }

    void commit() { ++size; }
};

}

#endif
