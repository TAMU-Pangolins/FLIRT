#ifndef FLIRT_COMPARE_REPORT_H
#define FLIRT_COMPARE_REPORT_H

#include <cstdio>
#include <string>
#include <vector>

// One named check: passes when no difference was recorded. Keeps at most
// maxShown difference descriptions but counts all of them.
struct Check
{
    std::string name;
    size_t maxShown = 10;
    size_t count = 0;
    std::vector<std::string> shown;
    std::vector<std::string> notes; // informational, never affects the verdict

    void diff(const std::string &what)
    {
        if (count++ < maxShown)
            shown.push_back(what);
    }

    bool passed() const { return count == 0; }

    void print() const
    {
        if (passed())
            std::printf("PASS  %s\n", name.c_str());
        else
            std::printf("FAIL  %s (%zu difference%s)\n", name.c_str(), count, count == 1 ? "" : "s");
        for (const auto &s: shown)
            std::printf("        %s\n", s.c_str());
        if (count > shown.size())
            std::printf("        ... %zu more\n", count - shown.size());
        for (const auto &n: notes)
            std::printf("      info: %s\n", n.c_str());
    }
};

// Formats both sides of a mismatch: "<what>: cand=<c> ref=<r>".
template <typename T>
std::string mismatch(const std::string &what, const T &cand, const T &ref)
{
    return what + ": cand=" + std::to_string(cand) + " ref=" + std::to_string(ref);
}

inline std::string mismatch(const std::string &what, const std::string &cand, const std::string &ref)
{
    return what + ": cand='" + cand + "' ref='" + ref + "'";
}

// "%.17g (0x<bits>)" for a double.
std::string format_double(double v);

#endif
