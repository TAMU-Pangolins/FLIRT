// Histogram checks: every TH1 compared bitwise.
#include "checks.h"

#include <TArrayD.h>
#include <TAxis.h>
#include <TClass.h>
#include <TDirectory.h>
#include <TFile.h>
#include <TH1.h>
#include <TKey.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace
{

void collect_histos(TDirectory *dir, const std::string &prefix, const Ignore &ignore,
                    std::vector<std::string> &out)
{
    TIter next(dir->GetListOfKeys());
    while (auto key = static_cast<TKey *>(next()))
    {
        auto cls = TClass::GetClass(key->GetClassName());
        if (!cls || ignore.matches(key->GetName()))
            continue;
        std::string path = prefix + key->GetName();
        if (cls->InheritsFrom(TH1::Class()))
            out.push_back(path + ";" + std::to_string(key->GetCycle()));
        else if (cls->InheritsFrom(TDirectory::Class()))
            if (auto sub = dir->GetDirectory(key->GetName()))
                collect_histos(sub, path + "/", ignore, out);
    }
}

bool same_bits(double a, double b) { return std::memcmp(&a, &b, sizeof(double)) == 0; }

void compare_axis(const std::string &what, const TAxis *a, const TAxis *b, Check &c)
{
    if (a->GetNbins() != b->GetNbins())
    {
        c.diff(mismatch(what + " bins", a->GetNbins(), b->GetNbins()));
        return;
    }
    if (!same_bits(a->GetXmin(), b->GetXmin()) || !same_bits(a->GetXmax(), b->GetXmax()))
        c.diff(what + " range: cand=[" + format_double(a->GetXmin()) + ", " + format_double(a->GetXmax())
               + "] ref=[" + format_double(b->GetXmin()) + ", " + format_double(b->GetXmax()) + "]");
    const TArrayD *xa = a->GetXbins(), *xb = b->GetXbins();
    if (xa->GetSize() != xb->GetSize()
        || std::memcmp(xa->GetArray(), xb->GetArray(), sizeof(double) * xa->GetSize()))
        c.diff(what + " variable bin edges differ");
}

void compare_histo(const std::string &name, TH1 *a, TH1 *b, Check &c, FusedSumwx2 &fused)
{
    auto text = [&](const std::string &what, const std::string &x, const std::string &y)
    {
        if (x != y)
            c.diff(mismatch(name + " " + what, x, y));
    };
    auto number = [&](const std::string &what, double x, double y)
    {
        if (!same_bits(x, y))
            c.diff(name + " " + what + ": cand=" + format_double(x) + " ref=" + format_double(y));
    };

    const size_t before = c.count; // differences of this histogram start here
    text("class", a->ClassName(), b->ClassName());
    text("title", a->GetTitle(), b->GetTitle());
    compare_axis(name + " x axis", a->GetXaxis(), b->GetXaxis(), c);
    compare_axis(name + " y axis", a->GetYaxis(), b->GetYaxis(), c);
    compare_axis(name + " z axis", a->GetZaxis(), b->GetZaxis(), c);
    if (a->GetNcells() != b->GetNcells())
        return;

    number("fEntries", a->GetEntries(), b->GetEntries());
    double sa[4] = {}, sb[4] = {}; // fTsumw, fTsumw2, fTsumwx, fTsumwx2 (1D)
    a->GetStats(sa);
    b->GetStats(sb);
    static const char *statNames[] = {"fTsumw", "fTsumw2", "fTsumwx", "fTsumwx2"};
    for (int i = 0; i < 3; i++)
        number(statNames[i], sa[i], sb[i]);
    // fTsumwx2 last: with --allow-fused-sumwx2 a histogram with no other
    // difference is proven later from the raw tree (fused_sumwx2.cc).
    const bool toProve = fused.allowed && !same_bits(sa[3], sb[3]);
    if (!toProve)
        number(statNames[3], sa[3], sb[3]);

    size_t shown = 0, differing = 0;
    for (int bin = 0; bin < a->GetNcells(); bin++) // includes under/overflow
        if (!same_bits(a->GetBinContent(bin), b->GetBinContent(bin)))
        {
            differing++;
            if (shown++ < 3)
                c.diff(name + " bin " + std::to_string(bin) + ": cand=" + format_double(a->GetBinContent(bin))
                       + " ref=" + format_double(b->GetBinContent(bin)));
        }
    if (differing > shown)
        c.diff(name + ": " + std::to_string(differing) + " bins differ in total");

    const TArrayD *wa = a->GetSumw2(), *wb = b->GetSumw2();
    if (wa->GetSize() != wb->GetSize()
        || std::memcmp(wa->GetArray(), wb->GetArray(), sizeof(double) * wa->GetSize()))
        c.diff(name + " fSumw2 array differs");

    if (toProve)
    {
        if (c.count == before)
            fused.differing.push_back({ name, sa[3], sb[3] });
        else
            number(statNames[3], sa[3], sb[3]);
    }
}

} // namespace

void check_histos(TFile *cand, TFile *ref, TFile *candRaw, Check &c, const Ignore &ignore, FusedSumwx2 &fused)
{
    std::vector<std::string> ha, hb;
    collect_histos(cand, "", ignore, ha);
    collect_histos(ref, "", ignore, hb);
    if (ha != hb)
        c.diff("histogram sets differ (cand " + std::to_string(ha.size()) + ", ref "
               + std::to_string(hb.size()) + "); comparing common ones");

    for (const auto &name: hb)
    {
        if (std::find(ha.begin(), ha.end(), name) == ha.end())
        {
            c.diff("histogram only in reference: " + name);
            continue;
        }
        std::unique_ptr<TH1> a(cand->Get<TH1>(name.c_str())), b(ref->Get<TH1>(name.c_str()));
        if (!a || !b)
        {
            c.diff(name + ": cannot read histogram");
            continue;
        }
        a->SetDirectory(nullptr);
        b->SetDirectory(nullptr);
        compare_histo(name, a.get(), b.get(), c, fused);
    }
    for (const auto &name: ha)
        if (std::find(hb.begin(), hb.end(), name) == hb.end())
            c.diff("histogram only in candidate: " + name);
    prove_fused_sumwx2(candRaw, cand, c, fused);
}
