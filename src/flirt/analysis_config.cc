#include "flirt/analysis_config.h"
#include "flirt/listfile.h"

#include <algorithm>
#include <map>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QVariantMap>

#include "analysis/analysis_json_util.h" // a2_listfilter_from_json (QtCore only)
#include "mdpp-sampling/mdpp_decode.h"

namespace flirt
{

namespace
{

// analysis.cc:147. Older files are converted by mvme; only the 4 -> 5 step
// (which touches nothing but VMEObjectSettings) is mirrored here.
constexpr int CurrentAnalysisVersion = 5;

struct EventSettings
{
    bool multiEventProcessing = false;
    bool eventBuilderEnabled = false;
};

// The two per-event flags mvme checks (analysis_util.cc:576-610), as they look
// after convert_to_current_version (analysis_serialization.cc:792-815).
std::map<QUuid, EventSettings> read_event_settings(const QJsonObject &json, int version)
{
    std::map<QUuid, EventSettings> result;
    auto container = json["VMEObjectSettings"].toObject();

    for (auto it = container.begin(); it != container.end(); ++it)
    {
        auto settings = it.value().toObject();
        EventSettings es;

        if (version == 4)
        {
            // v4_to_v5 (analysis_serialization.cc:724-772) rebuilds the
            // settings from EventBuilderEnabled/EventBuilderSettings only, so
            // MultiEventProcessing does not survive the conversion.
            es.eventBuilderEnabled = settings["EventBuilderEnabled"].toBool();
        }
        else
        {
            // deserialize_objects (analysis_serialization.cc:538-546) stores
            // toVariantMap(); the flags are read with QVariant::toBool().
            auto vars = settings.toVariantMap();
            es.multiEventProcessing = vars["MultiEventProcessing"].toBool();
            es.eventBuilderEnabled = vars["EventBuilderEnabled"].toBool();
        }

        result[QUuid(it.key())] = es;
    }

    return result;
}

// Extractor::read (analysis.cc:974-1004) followed by the filter rebuild in
// a2_adapter_build_datasources (a2_adapter.cc:1219-1227).
void read_extractor(const QJsonObject &data, DataSourceConfig &ds)
{
    using namespace a2::data_filter;

    ds.kind = DataSourceConfig::Kind::Extractor;

    if (!data.contains("rngSeed"))
        throw unsupported_input("Extractor without a stored rngSeed; mvme would use a random seed");

    ds.rngSeed = data["rngSeed"].toString().toULongLong(nullptr, 16);
    ds.dataBits = 0;

    for (const auto &value: data["subFilters"].toArray())
    {
        auto filterJson = value.toObject();
        auto bytes = filterJson["filterString"].toString().toLocal8Bit();
        auto wordIndex = filterJson["wordIndex"].toInt(-1);

        // makeFilterFromBytes (data_filter.cc:66-77): strip spaces, make_filter.
        std::string noSpaces;
        for (char c: bytes)
            if (c != ' ')
                noSpaces.push_back(c);
        auto slowFilter = make_filter(noSpaces, wordIndex);

        // a2_adapter.cc:1223-1226
        add_subfilter(&ds.filter, make_filter(to_string(slowFilter), slowFilter.matchWordIndex));
        // MultiWordDataFilter::getDataBits (data_filter.h:166-186)
        ds.dataBits += get_extract_bits(slowFilter, 'D');
    }

    ds.requiredCompletionCount = static_cast<u32>(data["requiredCompletionCount"].toInt());
    ds.options = static_cast<a2::DataSourceOptions::opt_t>(data["options"].toInt());
    ds.parameterNames = data["parameterNames"].toVariant().toStringList();
}

// ListFilterExtractor::read (analysis.cc:1116-1125).
void read_listfilter_extractor(const QJsonObject &data, DataSourceConfig &ds)
{
    ds.kind = DataSourceConfig::Kind::ListFilterExtractor;
    ds.listFilter = a2_listfilter_from_json(data["listFilter"].toObject());
    ds.repetitions = static_cast<u8>(data["repetitions"].toInt());
    ds.rngSeed = data["rngSeed"].toString().toULongLong(nullptr, 16);
    ds.options = static_cast<a2::DataSourceOptions::opt_t>(data["options"].toInt());
    ds.parameterNames = data["parameterNames"].toVariant().toStringList();
    // ListFilterExtractor::getDataBits (analysis.cc:1072-1077)
    ds.dataBits = get_extract_bits(&ds.listFilter, a2::data_filter::MultiWordFilter::CacheD);
}

// DataSourceMdppSampleDecoder::read (analysis.cc:1452-1461).
void read_mdpp_sample_decoder(const QJsonObject &data, DataSourceConfig &ds)
{
    ds.kind = DataSourceConfig::Kind::MdppSampleDecoder;
    ds.moduleTypeName = data["moduleTypeName"].toString("mdpp16_scp").toStdString();
    ds.maxChannels = static_cast<unsigned>(data["maxChannels"].toInt());
    ds.maxSamples = static_cast<unsigned>(data["maxSamples"].toInt());
    ds.rngSeed = data["rngSeed"].toString().toULongLong(nullptr, 16);
    ds.options = static_cast<a2::DataSourceOptions::opt_t>(data["options"].toInt());

    // make_mdpp_sample_decoder throws for module types without a decoder and
    // the a2 adapter then silently leaves the source out (a2_adapter.cc:1283-1295).
    // Channel 0's sample vector is what the event server sends (event_server.cc:537);
    // without channels or samples it would send a stats vector instead.
    if (!mesytec::mvme::mdpp_sampling::get_decoder_function(ds.moduleTypeName.c_str()))
        throw unsupported_input("waveform source '" + ds.name.toStdString()
                                + "': no MDPP sample decoder for module type " + ds.moduleTypeName);
    if (!ds.maxChannels || !ds.maxSamples)
        throw unsupported_input("waveform source '" + ds.name.toStdString()
                                + "' has no channels or no samples");
}

}

AnalysisConfig parse_analysis(const std::string &text, const VMEConfig &vmeConfig)
{
    // mvme refuses to load a document that does not parse
    // (gui_read_json, MVMEContext::loadAnalysisConfig: mvme_context.cc:2780-2794).
    QJsonParseError parseError;
    auto doc = QJsonDocument::fromJson(QByteArray::fromStdString(text), &parseError);

    if (parseError.error != QJsonParseError::NoError)
        throw unsupported_input("analysis is not valid JSON: " + parseError.errorString().toStdString());

    auto json = doc.object();

    // MVMEContext::loadAnalysisConfig (mvme_context.cc:2811-2814)
    if (json.contains("AnalysisNG"))
        json = json["AnalysisNG"].toObject();

    AnalysisConfig result;
    // get_version (analysis_serialization.cc:316-319)
    result.version = json["MVMEAnalysisVersion"].toInt(1);

    if (result.version != 4 && result.version != CurrentAnalysisVersion)
        throw unsupported_input("unsupported analysis version "
                                + std::to_string(result.version) + " (supported: 4, 5)");

    // Event building is not implemented; multi-event splitting is recorded
    // (uses_multi_event_splitting / uses_event_builder, analysis_util.cc:576-610).
    auto settings = read_event_settings(json, result.version);
    result.multiEventProcessing.assign(vmeConfig.events.size(), false);

    for (size_t ei = 0; ei < vmeConfig.events.size(); ++ei)
    {
        const auto &event = vmeConfig.events[ei];
        auto it = settings.find(event.id);
        if (it == settings.end())
            continue;
        if (it->second.eventBuilderEnabled)
            throw unsupported_input("event building is enabled for event '"
                                    + event.name.toStdString() + "'; not supported yet");
        result.multiEventProcessing[ei] = it->second.multiEventProcessing;
    }

    // build_id_to_index_mapping (vme_analysis_common.cc:416-442)
    std::map<QUuid, std::pair<int, int>> moduleIndexes;
    for (int ei = 0; ei < static_cast<int>(vmeConfig.events.size()); ++ei)
        for (int mi = 0; mi < static_cast<int>(vmeConfig.events[ei].modules.size()); ++mi)
            moduleIndexes[vmeConfig.events[ei].modules[mi].id] = { ei, mi };

    result.dataSources.resize(vmeConfig.events.size());

    // deserialize_objects keeps the JSON array order (analysis_serialization.cc:338-364).
    for (const auto &value: json["sources"].toArray())
    {
        auto sourceJson = value.toObject();
        auto className = sourceJson["class"].toString();
        auto eventId = QUuid(sourceJson["eventId"].toString());
        auto moduleId = QUuid(sourceJson["moduleId"].toString());
        auto it = moduleIndexes.find(moduleId);

        // a2_adapter_filter_sources (a2_adapter.cc:1623-1641): sources not
        // attached to a module of this VME config are not built.
        if (eventId.isNull() || moduleId.isNull() || it == moduleIndexes.end())
            continue;

        DataSourceConfig ds;
        ds.className = className;
        ds.name = sourceJson["name"].toString();
        ds.eventIndex = it->second.first;
        ds.moduleIndex = it->second.second;

        if (className == "analysis::Extractor")
            read_extractor(sourceJson["data"].toObject(), ds);
        else if (className == "analysis::ListFilterExtractor")
            read_listfilter_extractor(sourceJson["data"].toObject(), ds);
        else if (className == "analysis::DataSourceMdppSampleDecoder")
            read_mdpp_sample_decoder(sourceJson["data"].toObject(), ds);
        else
            throw unsupported_input("unsupported data source class " + className.toStdString()
                                    + " ('" + ds.name.toStdString() + "')");

        result.dataSources[ds.eventIndex].push_back(ds);
    }

    // a2_adapter.cc:1186-1192
    for (auto &sources: result.dataSources)
        std::stable_sort(sources.begin(), sources.end(),
                         [](const auto &a, const auto &b) { return a.moduleIndex < b.moduleIndex; });

    return result;
}

}
