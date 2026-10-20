#ifndef FLIRT_APPS_PROGRESS_H
#define FLIRT_APPS_PROGRESS_H

// Progress of a batch (flirt-convert's parent, batch.cc).
// A child writes its progress lines (--progress-fd) to a file the parent
// reads with ProgressReader.

#include <cstdint>
#include <string>
#include <sys/types.h>

namespace flirt
{

// A run's state: its last complete progress line.
struct ProgressState
{
    enum class Phase { Starting, Progress, Finishing };

    Phase phase = Phase::Starting;
    uint64_t processed = 0, total = 0;

    // floor(100 * processed / total), -1 unless Progress with total > 0.
    int percent() const;
};

// Reads a child's progress file with pread only, at its own offset: the
// child's descriptor shares the file offset. Does not own the descriptor.
class ProgressReader
{
    public:
        explicit ProgressReader(int fd = -1): fd_(fd) { }

        // Reads what was appended since the last call.
        void poll();
        const ProgressState &state() const { return state_; }

    private:
        int fd_;
        off_t offset_ = 0;
        std::string partial_;   // an incomplete last line
        ProgressState state_;
};

}

#endif
