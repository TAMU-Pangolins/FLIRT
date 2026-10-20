// compare: decides whether a candidate mvme_root_client-style output is
// identical to a reference. Independent of the converter by design: it links
// only ROOT and reads nothing but the two files.
//
// Exit status: 0 identical, 1 different, 2 usage or I/O error.
#include "checks.h"

#include <TError.h>
#include <TFile.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace
{

void usage(const char *argv0)
{
    std::fprintf(stderr,
                 "usage: %s <cand_raw.root> <ref_raw.root> [--histos <cand_histos.root> <ref_histos.root>]"
                 " [--max-report N] [--ignore-branch-regex <regex>] [--allow-fused-sumwx2]"
                 " [--ignore-run-id] [--data-only]\n", argv0);
}

std::unique_ptr<TFile> open_read(const std::string &path)
{
    std::unique_ptr<TFile> f(TFile::Open(path.c_str(), "READ"));
    if (!f || f->IsZombie())
    {
        std::fprintf(stderr, "error: cannot open %s\n", path.c_str());
        return {};
    }
    return f;
}

} // namespace

int main(int argc, char *argv[])
{
    std::vector<std::string> positional;
    std::string histoCand, histoRef;
    size_t maxReport = 10;
    Ignore ignore;
    FusedSumwx2 fused;
    bool ignoreRunId = false;

    for (int i = 1; i < argc; i++)
    {
        std::string arg = argv[i];
        if (arg == "--histos" && i + 2 < argc)
        {
            histoCand = argv[++i];
            histoRef = argv[++i];
        }
        else if (arg == "--max-report" && i + 1 < argc)
            maxReport = std::strtoul(argv[++i], nullptr, 10);
        else if (arg == "--ignore-branch-regex" && i + 1 < argc)
        {
            ignore.pattern = argv[++i];
            try
            {
                ignore.re = std::regex(ignore.pattern);
            }
            catch (const std::regex_error &e)
            {
                std::fprintf(stderr, "error: invalid regex '%s': %s\n", ignore.pattern.c_str(), e.what());
                return 2;
            }
        }
        else if (arg == "--allow-fused-sumwx2")
            fused.allowed = true;
        else if (arg == "--ignore-run-id")
            ignoreRunId = true;
        else if (arg == "--data-only")
            ignore.dataOnly = true;
        else if (!arg.empty() && arg[0] == '-')
        {
            usage(argv[0]);
            return 2;
        }
        else
            positional.push_back(arg);
    }
    if (positional.size() != 2)
    {
        usage(argv[0]);
        return 2;
    }

    // The generated experiment classes have no dictionary here, by design.
    gErrorIgnoreLevel = kError;

    const auto t0 = std::chrono::steady_clock::now();

    auto cand = open_read(positional[0]), ref = open_read(positional[1]);
    if (!cand || !ref)
        return 2;

    std::deque<Check> checks; // references into it stay valid as it grows
    auto make = [&](const char *name) -> Check &
    {
        auto &c = checks.emplace_back();
        c.name = name;
        c.maxShown = maxReport;
        return c;
    };

    std::printf("candidate: %s\nreference: %s\n", positional[0].c_str(), positional[1].c_str());
    if (ignoreRunId)
        std::printf("IGNORING RunID in MVMERunInfo (--ignore-run-id): a run mvme recorded live is named by its"
                    " start time, a replay by its listfile.\n");
    if (ignore.active())
        std::printf("IGNORING branches, class members and histograms matching '%s'.\n"
                    "Basket layout, byte totals and element ids are then not compared.\n",
                    ignore.pattern.c_str());
    if (ignore.dataOnly)
        std::printf("DATA ONLY (--data-only): every value and every histogram's contents bitwise; tree layout,"
                    " basket sizes, byte counts and compression not compared.\n");
    if (fused.allowed)
        std::printf("ALLOWING a histogram's fTsumwx2 to differ where one file holds the fused sum and the other"
                    " the separate sum, recomputed from the raw tree (--allow-fused-sumwx2).\n");
    check_keys(cand.get(), ref.get(), make("raw: keys"), ignore);
    check_streamers(cand.get(), ref.get(), make("raw: streamer info"), ignore);
    check_run_info(cand.get(), ref.get(), make("raw: MVMERunInfo"), ignoreRunId);
    Check &layout = make(ignore.dataOnly ? "raw: tree structure (layout not compared)" : "raw: tree layout");
    Check &values = make("raw: tree values (bitwise)");
    check_trees(cand.get(), ref.get(), layout, values, ignore);

    if (!histoCand.empty())
    {
        auto hc = open_read(histoCand), hr = open_read(histoRef);
        if (!hc || !hr)
            return 2;
        std::printf("candidate histos: %s\nreference histos: %s\n", histoCand.c_str(), histoRef.c_str());
        check_keys(hc.get(), hr.get(), make("histos: keys"), ignore);
        check_streamers(hc.get(), hr.get(), make("histos: streamer info"), ignore);
        check_histos(hc.get(), hr.get(), cand.get(), make("histos: contents (bitwise)"), ignore, fused);
    }

    bool identical = true;
    for (const auto &c: checks)
    {
        c.print();
        identical = identical && c.passed();
    }

    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::string except;
    if (ignoreRunId)
        except = "RunID";
    if (ignore.active())
        except += (except.empty() ? "" : " AND ") + std::string("IGNORED '") + ignore.pattern
            + "' (basket layout not compared)";
    if (fused.histograms)
        except += (except.empty() ? "" : " AND ") + std::string("fTsumwx2 fused rounding in ")
            + std::to_string(fused.histograms) + " histogram" + (fused.histograms == 1 ? "" : "s");
    if (!identical)
        std::printf("DIFFERENT (%.1f s)\n", secs);
    else if (ignore.dataOnly)
        std::printf("SAME DATA (layout not compared)%s%s (%.1f s)\n", except.empty() ? "" : " EXCEPT ",
                    except.c_str(), secs);
    else if (!except.empty())
        std::printf("IDENTICAL EXCEPT %s (%.1f s)\n", except.c_str(), secs);
    else
        std::printf("IDENTICAL (%.1f s)\n", secs);
    return identical ? 0 : 1;
}
