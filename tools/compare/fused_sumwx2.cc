// --allow-fused-sumwx2: proves that a histogram's differing fTsumwx2 is the
// fused sum in one file and the separate sum in the other, recomputed from the
// candidate's raw tree (docs/design.md, "macOS (Apple silicon)").
// TH1::Fill(x) does fTsumwx2 += x*x; the two ways a compiler builds that line:
//   separate: x*x rounded to a double, then added (x86-64 without FMA, as the
//             official Linux build);
//   fused:    one fused multiply-add, std::fma(x, x, sum) (clang's default for
//             arm64, e.g. an Apple silicon ROOT's libHist).
// This file is compiled with -ffp-contract=off (CMakeLists.txt), so the
// separate sum is never contracted into an fma.
#include "checks.h"

#include <TBranch.h>
#include <TClass.h>
#include <TDirectory.h>
#include <TFile.h>
#include <TH1.h>
#include <TKey.h>
#include <TLeaf.h>
#include <TTree.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace
{

bool same_bits(double a, double b) { return std::memcmp(&a, &b, sizeof(double)) == 0; }

// Distance in units in the last place for two finite doubles of the same sign;
// -1 otherwise.
int64_t ulp_distance(double a, double b)
{
    if (!std::isfinite(a) || !std::isfinite(b) || std::signbit(a) != std::signbit(b))
        return -1;
    int64_t ia, ib;
    std::memcpy(&ia, &a, sizeof(double));
    std::memcpy(&ib, &b, sizeof(double));
    return ia > ib ? ia - ib : ib - ia;
}

std::string g17(double v)
{
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.17g", v);
    return buf;
}

// The raw histograms of an event in the order the client creates them, which
// is the order of their keys: per module (the tree's top-level branches), per
// data source (the module's array sub-branches), per index
// (mvme_root_client.cc make_raw_histograms; FLIRT's root_output.cc:70-120).
struct Slot
{
    TLeaf *leaf = nullptr;
    int index = 0;
    std::string module;
};

std::vector<Slot> histogram_slots(TTree *tree)
{
    std::vector<Slot> slots;
    for (auto moduleObj: *tree->GetListOfBranches())
    {
        auto module = static_cast<TBranch *>(moduleObj);
        for (auto subObj: *module->GetListOfBranches())
        {
            const std::string sub = subObj->GetName();
            const auto bracket = sub.find('[');
            if (bracket == std::string::npos)
                continue;
            auto leaf = static_cast<TBranch *>(subObj)->GetLeaf(sub.substr(0, bracket).c_str());
            if (!leaf)
                continue;
            for (int i = 0; i < leaf->GetLenStatic(); ++i)
                slots.push_back({ leaf, i, module->GetName() });
        }
    }
    return slots;
}

struct Recomputed
{
    const FusedSumwx2::Differing *d = nullptr;
    std::unique_ptr<TH1> histo; // the candidate's, for its axis and statistics rules
    TLeaf *leaf = nullptr;
    int index = 0;
    double separate = 0, fused = 0;
};

} // namespace

void prove_fused_sumwx2(TFile *candRaw, TFile *candHistos, Check &c, FusedSumwx2 &fused)
{
    if (fused.differing.empty())
        return;
    std::map<std::string, const FusedSumwx2::Differing *> wanted;
    for (const auto &d: fused.differing)
        wanted[d.name] = &d;
    std::set<std::string> mapped;
    std::vector<Recomputed> all;

    // Histogram k of an event directory is filled from slot k of the event's
    // tree; its name must start with the module's.
    TIter nextDir(candHistos->GetListOfKeys());
    while (auto dirKey = static_cast<TKey *>(nextDir()))
    {
        auto cls = TClass::GetClass(dirKey->GetClassName());
        if (!cls || !cls->InheritsFrom(TDirectory::Class()))
            continue;
        auto dir = candHistos->GetDirectory(dirKey->GetName());
        auto tree = candRaw->Get<TTree>(dirKey->GetName());
        if (!dir || !tree)
            continue;

        const auto slots = histogram_slots(tree);
        std::vector<Recomputed> event;
        size_t k = 0;
        TIter next(dir->GetListOfKeys());
        while (auto key = static_cast<TKey *>(next()))
        {
            auto hcls = TClass::GetClass(key->GetClassName());
            if (!hcls || !hcls->InheritsFrom(TH1::Class()))
                continue;
            const size_t slot = k++;
            const std::string name = std::string(dirKey->GetName()) + "/" + key->GetName() + ";"
                + std::to_string(key->GetCycle());
            auto it = wanted.find(name);
            if (it == wanted.end())
                continue;
            Recomputed r;
            r.d = it->second;
            if (slot < slots.size() && std::string(key->GetName()).rfind(slots[slot].module + "_", 0) == 0)
            {
                r.leaf = slots[slot].leaf;
                r.index = slots[slot].index;
            }
            r.histo.reset(dir->Get<TH1>(name.substr(name.find('/') + 1).c_str()));
            if (r.histo)
                r.histo->SetDirectory(nullptr);
            event.push_back(std::move(r));
        }
        if (k != slots.size()) // no histogram of this event can be mapped
            for (auto &r: event)
                r.leaf = nullptr;

        // One pass over the tree for all differing histograms of this event,
        // with TH1::Fill(x)'s rules: NaN is not filled (the client skips it),
        // under/overflow does not count unless the histogram says so.
        std::set<TBranch *> branches;
        for (auto &r: event)
            if (r.leaf && r.histo)
                branches.insert(r.leaf->GetBranch());
        for (Long64_t e = 0; !branches.empty() && e < tree->GetEntries(); ++e)
        {
            for (auto b: branches)
                b->GetEntry(e);
            for (auto &r: event)
            {
                if (!r.leaf || !r.histo)
                    continue;
                const double x = r.leaf->GetValue(r.index);
                if (std::isnan(x))
                    continue;
                const int bin = r.histo->GetXaxis()->FindBin(x);
                // kNeutral means TH1's global default, off unless TH1::StatOverflows()
                // is called, which the client never does.
                if ((bin == 0 || bin > r.histo->GetXaxis()->GetNbins())
                    && r.histo->GetStatOverflows() != TH1::EStatOverflows::kConsider)
                    continue;
                const double square = x * x; // rounded: this file is built with -ffp-contract=off
                r.separate = r.separate + square;
                r.fused = std::fma(x, x, r.fused);
            }
        }
        for (auto &r: event)
        {
            mapped.insert(r.d->name);
            all.push_back(std::move(r));
        }
    }

    for (const auto &r: all)
    {
        const auto &d = *r.d;
        if (!r.leaf || !r.histo)
        {
            c.diff(d.name + " fTsumwx2: cand=" + g17(d.cand) + " ref=" + g17(d.ref)
                   + ", not mapped to a raw tree value, so not recomputed");
            continue;
        }
        const std::string ulps = std::to_string(ulp_distance(d.cand, d.ref));
        if (same_bits(d.cand, r.fused) && same_bits(d.ref, r.separate))
            c.notes.push_back(d.name + " fTsumwx2: cand " + g17(d.cand) + " is the fused sum, ref " + g17(d.ref)
                              + " the separate sum (" + ulps + " ulp apart)");
        else if (same_bits(d.cand, r.separate) && same_bits(d.ref, r.fused))
            c.notes.push_back(d.name + " fTsumwx2: cand " + g17(d.cand) + " is the separate sum, ref "
                              + g17(d.ref) + " the fused sum (" + ulps + " ulp apart)");
        else
        {
            c.diff(d.name + " fTsumwx2: cand=" + g17(d.cand) + " ref=" + g17(d.ref) + " fused=" + g17(r.fused)
                   + " separate=" + g17(r.separate) + ": not the fused sum in one file and the separate in the other");
            continue;
        }
        fused.histograms++;
    }
    for (const auto &d: fused.differing)
        if (!mapped.count(d.name))
            c.diff(d.name + " fTsumwx2: cand=" + g17(d.cand) + " ref=" + g17(d.ref)
                   + ", no raw histogram of an event, so not recomputed");
}
