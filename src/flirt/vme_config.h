#ifndef FLIRT_VME_CONFIG_H
#define FLIRT_VME_CONFIG_H

#include <QByteArray>
#include <QString>
#include <QUuid>
#include <string>
#include <vector>

#include "flirt/module_templates.h"

namespace flirt
{

// The part of mvme's VMEConfig that shapes the ROOT output.
struct ModuleConfig
{
    QUuid id;
    QString name;     // objectName()
    bool enabled = true;
    // ModuleConfig::getModuleMeta(), resolved as ModuleConfig::read_impl does
    // (vme_config.cc:520-528): the installed template for the module's "type"
    // wins; the embedded "ModuleMeta" is used only if no template matches.
    QString typeName;
    std::vector<QByteArray> eventHeaderFilters;
    QString metaSource; // template file, "embedded ModuleMeta", or empty
};

struct EventConfig
{
    QUuid id;
    QString name;
    std::vector<ModuleConfig> modules;
};

struct VMEConfig
{
    QString controllerType;
    std::vector<EventConfig> events;
};

// Parses the preamble MVMEConfig section ({"VMEConfig": {...}}, space padded).
// Only the current schema version is accepted; older versions are refused
// rather than converted. Module metas are looked up in the given templates.
VMEConfig parse_vme_config(const std::string &json, const std::vector<ModuleMeta> &templates);

// mvme's parser module index -> VME config module index map for one event:
// enabled modules only, in order (vme_analysis_common.cc:444-469).
std::vector<int> make_module_index_map(const EventConfig &event);

}

#endif
