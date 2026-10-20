#include "flirt/hit_flags.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>
#include <cctype>
#include <map>
#include <set>

#include "common/failures.h"
#include "flirt/listfile.h"

namespace flirt
{

namespace es = mvme::event_server;

namespace
{

// An amplitude or integral filter of an mvme module template and the bits of
// its data word that hold the flags. Filters as in the pinned templates
// (templates/<type>/analysis/default_filters.analysis, line cited); the flag
// bits from the data sheets: MDPP-32 SCP V00.25 and MDPP-16 SCP V00.20 ("pu =
// pile up flag, ov = overflow or underflow flag"), MDPP-16 QDC and MDPP-32 QDC
// V0010 (overflow only). Time words carry no flags, so they have no entry.
struct FlagTemplate
{
    const char *type;
    const char *filter;
    int pileUpBit;      // -1: none (QDC)
    int overflowBit;
};

const FlagTemplate Templates[] = {
    { "mdpp32_scp", "0001XXXPO00AAAAADDDDDDDDDDDDDDDD", 24, 23 },   // mdpp32_scp :587 amplitude
    { "mdpp16_scp", "0001XXXXPO00AAAADDDDDDDDDDDDDDDD", 23, 22 },   // mdpp16_scp :486 amplitude
    { "mdpp32_qdc", "0001XXXXX00AAAAADDDDDDDDDDDDDDDD", -1, 23 },   // mdpp32_qdc :809 integration_long
    { "mdpp32_qdc", "0001XXXXX11AAAAADDDDDDDDDDDDDDDD", -1, 23 },   // mdpp32_qdc :828 integration_short
    { "mdpp16_qdc", "0001XXXXXX00AAAADDDDDDDDDDDDDDDD", -1, 22 },   // mdpp16_qdc :626 integration_long
    { "mdpp16_qdc", "0001XXXXXX11AAAADDDDDDDDDDDDDDDD", -1, 22 },   // mdpp16_qdc :646 integration_short
};

constexpr int FilterSize = 32;

// The filter as make_filter reads it (data_filter.cc:28-57): spaces removed,
// a shorter filter's missing high bits 'X'. Index i holds bit 31 - i.
std::string full_filter(const QString &filterString)
{
    std::string s;
    for (const auto c: filterString.toStdString())
        if (c != ' ')
            s.push_back(c);
    if (s.size() > FilterSize)
        return {};
    return std::string(FilterSize - s.size(), 'X') + s;
}

// The fixed bits, as make_filter's matchMask and matchValue (data_filter.cc:46-55).
std::pair<uint32_t, uint32_t> fixed_bits(const std::string &filter)
{
    uint32_t mask = 0, value = 0;
    for (int i = 0; i < FilterSize; ++i)
    {
        const uint32_t bit = 1u << (FilterSize - 1 - i);
        if (filter[i] == '0' || filter[i] == '1')
            mask |= bit;
        if (filter[i] == '1')
            value |= bit;
    }
    return { mask, value };
}

bool fixed(char c) { return c == '0' || c == '1'; }

const ModuleConfig *find_module(const VMEConfig &vmeConfig, const QUuid &id, int &eventIndex, int &moduleIndex)
{
    for (size_t ei = 0; ei < vmeConfig.events.size(); ++ei)
        for (size_t mi = 0; mi < vmeConfig.events[ei].modules.size(); ++mi)
            if (vmeConfig.events[ei].modules[mi].id == id)
            {
                eventIndex = static_cast<int>(ei);
                moduleIndex = static_cast<int>(mi);
                return &vmeConfig.events[ei].modules[mi];
            }
    return nullptr;
}

}

HitFlagsEdit add_hit_flag_sources(const std::string &analysisJson, const VMEConfig &vmeConfig)
{
    // Read as parse_analysis reads it (MVMEContext::loadAnalysisConfig,
    // mvme_context.cc:2780-2814).
    QJsonParseError parseError;
    auto doc = QJsonDocument::fromJson(QByteArray::fromStdString(analysisJson), &parseError);
    if (parseError.error != QJsonParseError::NoError)
        throw unsupported_input("analysis is not valid JSON: " + parseError.errorString().toStdString());

    auto top = doc.object();
    const bool wrapped = top.contains("AnalysisNG");
    auto analysis = wrapped ? top["AnalysisNG"].toObject() : top;
    auto sources = analysis["sources"].toArray();

    HitFlagsEdit result;
    std::set<QString> names;
    for (const auto &value: sources)
        names.insert(value.toObject()["name"].toString());
    std::set<QUuid> flagged;    // modules with a source that matches a template

    QJsonArray added;
    for (const auto &value: sources)
    {
        const auto source = value.toObject();
        auto data = source["data"].toObject();
        const auto subFilters = data["subFilters"].toArray();
        if (source["class"].toString() != "analysis::Extractor" || subFilters.size() != 1)
            continue;

        int eventIndex = -1, moduleIndex = -1;
        const auto moduleId = QUuid(source["moduleId"].toString());
        const auto *module = find_module(vmeConfig, moduleId, eventIndex, moduleIndex);
        if (!module)
            continue;   // not built (a2_adapter.cc:1623-1641)

        auto subFilter = subFilters[0].toObject();
        const auto filter = full_filter(subFilter["filterString"].toString());
        if (filter.empty())
            continue;
        const FlagTemplate *match = nullptr;
        for (const auto &t: Templates)
            if (module->typeName == t.type && fixed_bits(filter) == fixed_bits(t.filter))
                match = &t;
        if (!match)
            continue;

        // A flag bit fixed by the filter carries no information (with equal
        // fixed bits it cannot be, as the templates leave them open).
        const int pileUpAt = match->pileUpBit < 0 ? -1 : FilterSize - 1 - match->pileUpBit;
        const int overflowAt = FilterSize - 1 - match->overflowBit;
        if ((pileUpAt >= 0 && fixed(filter[pileUpAt])) || fixed(filter[overflowAt]))
            continue;
        // A source that extracts nothing but flag bits is a flag source
        // itself (e.g. of an analysis this option wrote): nothing to add.
        bool extractsValue = false;
        for (int i = 0; i < FilterSize; ++i)
            extractsValue = extractsValue || (i != pileUpAt && i != overflowAt && std::tolower(static_cast<unsigned char>(filter[i])) == 'd');
        if (!extractsValue)
            continue;
        flagged.insert(moduleId);

        const auto name = source["name"].toString() + "_flags";
        if (names.count(name))
        {
            result.notAdded.push_back(name.toStdString() + " exists already, not added");
            continue;
        }
        names.insert(name);

        // The flag positions extract ('D'), the value's own bits do not ('X');
        // the fixed bits and the address ('A') stay as they are.
        std::string flags = filter;
        for (int i = 0; i < FilterSize; ++i)
            if (i == pileUpAt || i == overflowAt)
                flags[i] = 'D';
            else if (std::tolower(static_cast<unsigned char>(flags[i])) == 'd')
                flags[i] = 'X';

        // A deep copy: options, rngSeed, requiredCompletionCount and the
        // subfilter's wordIndex kept. No parameter names: copied ones would
        // repeat the original's in the generated class (mvme_root_client's
        // templates make a member per name).
        auto copy = source;
        copy["name"] = name;
        copy["id"] = QUuid::createUuidV5(QUuid(source["id"].toString()), QByteArray("flirt-hit-flags")).toString();
        subFilter["filterString"] = QString::fromStdString(flags);
        data["subFilters"] = QJsonArray{ subFilter };
        data["parameterNames"] = QJsonArray();
        copy["data"] = data;
        added.append(copy);

        HitFlagSource hs;
        hs.name = name.toStdString();
        hs.original = source["name"].toString().toStdString();
        hs.module = module->name.toStdString();
        hs.type = module->typeName.toStdString();
        hs.eventIndex = eventIndex;
        hs.moduleIndex = moduleIndex;
        hs.pileUp = match->pileUpBit >= 0;
        hs.filter = flags;
        result.sources.push_back(hs);
    }

    // Appended at the end, in their originals' order: the a2 adapter's stable
    // sort by module index (a2_adapter.cc:1186-1192) then puts each one last
    // within its module.
    for (const auto &value: added)
        sources.append(value);
    analysis["sources"] = sources;
    if (wrapped)
        top["AnalysisNG"] = analysis;
    else
        top = analysis;

    for (const auto &event: vmeConfig.events)
        for (const auto &module: event.modules)
            if (!flagged.count(module.id))
            {
                bool known = false;
                for (const auto &t: Templates)
                    known = known || module.typeName == t.type;
                result.notAdded.push_back("not available for module " + module.name.toStdString() + " (type "
                                          + module.typeName.toStdString() + ")"
                                          + (known ? ": no source with mvme's amplitude or integral filter" : ""));
            }

    // mvme saves analyses the same way (AnalysisSerializer, mvme_context.cc:129-145).
    result.analysisJson = QJsonDocument(top).toJson(QJsonDocument::Indented).toStdString();
    return result;
}

HitFlagCounter::HitFlagCounter(const AnalysisConfig &analysis, const std::vector<HitFlagSource> &sources)
    : counts_(sources.size())
{
    slots_.resize(analysis.dataSources.size());
    for (size_t ei = 0; ei < analysis.dataSources.size(); ++ei)
    {
        const auto &list = analysis.dataSources[ei];
        slots_[ei].assign(list.size(), -1);
        for (size_t ds = 0; ds < list.size(); ++ds)
            for (size_t i = 0; i < sources.size(); ++i)
                if (sources[i].eventIndex == static_cast<int>(ei) && sources[i].moduleIndex == list[ds].moduleIndex
                    && sources[i].name == list[ds].name.toStdString())
                    slots_[ei][ds] = static_cast<int>(i);
    }
}

void HitFlagCounter::eventData(const es::Message &, int eventIndex, const std::vector<es::DataSourceContents> &contents)
{
    if (eventIndex < 0 || static_cast<size_t>(eventIndex) >= slots_.size())
        return;
    const auto &slotOf = slots_[eventIndex];
    for (size_t ds = 0; ds < contents.size() && ds < slotOf.size(); ++ds)
    {
        if (slotOf[ds] < 0)
            continue;
        const auto &dsc = contents[ds];
        auto &channels = counts_[slotOf[ds]];
        const auto entrySize = es::get_entry_size(dsc);
        // As mvme_root_client reads the pairs (mvme_root_client.cc:989-1012).
        for (size_t e = 0; e < dsc.count; ++e)
        {
            const uint8_t *indexPtr = dsc.firstIndex + e * entrySize;
            const uint8_t *valuePtr = indexPtr + es::get_storage_type_size(dsc.indexType);
            const auto index = es::read_storage<uint32_t>(dsc.indexType, indexPtr);
            const auto value = es::read_storage<uint64_t>(dsc.valueType, valuePtr);
            if (index >= channels.size())
                channels.resize(index + 1);
            auto &c = channels[index];
            ++c.hits;
            if (value == 1) ++c.value1;
            else if (value == 2) ++c.value2;
            else if (value == 3) ++c.value3;
        }
    }
}

es::json hit_flags_json(const HitFlagsEdit &edit, const HitFlagCounter &counter, const std::string &analysisFileName)
{
    uint64_t hits = 0, pileUp = 0, overUnderflow = 0, both = 0, overflow = 0;
    es::json sources = es::json::array();
    for (size_t i = 0; i < edit.sources.size(); ++i)
    {
        const auto &s = edit.sources[i];
        es::json channels = es::json::array();
        uint64_t sHits = 0, v1 = 0, v2 = 0, v3 = 0;
        const auto &counts = counter.counts().at(i);
        for (size_t ch = 0; ch < counts.size(); ++ch)
        {
            const auto &c = counts[ch];
            if (!c.hits)
                continue;
            sHits += c.hits;
            v1 += c.value1;
            v2 += c.value2;
            v3 += c.value3;
            if (s.pileUp)
                channels.push_back({ {"channel", ch}, {"hits", c.hits}, {"pileUp", c.value2},
                                     {"overUnderflow", c.value1}, {"both", c.value3} });
            else
                channels.push_back({ {"channel", ch}, {"hits", c.hits}, {"overflow", c.value1} });
        }
        hits += sHits;
        es::json source = { {"name", s.name}, {"original", s.original}, {"module", s.module}, {"type", s.type},
                            {"filter", s.filter}, {"hits", sHits} };
        if (s.pileUp)
        {
            pileUp += v2;
            overUnderflow += v1;
            both += v3;
            source["values"] = { {"0", "clean"}, {"1", "over/underflow"}, {"2", "pile-up"}, {"3", "both"} };
            source["pileUp"] = v2;
            source["overUnderflow"] = v1;
            source["both"] = v3;
        }
        else
        {
            overflow += v1;
            source["values"] = { {"0", "clean"}, {"1", "overflow"} };
            source["overflow"] = v1;
        }
        source["channels"] = channels;
        sources.push_back(source);
    }
    return { {"enabled", true}, {"analysis", analysisFileName},
             {"totals", { {"hits", hits}, {"pileUp", pileUp}, {"overUnderflow", overUnderflow}, {"both", both},
                          {"overflow", overflow} }},
             {"sources", sources}, {"notAdded", edit.notAdded} };
}

std::string hit_flags_line(const es::json &hitFlags)
{
    auto n = [](const es::json &j) { return separate_thousands(std::to_string(j.get<uint64_t>())); };
    bool scp = false, qdc = false;
    for (const auto &s: hitFlags.at("sources"))
        (s.count("pileUp") ? scp : qdc) = true;
    if (!scp && !qdc)
        return "hit flags: none added, no module has an amplitude source FLIRT knows the flags of"
               " (the report lists the modules)";
    const auto &t = hitFlags.at("totals");
    std::string line = "hit flags: ";
    if (scp)
        line += n(t.at("pileUp")) + " pile-up, " + n(t.at("overUnderflow")) + " over/underflow, "
            + n(t.at("both")) + " both";
    if (qdc)
        line += std::string(scp ? ", " : "") + n(t.at("overflow")) + " overflow";
    return line + " (of " + n(t.at("hits")) + " hits)";
}

}
