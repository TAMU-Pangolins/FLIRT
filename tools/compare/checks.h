#ifndef FLIRT_COMPARE_CHECKS_H
#define FLIRT_COMPARE_CHECKS_H

#include "report.h"

#include <optional>
#include <regex>
#include <string>
#include <vector>

class TFile;

// Branches, class data members and histograms whose name matches are left out
// of the comparison (--ignore-branch-regex). Basket-level tree layout then
// cannot be compared either: ROOT sizes and flushes every branch's baskets
// from the bytes of all branches, including the ignored ones.
struct Ignore
{
    std::optional<std::regex> re;
    std::string pattern;
    // --data-only: likewise the fields ROOT derives from compressed bytes
    // (flush point, basket sizes and boundaries, byte totals), as for files
    // written with another compression. Values are compared as always.
    bool dataOnly = false;

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

// --allow-fused-sumwx2: a histogram whose only difference is fTsumwx2 is
// accepted when, recomputed from the candidate's raw tree, one file holds the
// fused sum and the other the separate sum (ROOT's TH1::Fill on Apple silicon
// fuses fTsumwx2 += x*x into one fused multiply-add, docs/design.md
// "macOS (Apple silicon)"). No ulp bound; everything else stays bitwise.
struct FusedSumwx2
{
    struct Differing
    {
        std::string name; // "<dir>/<histogram>;<cycle>"
        double cand = 0, ref = 0;
    };
    bool allowed = false;
    std::vector<Differing> differing; // to prove
    size_t histograms = 0;            // proven
};

// Every TH1 present in both files: identity, binning, contents and stats
// bitwise; with fused.allowed, fTsumwx2 as above, from candRaw's trees.
void check_histos(TFile *cand, TFile *ref, TFile *candRaw, Check &c, const Ignore &ignore, FusedSumwx2 &fused);

// fused_sumwx2.cc: recomputes fused.differing from candRaw's trees in one pass
// per tree; a note per proven histogram, a difference per other one.
void prove_fused_sumwx2(TFile *candRaw, TFile *candHistos, Check &c, FusedSumwx2 &fused);

#endif
