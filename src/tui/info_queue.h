#ifndef FLIRT_TUI_INFO_QUEUE_H
#define FLIRT_TUI_INFO_QUEUE_H

// Which listfiles flirt-tui reads with flirt-info, and when (docs/design.md,
// "Reading several listfiles at once"): every row of box 1, the cursor's row
// first, at most the limit (src/common/cores.h) at once.
// Only the order and the counting live here; flirt_tui.cc runs the reads.

#include <deque>
#include <set>
#include <string>
#include <vector>

namespace flirt::tui
{

// What the model asks for after an event (send_info_requests).
struct InfoRequests
{
    std::vector<std::string> want;  // to read, in this order (the cursor's row first, then list order)
    std::string cursor;             // the row under the cursor ("" none)
    std::vector<std::string> drop;  // rows taken off the list: not read, a running read stopped
    bool busy = false;              // a conversion runs: no new read-ahead
};

class InfoQueue
{
    public:
        // Wanted paths join the queue's end (unless queued already); dropped
        // ones leave it. Returns the dropped paths that are running (to stop;
        // no longer counted).
        std::vector<std::string> apply(const InfoRequests &r);

        // The next read to start, marked running; "" for none. The cursor's
        // row starts at once, past the limit and while converting; the
        // others in queue order while fewer than `limit` run and no
        // conversion runs. A running read is never stopped for the limit.
        std::string next(int limit);

        // The cursor's row if it waits, marked running; "" otherwise.
        std::string next_cursor();

        // Whether a read-ahead could start with a limit of `most`.
        bool can_read_ahead(int most) const;

        // A read ended (not a dropped one).
        void finished(const std::string &path);

        const std::deque<std::string> &queued() const { return queue_; }
        size_t running() const { return running_.size(); }

    private:
        std::deque<std::string> queue_;
        std::multiset<std::string> running_;
        std::string cursor_;
        bool busy_ = false;
};

}

#endif
