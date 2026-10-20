#include "info_queue.h"

#include <algorithm>

namespace flirt::tui
{

std::vector<std::string> InfoQueue::apply(const InfoRequests &r)
{
    for (const auto &path: r.want)
        if (std::find(queue_.begin(), queue_.end(), path) == queue_.end())
            queue_.push_back(path);
    std::vector<std::string> stop;
    for (const auto &path: r.drop)
    {
        queue_.erase(std::remove(queue_.begin(), queue_.end(), path), queue_.end());
        if (running_.count(path))
        {
            running_.erase(path);
            stop.push_back(path);
        }
    }
    cursor_ = r.cursor;
    busy_ = r.busy;
    return stop;
}

std::string InfoQueue::next(int limit)
{
    auto found = std::find(queue_.begin(), queue_.end(), cursor_);
    if (cursor_.empty() || found == queue_.end())
    {
        if (busy_ || queue_.empty() || running_.size() >= static_cast<size_t>(std::max(1, limit)))
            return {};
        found = queue_.begin();
    }
    const auto path = *found;
    queue_.erase(found);
    running_.insert(path);
    return path;
}

std::string InfoQueue::next_cursor()
{
    const auto found = std::find(queue_.begin(), queue_.end(), cursor_);
    if (cursor_.empty() || found == queue_.end())
        return {};
    queue_.erase(found);
    running_.insert(cursor_);
    return cursor_;
}

bool InfoQueue::can_read_ahead(int most) const
{
    return !busy_ && !queue_.empty() && running_.size() < static_cast<size_t>(std::max(1, most));
}

void InfoQueue::finished(const std::string &path)
{
    const auto found = running_.find(path);
    if (found != running_.end())
        running_.erase(found);
}

}
