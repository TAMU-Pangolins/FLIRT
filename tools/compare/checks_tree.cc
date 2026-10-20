// TTree checks: settings and branch layout, then every value bitwise.
//
// Values are read without the generated experiment dictionary: the tree is put
// in MakeClass mode and every terminal (leaf-carrying) branch is bound to a
// plain buffer through its TBranch pointer. Binding by pointer, not by name,
// matters because mvme's module branches have no trailing '.', so sub-branch
// names such as "fName" repeat across modules.
#include "checks.h"

#include <TBranchElement.h>
#include <TClass.h>
#include <TDirectory.h>
#include <TFile.h>
#include <TKey.h>
#include <TLeaf.h>
#include <TString.h>
#include <TTree.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace
{

// "path;cycle" of every TTree key, recursing into directories.
void collect_trees(TDirectory *dir, const std::string &prefix, std::vector<std::string> &out)
{
    TIter next(dir->GetListOfKeys());
    while (auto key = static_cast<TKey *>(next()))
    {
        auto cls = TClass::GetClass(key->GetClassName());
        if (!cls)
            continue;
        std::string path = prefix + key->GetName();
        if (cls->InheritsFrom(TTree::Class()))
            out.push_back(path + ";" + std::to_string(key->GetCycle()));
        else if (cls->InheritsFrom(TDirectory::Class()))
            if (auto sub = dir->GetDirectory(key->GetName()))
                collect_trees(sub, path + "/", out);
    }
}

// All branches in depth-first order with their '/'-joined paths. Ignored
// branches are left out together with their sub-branches.
void collect_branches(TObjArray *branches, const std::string &prefix, const Ignore &ignore,
                      std::vector<std::pair<std::string, TBranch *>> &out)
{
    TIter next(branches);
    while (auto b = static_cast<TBranch *>(next()))
    {
        if (ignore.matches(b->GetName()))
            continue;
        std::string path = prefix + b->GetName();
        out.emplace_back(path, b);
        collect_branches(b->GetListOfBranches(), path + "/", ignore, out);
    }
}

std::vector<std::pair<std::string, TBranch *>> branches_of(TTree *t, const Ignore &ignore)
{
    std::vector<std::pair<std::string, TBranch *>> result;
    collect_branches(t->GetListOfBranches(), "", ignore, result);
    return result;
}

std::string leaf_types(TBranch *b)
{
    std::string s;
    TIter next(b->GetListOfLeaves());
    while (auto l = static_cast<TLeaf *>(next()))
        s += std::string(s.empty() ? "" : " ") + l->GetName() + ":" + l->GetTypeName() + "["
            + std::to_string(l->GetLenStatic()) + "]" + (l->GetLeafCount() ? "(var)" : "");
    return s;
}

// Returns true when both trees have the same branch paths and leaf types, i.e.
// their values can be compared slot by slot.
bool compare_tree_layout(const std::string &name, TTree *a, TTree *b, Check &c, const Ignore &ignore)
{
    auto field = [&](const std::string &what, auto x, auto y)
    {
        if (x != y)
            c.diff(mismatch(name + " " + what, x, y));
    };

    // Fields ROOT derives from all branches of the tree (flush point, basket
    // sizes and boundaries, byte totals) or from a member's position in its
    // class (element id). With ignored branches or members, or with
    // --data-only, they are reported, not compared.
    size_t notCompared = 0;
    auto allBranchesField = [&](const std::string &what, auto x, auto y)
    {
        if (!ignore.active() && !ignore.dataOnly)
            field(what, x, y);
        else if (x != y && notCompared++ < 5)
            c.notes.push_back((ignore.dataOnly ? "NOT COMPARED (--data-only): "
                                               : "IGNORED (depends on ignored branches): ")
                              + mismatch(name + " " + what, x, y));
    };

    field("title", std::string(a->GetTitle()), std::string(b->GetTitle()));
    field("entries", a->GetEntries(), b->GetEntries());
    allBranchesField("fAutoFlush", a->GetAutoFlush(), b->GetAutoFlush());
    field("fAutoSave", a->GetAutoSave(), b->GetAutoSave());
    field("fEstimate", a->GetEstimate(), b->GetEstimate());
    field("fMaxEntryLoop", a->GetMaxEntryLoop(), b->GetMaxEntryLoop());
    field("fMaxVirtualSize", a->GetMaxVirtualSize(), b->GetMaxVirtualSize());
    allBranchesField("fTotBytes", a->GetTotBytes(), b->GetTotBytes());
    if (a->GetZipBytes() != b->GetZipBytes())
        c.notes.push_back(mismatch(name + " fZipBytes (not compared)", a->GetZipBytes(), b->GetZipBytes()));

    auto ba = branches_of(a, ignore), bb = branches_of(b, ignore);
    std::vector<std::string> pa, pb;
    for (auto &p: ba) pa.push_back(p.first);
    for (auto &p: bb) pb.push_back(p.first);
    if (pa != pb)
    {
        c.diff(name + ": branch hierarchies differ (cand " + std::to_string(pa.size())
               + " branches, ref " + std::to_string(pb.size()) + ")");
        for (auto &p: pa)
            if (std::find(pb.begin(), pb.end(), p) == pb.end())
                c.diff(name + ": branch only in candidate: " + p);
        for (auto &p: pb)
            if (std::find(pa.begin(), pa.end(), p) == pa.end())
                c.diff(name + ": branch only in reference: " + p);
        return false;
    }

    bool sameLeaves = true;

    for (size_t i = 0; i < ba.size(); i++)
    {
        const std::string &what = ba[i].first; // field() prefixes the tree name
        TBranch *x = ba[i].second, *y = bb[i].second;
        field(what + " class", std::string(x->ClassName()), std::string(y->ClassName()));
        auto ex = dynamic_cast<TBranchElement *>(x), ey = dynamic_cast<TBranchElement *>(y);
        if (ex && ey)
        {
            field(what + " element class", std::string(ex->GetClassName()), std::string(ey->GetClassName()));
            field(what + " streamer type", ex->GetStreamerType(), ey->GetStreamerType());
            field(what + " type", ex->GetType(), ey->GetType());
            allBranchesField(what + " id", ex->GetID(), ey->GetID());
        }
        if (leaf_types(x) != leaf_types(y))
            sameLeaves = false;
        field(what + " leaves", leaf_types(x), leaf_types(y));
        field(what + " split level", x->GetSplitLevel(), y->GetSplitLevel());
        allBranchesField(what + " basket size", x->GetBasketSize(), y->GetBasketSize());
        field(what + " entries", x->GetEntries(), y->GetEntries());
        allBranchesField(what + " fTotBytes", x->GetTotBytes(), y->GetTotBytes());
        allBranchesField(what + " baskets", x->GetWriteBasket(), y->GetWriteBasket());
        if (x->GetWriteBasket() == y->GetWriteBasket())
            for (int k = 0; k <= x->GetWriteBasket(); k++)
                allBranchesField(what + " basket " + std::to_string(k) + " first entry",
                                 x->GetBasketEntry()[k], y->GetBasketEntry()[k]);
        if (x->GetZipBytes() != y->GetZipBytes() && !ignore.dataOnly)
            c.notes.push_back(mismatch(what + " fZipBytes (not compared)", x->GetZipBytes(), y->GetZipBytes()));
    }

    if (ignore.dataOnly)
        c.notes.push_back("NOT COMPARED " + name + " (--data-only): basket layout, byte totals and element ids ("
                          + std::to_string(notCompared) + " such field(s) differ)");
    else if (ignore.active())
        c.notes.push_back("IGNORED " + name + ": basket layout, byte totals and element ids not compared ("
                          + std::to_string(notCompared) + " such field(s) differ)");
    return sameLeaves;
}

// A terminal branch bound to a plain buffer in MakeClass mode.
struct Binding
{
    TBranch *branch = nullptr;
    std::string path;
    std::string type;     // leaf type name
    int len = 0;          // elements
    size_t elemSize = 0;  // 0 for TString
    std::vector<unsigned char> bytes;
    TString str;
    size_t diffs = 0;
};

size_t element_size(const std::string &type)
{
    static const std::map<std::string, size_t> sizes = {
        {"Double_t", 8}, {"Float_t", 4}, {"Long64_t", 8}, {"ULong64_t", 8},
        {"Int_t", 4}, {"UInt_t", 4}, {"Short_t", 2}, {"UShort_t", 2},
        {"Char_t", 1}, {"UChar_t", 1}, {"Bool_t", 1}};
    auto it = sizes.find(type);
    return it == sizes.end() ? 0 : it->second;
}

// Binds every terminal branch of t. Returns false (with a diff) on a branch
// this reader cannot represent exactly, so nothing is silently skipped.
bool bind(TTree *t, const std::string &name, const Ignore &ignore, std::vector<Binding> &out, Check &c)
{
    t->SetMakeClass(1);
    auto all = branches_of(t, ignore);
    out.reserve(all.size());
    for (auto &[path, b]: all)
    {
        if (b->GetListOfBranches()->GetEntries() > 0)
            continue;
        auto leaf = static_cast<TLeaf *>(b->GetListOfLeaves()->At(0));
        if (!leaf || b->GetListOfLeaves()->GetEntries() != 1 || leaf->GetLeafCount())
        {
            c.diff(name + " " + path + ": unsupported leaf layout " + leaf_types(b));
            return false;
        }
        Binding bd;
        bd.branch = b;
        bd.path = path;
        bd.type = leaf->GetTypeName();
        bd.len = leaf->GetLenStatic();
        if (bd.type != "TString")
        {
            bd.elemSize = element_size(bd.type);
            if (!bd.elemSize || bd.len < 1)
            {
                c.diff(name + " " + path + ": unsupported leaf type " + leaf_types(b));
                return false;
            }
            bd.bytes.resize(bd.elemSize * bd.len);
        }
        out.push_back(std::move(bd));
        auto &ref = out.back();
        b->SetAddress(ref.elemSize ? static_cast<void *>(ref.bytes.data()) : static_cast<void *>(&ref.str));
    }
    return true;
}

std::string format_element(const Binding &b, int i)
{
    const unsigned char *p = b.bytes.data() + i * b.elemSize;
    if (b.type == "Double_t")
    {
        double v;
        std::memcpy(&v, p, 8);
        return format_double(v);
    }
    std::string hex = "0x";
    char buf[3];
    for (size_t k = b.elemSize; k-- > 0;) // little-endian: print most significant first
    {
        std::snprintf(buf, sizeof(buf), "%02x", p[k]);
        hex += buf;
    }
    return hex;
}

void compare_tree_values(const std::string &name, TTree *a, TTree *b, Check &c, const Ignore &ignore)
{
    std::vector<Binding> ba, bb;
    if (!bind(a, name, ignore, ba, c) || !bind(b, name, ignore, bb, c))
        return;

    const Long64_t n = std::min(a->GetEntries(), b->GetEntries());
    if (a->GetEntries() != b->GetEntries())
        c.diff(mismatch(name + " entries (compared first " + std::to_string(n) + ")",
                        a->GetEntries(), b->GetEntries()));

    for (Long64_t e = 0; e < n; e++)
    {
        // Each bound branch is read on its own, so ignored branches are never read.
        for (size_t k = 0; k < ba.size(); k++)
            if (ba[k].branch->GetEntry(e) < 0 || bb[k].branch->GetEntry(e) < 0)
            {
                c.diff(name + ": read error at entry " + std::to_string(e) + " in " + ba[k].path);
                return;
            }
        for (size_t k = 0; k < ba.size(); k++)
        {
            Binding &x = ba[k], &y = bb[k];
            if (!x.elemSize)
            {
                if (x.str != y.str)
                {
                    x.diffs++;
                    c.diff(name + " entry " + std::to_string(e) + " " + x.path + ": cand='"
                           + x.str.Data() + "' ref='" + y.str.Data() + "'");
                }
                continue;
            }
            if (std::memcmp(x.bytes.data(), y.bytes.data(), x.bytes.size()) == 0)
                continue;
            for (int i = 0; i < x.len; i++)
                if (std::memcmp(x.bytes.data() + i * x.elemSize, y.bytes.data() + i * y.elemSize, x.elemSize))
                {
                    x.diffs++;
                    c.diff(name + " entry " + std::to_string(e) + " " + x.path + " slot " + std::to_string(i)
                           + ": cand=" + format_element(x, i) + " ref=" + format_element(y, i));
                }
        }
    }

    size_t slots = 0;
    for (auto &x: ba)
        slots += x.len;
    c.notes.push_back(name + ": compared " + std::to_string(n) + " entries x " + std::to_string(slots)
                      + " slots in " + std::to_string(ba.size()) + " leaf branches");
    for (auto &x: ba)
        if (x.diffs)
            c.notes.push_back(name + " " + x.path + ": " + std::to_string(x.diffs) + " differing slots");
    a->ResetBranchAddresses();
    b->ResetBranchAddresses();
}

} // namespace

void check_trees(TFile *cand, TFile *ref, Check &layout, Check &values, const Ignore &ignore)
{
    std::vector<std::string> ta, tb;
    collect_trees(cand, "", ta);
    collect_trees(ref, "", tb);

    for (const auto &name: tb)
    {
        if (std::find(ta.begin(), ta.end(), name) == ta.end())
        {
            layout.diff("tree only in reference: " + name);
            continue;
        }
        std::unique_ptr<TTree> a(cand->Get<TTree>(name.c_str())), b(ref->Get<TTree>(name.c_str()));
        if (!a || !b)
        {
            layout.diff(name + ": cannot read tree");
            continue;
        }
        if (compare_tree_layout(name, a.get(), b.get(), layout, ignore))
            compare_tree_values(name, a.get(), b.get(), values, ignore);
        else
            values.diff(name + ": values not compared because the branch structure differs");
    }
    for (const auto &name: ta)
        if (std::find(tb.begin(), tb.end(), name) == tb.end())
            layout.diff("tree only in candidate: " + name);
}
