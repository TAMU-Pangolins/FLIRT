// File-level checks: keys, streamer infos, MVMERunInfo.
#include "checks.h"

#include <TClass.h>
#include <TDirectory.h>
#include <TFile.h>
#include <TKey.h>
#include <TList.h>
#include <TStreamerElement.h>
#include <TStreamerInfo.h>

#include <cinttypes>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>

std::string format_double(double v)
{
    uint64_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.17g (0x%016" PRIx64 ")", v, bits);
    return buf;
}

namespace
{

using KeySet = std::set<std::string>; // "path;cycle class"

void collect_keys(TDirectory *dir, const std::string &prefix, const Ignore &ignore,
                  KeySet &out, size_t &ignored)
{
    TIter next(dir->GetListOfKeys());
    while (auto key = static_cast<TKey *>(next()))
    {
        if (ignore.matches(key->GetName()))
        {
            ignored++;
            continue;
        }

        std::string path = prefix + key->GetName();
        out.insert(path + ";" + std::to_string(key->GetCycle()) + " " + key->GetClassName());

        auto cls = TClass::GetClass(key->GetClassName());
        if (cls && cls->InheritsFrom(TDirectory::Class()))
            if (auto sub = dir->GetDirectory(key->GetName()))
                collect_keys(sub, path + "/", ignore, out, ignored);
    }
}

struct StreamerDesc
{
    std::string version;
    std::string checksum;
    std::string elements;   // " name:type:typeid[len]" per element not ignored
    size_t ignored = 0;     // elements left out
};

std::map<std::string, StreamerDesc> collect_streamers(TFile *f, const Ignore &ignore,
                                                      std::set<std::string> &stockClasses)
{
    std::map<std::string, StreamerDesc> result;
    std::unique_ptr<TList> infos(f->GetStreamerInfoList());
    if (!infos)
        return result;

    TIter next(infos.get());
    while (auto obj = next())
    {
        auto info = dynamic_cast<TStreamerInfo *>(obj);
        if (!info)
            continue; // schema rule lists

        StreamerDesc desc;
        desc.version = "v" + std::to_string(info->GetClassVersion());
        desc.checksum = std::to_string(info->GetCheckSum());
        TIter elems(info->GetElements());
        while (auto el = static_cast<TStreamerElement *>(elems()))
        {
            if (ignore.matches(el->GetName()))
            {
                desc.ignored++;
                continue;
            }
            desc.elements += std::string(" ") + el->GetName() + ":" + el->GetTypeName() + ":"
                + std::to_string(el->GetType()) + "[" + std::to_string(el->GetArrayLength()) + "]";
        }

        std::string key = std::string(info->GetName()) + " " + desc.version;
        result[key] = desc;

        auto cls = TClass::GetClass(info->GetName());
        if (cls && cls->HasDictionary())
            stockClasses.insert(key);
    }
    return result;
}

} // namespace

void check_keys(TFile *cand, TFile *ref, Check &c, const Ignore &ignore)
{
    KeySet a, b;
    size_t ignoredA = 0, ignoredB = 0;
    collect_keys(cand, "", ignore, a, ignoredA);
    collect_keys(ref, "", ignore, b, ignoredB);
    for (const auto &k: a)
        if (!b.count(k))
            c.diff("only in candidate: " + k);
    for (const auto &k: b)
        if (!a.count(k))
            c.diff("only in reference: " + k);
    if (ignoredA || ignoredB)
        c.notes.push_back("IGNORED keys: " + std::to_string(ignoredA) + " in candidate, "
                          + std::to_string(ignoredB) + " in reference");
}

void check_streamers(TFile *cand, TFile *ref, Check &c, const Ignore &ignore)
{
    std::set<std::string> stock;
    auto a = collect_streamers(cand, ignore, stock);
    auto b = collect_streamers(ref, ignore, stock);

    auto report = [&](const std::string &key, const std::string &msg)
    {
        if (stock.count(key))
            c.notes.push_back("stock ROOT class " + msg);
        else
            c.diff(msg);
    };

    for (const auto &[key, da]: a)
    {
        auto it = b.find(key);
        if (it == b.end())
        {
            report(key, "only in candidate: " + key);
            continue;
        }
        const auto &db = it->second;
        // A class checksum covers every member, so it can only be compared
        // when no member was ignored on either side.
        const bool withChecksum = !da.ignored && !db.ignored;
        auto text = [&](const StreamerDesc &d)
        {
            return d.version + (withChecksum ? " checksum=" + d.checksum : std::string()) + " |" + d.elements;
        };
        if (text(da) != text(db))
            report(key, key + " differs: cand {" + text(da) + "} ref {" + text(db) + "}");
        if (!withChecksum)
            c.notes.push_back("IGNORED " + key + ": " + std::to_string(da.ignored) + " member(s) in candidate, "
                              + std::to_string(db.ignored) + " in reference; checksum not compared");
    }
    for (const auto &[key, desc]: b)
        if (!a.count(key))
            report(key, "only in reference: " + key);
}

void check_run_info(TFile *cand, TFile *ref, Check &c, bool ignoreRunId)
{
    using RunInfo = std::map<std::string, std::string>;
    auto read = [](TFile *f) -> std::unique_ptr<RunInfo>
    {
        auto key = f->GetKey("MVMERunInfo");
        if (!key)
            return {};
        return std::unique_ptr<RunInfo>(
            static_cast<RunInfo *>(key->ReadObjectAny(TClass::GetClass("map<string,string>"))));
    };

    auto a = read(cand), b = read(ref);
    if (!a || !b)
    {
        if (a || b)
            c.diff(std::string("MVMERunInfo missing in ") + (a ? "reference" : "candidate"));
        return;
    }
    if (ignoreRunId)
    {
        auto value = [](const RunInfo &m)
        {
            auto it = m.find("RunID");
            return it == m.end() ? std::string("(none)") : "'" + it->second + "'";
        };
        c.notes.push_back("RunID not compared: cand=" + value(*a) + " ref=" + value(*b));
        a->erase("RunID");
        b->erase("RunID");
    }
    for (const auto &[k, v]: *a)
    {
        auto it = b->find(k);
        if (it == b->end())
            c.diff("only in candidate: " + k + "=" + v);
        else if (it->second != v)
            c.diff(mismatch(k, v, it->second));
    }
    for (const auto &[k, v]: *b)
        if (!a->count(k))
            c.diff("only in reference: " + k + "=" + v);
}
