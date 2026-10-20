#include "flirt/default_filters.h"
#include "flirt/listfile.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QUuid>
#include <algorithm>
#include <tuple>

namespace flirt
{

namespace
{

constexpr int CurrentAnalysisVersion = 5; // analysis.cc:147

// get_version (analysis_serialization.cc:316-319)
int get_version(const QJsonObject &json)
{
    return json["MVMEAnalysisVersion"].toInt(1);
}

// v4_to_v5 (analysis_serialization.cc:724-772), line by line: the settings are
// rebuilt from EventBuilderEnabled and EventBuilderSettings only, so
// MultiEventProcessing does not survive, as read_event_settings reads a
// version 4 analysis (analysis_config.cc).
QJsonObject v4_to_v5(QJsonObject json)
{
    auto oldObjectSettings = json["VMEObjectSettings"].toObject();
    QJsonObject newObjectSettings;

    for (auto it = oldObjectSettings.begin(); it != oldObjectSettings.end(); ++it)
    {
        auto eventId = it.key();
        auto oldSettings = it.value().toObject();
        auto oldEbSettings = oldSettings["EventBuilderSettings"].toObject();

        QJsonObject newSettings;
        QJsonObject newEbSettings;

        newSettings["EventBuilderEnabled"] = oldSettings["EventBuilderEnabled"].toBool();
        newEbSettings["ClockFrequency"] = 16;
        QJsonObject histoSettings;
        histoSettings["binCount"] = 32;
        histoSettings["minValue"] = -16;
        histoSettings["maxValue"] = 16;
        newEbSettings["Histograms"] = histoSettings;

        auto oldMatchWindows = oldEbSettings["MatchWindows"].toObject();
        QJsonObject newMatchWindows;

        for (auto jt = oldMatchWindows.begin(); jt != oldMatchWindows.end(); ++jt)
        {
            auto moduleId = jt.key();
            auto oldMwSettings = jt.value().toObject();
            QJsonObject newMwSettings;

            newMwSettings["enableModule"] = !oldMwSettings.value("ignoreModule").toBool();
            newMwSettings["offset"] = 0;
            newMwSettings["window"] = oldMwSettings["upper"].toInt() - oldMwSettings["lower"].toInt();
            newMatchWindows[moduleId] = newMwSettings;
        }

        newEbSettings["MatchWindows"] = newMatchWindows;
        newSettings["EventBuilderSettings"] = newEbSettings;
        newObjectSettings[eventId] = newSettings;
    }

    json["VMEObjectSettings"] = newObjectSettings;
    return json;
}

// Classes registered in Analysis' object factory (analysis.cc:4837-4871);
// deserialize_objects() silently drops objects of any other class. v1.18.0
// also registered analysis::HistogramOperation; no template uses it.
const QSet<QString> SourceClasses = {
    "analysis::ListFilterExtractor", "analysis::Extractor", "analysis::MultiHitExtractor",
    "analysis::DataSourceMdppSampleDecoder" };
const QSet<QString> OperatorClasses = {
    "analysis::CalibrationMinMax", "analysis::PreviousValue", "analysis::Difference", "analysis::Sum",
    "analysis::ArrayMap", "analysis::RangeFilter1D", "analysis::ConditionFilter", "analysis::RectFilter2D",
    "analysis::BinarySumDiff", "analysis::AggregateOps", "analysis::ExpressionOperator",
    "analysis::ScalerOverflow", "analysis::MdppDeconvolution", "analysis::IntervalCondition",
    "analysis::PolygonCondition", "analysis::ExpressionCondition",
    // sinks, tried by deserialize_objects when makeOperator() fails
    "analysis::Histo1DSink", "analysis::Histo2DSink", "analysis::RateMonitorSink",
    "analysis::WaveformSink", "analysis::ExportSink" };
const QSet<QString> GenericClasses = { "analysis::PlotGridView" };

// The objects deserialize_objects() (analysis_serialization.cc:321-554) keeps.
struct ObjectStore
{
    QJsonArray sources, operators, directories, genericObjects;
    // (srcId, srcIndex, dstId, dstIndex, dstParamIndex); QSet<Connection>:
    // duplicates collapse.
    std::vector<std::tuple<QString, int, QString, int, int>> connections;
};

QJsonArray keep_classes(const QJsonArray &objects, const QSet<QString> &classes)
{
    QJsonArray result;
    for (const auto &value: objects)
        if (classes.contains(value.toObject()["class"].toString()))
            result.append(value);
    return result;
}

// Objects of registered classes; connections whose source is a pipe source
// (data source or operator) and whose destination is an operator.
ObjectStore deserialize_objects(const QJsonObject &json, const QString &file)
{
    ObjectStore store;
    store.sources = keep_classes(json["sources"].toArray(), SourceClasses);
    store.operators = keep_classes(json["operators"].toArray(), OperatorClasses);
    store.directories = json["directories"].toArray();
    store.genericObjects = keep_classes(json["genericObjects"].toArray(), GenericClasses);

    QSet<QString> ids, pipeSources, operators;
    size_t count = 0;
    for (const auto *array: { &store.sources, &store.operators, &store.directories, &store.genericObjects })
        for (const auto &value: *array)
        {
            ids.insert(value.toObject()["id"].toString());
            ++count;
        }
    if (static_cast<size_t>(ids.size()) != count)
        throw unsupported_input(file.toStdString() + ": duplicate object ids");

    for (const auto *array: { &store.sources, &store.operators })
        for (const auto &value: *array)
            pipeSources.insert(value.toObject()["id"].toString());
    for (const auto &value: store.operators)
        operators.insert(value.toObject()["id"].toString());

    for (const auto &value: json["connections"].toArray())
    {
        const auto c = value.toObject();
        const auto key = std::make_tuple(c["srcId"].toString(), c["srcIndex"].toInt(0), c["dstId"].toString(),
                                         c["dstIndex"].toInt(0), c["dstParamIndex"].toInt(0));
        if (pipeSources.contains(std::get<0>(key)) && operators.contains(std::get<2>(key))
            && std::find(store.connections.begin(), store.connections.end(), key) == store.connections.end())
            store.connections.push_back(key);
    }

    return store;
}

void append_all(QJsonArray &to, const QJsonArray &from)
{
    for (const auto &value: from)
        to.append(value);
}

// add_default_filters (analysis_util.cc:667-721) for one module, appending to
// the analysis's arrays. Returns the template file used, empty if none.
QString add_default_filters(QJsonObject &analysis, const ModuleConfig &module, const EventConfig &event,
                            const std::vector<ModuleMeta> &templates)
{
    // vats::get_module_path (template_system.cc:361-372): the directory of the
    // first template with the module meta's typeName.
    const auto meta = find_module_meta(templates, module.typeName);
    if (!meta)
        return {};
    QDir moduleDir(QFileInfo(meta->source).absolutePath());
    QFile filtersFile(moduleDir.filePath("analysis/default_filters.analysis"));

    if (!filtersFile.open(QIODevice::ReadOnly)) // :673-674
        return {};

    const auto file = QFileInfo(filtersFile).canonicalFilePath();
    auto json = QJsonDocument::fromJson(filtersFile.readAll()).object()["AnalysisNG"].toObject(); // :676-677
    json = convert_to_current_version(json, file.toStdString());                                    // :678
    auto store = deserialize_objects(json, file);                                                   // :679

    // generate_new_object_ids (analysis_util.cc:190-222): a fresh random UUID
    // per object; directory members that are not objects of this store are dropped.
    QHash<QString, QString> newIds;
    for (auto *array: { &store.sources, &store.operators, &store.directories, &store.genericObjects })
        for (const auto &value: *array)
            newIds.insert(value.toObject()["id"].toString(), QUuid::createUuid().toString()); // :683

    const auto typeName = module.typeName;
    const auto eventId = event.id.toString();
    const auto moduleId = module.id.toString();

    auto rewrite = [&](QJsonArray &array, bool setEventId, bool setModuleId, bool isDirectory)
    {
        for (int i = 0; i < array.size(); ++i)
        {
            auto o = array[i].toObject();
            o["id"] = newIds.value(o["id"].toString());

            if (isDirectory)
            {
                auto data = o["data"].toObject();
                QJsonArray members;
                for (const auto &m: data["members"].toArray())
                    if (newIds.contains(m.toString()))
                        members.append(newIds.value(m.toString()));
                data["members"] = members;
                o["data"] = data;
            }

            // :704-705 (generic objects serialize no eventId), :707-708
            if (setEventId)
                o["eventId"] = eventId;
            if (setModuleId)
                o["moduleId"] = moduleId;

            // :712-717: the module's typeName in object names becomes its name.
            o["name"] = o["name"].toString().replace(typeName, module.name, Qt::CaseInsensitive);
            array[i] = o;
        }
    };

    rewrite(store.sources, true, true, false);
    rewrite(store.operators, true, false, false);
    rewrite(store.directories, true, false, true);
    rewrite(store.genericObjects, false, false, false);

    // Analysis::addObjects (analysis.cc:5760-5773) appends sources, operators,
    // directories and generic objects; condition links, VMEObjectSettings and
    // properties of the template are not taken over.
    for (const auto &key: { "sources", "operators", "directories", "genericObjects" })
    {
        auto array = analysis[key].toArray();
        const QString k = key;
        append_all(array, k == "sources" ? store.sources : k == "operators" ? store.operators
                          : k == "directories" ? store.directories : store.genericObjects);
        analysis[key] = array;
    }

    auto connections = analysis["connections"].toArray();
    for (const auto &[src, srcIndex, dst, dstIndex, dstParamIndex]: store.connections)
        connections.append(QJsonObject{ {"srcId", newIds.value(src)}, {"srcIndex", srcIndex},
                                        {"dstId", newIds.value(dst)}, {"dstIndex", dstIndex},
                                        {"dstParamIndex", dstParamIndex} });
    analysis["connections"] = connections;

    return file;
}

}

// convert_to_current_version (analysis_serialization.cc:792-815) with only the
// last converter of get_version_converters (:776-788) ported.
QJsonObject convert_to_current_version(QJsonObject json, const std::string &what)
{
    const int version = get_version(json);

    if (version >= 1 && version < 4)
        throw unsupported_input(what + ": analysis version " + std::to_string(version)
                                + "; mvme's converters for older versions (v1_to_v2, v3_to_v4) are not ported");
    if (version != 4 && version != CurrentAnalysisVersion)
        throw unsupported_input(what + ": unsupported analysis version " + std::to_string(version)
                                + " (supported: 4, 5)");

    if (version == 4)
    {
        json = v4_to_v5(json);
        json["MVMEAnalysisVersion"] = version + 1;
    }
    return json;
}

DefaultFilters make_default_filters(const VMEConfig &vmeConfig, const std::vector<ModuleMeta> &templates,
                                    const std::string &embeddedAnalysisJson)
{
    // The archive's analysis, as MVMEContext::loadAnalysisConfig reads it
    // (mvme_context.cc:2780-2814); only its VMEObjectSettings are used.
    QJsonParseError parseError;
    auto embedded = QJsonDocument::fromJson(QByteArray::fromStdString(embeddedAnalysisJson), &parseError).object();
    if (parseError.error != QJsonParseError::NoError)
        throw unsupported_input("the archive's analysis.analysis is not valid JSON: "
                                + parseError.errorString().toStdString());
    if (embedded.contains("AnalysisNG"))
        embedded = embedded["AnalysisNG"].toObject();
    // MVMEContext::loadAnalysisConfig converts it in Analysis::read
    // (analysis.cc:6296) before its VMEObjectSettings are used.
    embedded = convert_to_current_version(embedded, "--default-filters: the archive's analysis.analysis");

    DefaultFilters result;
    QJsonObject analysis;
    for (const auto &key: { "sources", "operators", "directories", "genericObjects", "connections" })
        analysis[key] = QJsonArray();

    for (size_t ei = 0; ei < vmeConfig.events.size(); ++ei)
        for (const auto &module: vmeConfig.events[ei].modules)
        {
            const auto file = add_default_filters(analysis, module, vmeConfig.events[ei], templates);
            result.modules.push_back({ static_cast<int>(ei), module.name.toStdString(),
                                       module.typeName.toStdString(), file.toStdString() });
        }

    // Analysis::write (analysis.cc:6362-6388) with the ModuleProperties that
    // update_analysis_vme_properties (vme_analysis_common.cc:43-73) sets before
    // every save or autosave (mvme_context.cc:129-145).
    QJsonArray moduleProperties;
    for (const auto &event: vmeConfig.events)
        for (const auto &module: event.modules)
            moduleProperties.append(QJsonObject{ {"moduleId", module.id.toString()},
                                                 {"moduleName", module.name},
                                                 {"moduleTypeName", module.typeName} });

    analysis["MVMEAnalysisVersion"] = CurrentAnalysisVersion;
    analysis["conditionLinks"] = QJsonObject();
    analysis["VMEObjectSettings"] = embedded["VMEObjectSettings"].toObject();
    analysis["properties"] = QJsonObject{ {"ModuleProperties", moduleProperties} };

    // serialize_analysis_to_json_document (analysis.cc:7027-7039)
    result.analysisJson = QJsonDocument(QJsonObject{ {"AnalysisNG", analysis} })
        .toJson(QJsonDocument::Indented).toStdString();
    return result;
}

}
