#include "progress.h"

#include <sstream>
#include <unistd.h>

namespace flirt
{

int ProgressState::percent() const
{
    if (phase != Phase::Progress || total == 0)
        return -1;
    const uint64_t p = processed >= total ? total : processed;
    return static_cast<int>(p * 100 / total);
}

void ProgressReader::poll()
{
    if (fd_ < 0)
        return;

    char buf[4096];
    ssize_t n;
    while ((n = pread(fd_, buf, sizeof(buf), offset_)) > 0)
    {
        offset_ += n;
        partial_.append(buf, static_cast<size_t>(n));
    }

    // The last complete line decides.
    const auto end = partial_.rfind('\n');
    if (end == std::string::npos)
        return;
    const std::string complete = partial_.substr(0, end);
    partial_.erase(0, end + 1);
    const auto begin = complete.rfind('\n');
    const std::string last = begin == std::string::npos ? complete : complete.substr(begin + 1);

    std::istringstream in(last);
    std::string word;
    in >> word;
    if (word == "finishing")
        state_ = { ProgressState::Phase::Finishing, 0, 0 };
    else if (word == "progress")
    {
        uint64_t processed = 0, total = 0;
        if (in >> processed >> total)
            state_ = { ProgressState::Phase::Progress, processed, total };
    }
}

}
