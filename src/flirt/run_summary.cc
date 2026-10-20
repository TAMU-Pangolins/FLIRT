#include "flirt/run_summary.h"

#include <cstdio>

namespace flirt
{

std::string format_duration(int64_t seconds)
{
    char buf[64];
    const long long h = seconds / 3600, m = seconds / 60 % 60, s = seconds % 60;

    if (h)
        std::snprintf(buf, sizeof(buf), "%lld h %02lld min %02lld s", h, m, s);
    else if (m)
        std::snprintf(buf, sizeof(buf), "%lld min %02lld s", m, s);
    else
        std::snprintf(buf, sizeof(buf), "%lld s", s);
    return buf;
}

}
