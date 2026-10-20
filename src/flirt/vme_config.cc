#include "flirt/vme_config.h"
#include "flirt/listfile.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace flirt
{

// external/mvme/src/vme_config_version.cc
static const int CurrentVMEConfigVersion = 4;

VMEConfig parse_vme_config(const std::string &json, const std::vector<ModuleMeta> &templates)
{
    // listfile_replay.cc:231-236: parse, then take the "VMEConfig" object.
    auto doc = QJsonDocument::fromJson(QByteArray::fromStdString(json));
    auto root = doc.object().value("VMEConfig").toObject();

    if (root.isEmpty())
        throw unsupported_input("MVMEConfig section does not contain a VMEConfig object");

    // vme_config_json_schema_updates.cc:660-663
    int version = root["properties"].toObject()["version"].toInt(1);

    if (version != CurrentVMEConfigVersion)
        throw unsupported_input("unsupported VME config version "
                                + std::to_string(version) + " (only "
                                + std::to_string(CurrentVMEConfigVersion) + " is supported)");

    VMEConfig result;
    // vme_config.cc:1053-1054
    result.controllerType = root["vme_controller"].toObject()["type"].toString();

    if (result.controllerType != "MVLC_ETH" && result.controllerType != "MVLC_USB")
        throw unsupported_input("unsupported VME controller "
                                + result.controllerType.toStdString() + " (MVLC only)");

    // vme_config.cc:1009-1016 (events), EventConfig::read_impl (modules),
    // ConfigObject::read (id, name, enabled: vme_config.cc:152-158).
    for (const auto &eventValue: root["events"].toArray())
    {
        auto eventJson = eventValue.toObject();
        EventConfig event;
        event.id = QUuid(eventJson["id"].toString());
        event.name = eventJson["name"].toString();

        for (const auto &moduleValue: eventJson["modules"].toArray())
        {
            auto moduleJson = moduleValue.toObject();
            ModuleConfig module;
            module.id = QUuid(moduleJson["id"].toString());
            module.name = moduleJson["name"].toString();
            module.enabled = moduleJson["enabled"].toBool(true);

            // ModuleConfig::read_impl (vme_config.cc:520-528)
            std::optional<ModuleMeta> meta = find_module_meta(templates, moduleJson["type"].toString());

            if (!meta && moduleJson.contains("ModuleMeta"))
            {
                meta = modulemeta_from_json(moduleJson["ModuleMeta"].toObject());
                meta->source = QStringLiteral("embedded ModuleMeta");
            }

            if (meta)
            {
                module.typeName = meta->typeName;
                module.eventHeaderFilters = meta->eventHeaderFilters;
                module.metaSource = meta->source;
            }

            event.modules.push_back(module);
        }

        result.events.push_back(event);
    }

    return result;
}

std::vector<int> make_module_index_map(const EventConfig &event)
{
    std::vector<int> result;

    for (int mi = 0; mi < static_cast<int>(event.modules.size()); ++mi)
    {
        if (event.modules[mi].enabled)
            result.push_back(mi);
    }

    return result;
}

}
