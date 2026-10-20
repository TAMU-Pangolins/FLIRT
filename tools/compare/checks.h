#ifndef FLIRT_COMPARE_CHECKS_H
#define FLIRT_COMPARE_CHECKS_H

#include "report.h"

#include <optional>
#include <regex>
#include <string>

class TFile;

// Branches, class data members and histograms whose name matches are left out
// of the comparison (--ignore-branch-regex). Basket-level tree layout then
// cannot be compared either: ROOT sizes and flushes every branch's baskets
// from the bytes of all branches, including the ignored ones.
struct Ignore
{
    std::optional<std::regex> re;
    std::string pattern;

    bool active() const { return re.has_value(); }
    bool matches(const std::string &name) const { return re && std::regex_search(name, *re); }
};

// Same set of (path, cycle, class) keys, recursing into directories.
void check_keys(TFile *cand, TFile *ref, Check &c, const Ignore &ignore);

// Same user-class streamer infos (version, checksum, elements). Classes that
// stock ROOT has a dictionary for are only reported informationally.
void check_streamers(TFile *cand, TFile *ref, Check &c, const Ignore &ignore);

// Same MVMERunInfo map contents. With ignoreRunId (--ignore-run-id) the entry
// RunID is left out and both values are reported as a note: mvme names a run
// it records live by its start time, a replay (and FLIRT) by the listfile.
void check_run_info(TFile *cand, TFile *ref, Check &c, bool ignoreRunId);

// For every TTree cycle present in both files: tree settings and branch layout
// (layout), then every leaf of every entry bitwise (values).
void check_trees(TFile *cand, TFile *ref, Check &layout, Check &values, const Ignore &ignore);

// Every TH1 present in both files: identity, binning, contents and stats bitwise.
void check_histos(TFile *cand, TFile *ref, Check &c, const Ignore &ignore);

#endif
