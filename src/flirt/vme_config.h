#ifndef FLIRT_VME_CONFIG_H
#define FLIRT_VME_CONFIG_H

#include <QByteArray>
#include <QString>
#include <QUuid>
#include <map>
#include <string>
#include <vector>

#include "flirt/module_templates.h"

namespace flirt
{

// A ConfigObject's "variable_table" (vme_script_variables.cc:97-111): variable
// name -> value.
using VariableTable = std::map<QString, QString>;

// A VMEScriptConfig: name, "vme_script" text (vme_config.cc:369-372) and its
// own variables.
struct ScriptConfig
{
    QString name;
    QString text;
    VariableTable variables;
};

// The part of mvme's VMEConfig that shapes the ROOT output, plus the module
// init scripts and variables the waveform config check reads (sampling_config.h).
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
    // The scripts that initialize the module, in the order mvme runs them
    // (vmeconfig_to_crateconfig.cc:415-440): "vmeReset", then every script in
    // "initScripts". Their own "enabled" flag is not checked (vme_daq.cc:200-235).
    std::vector<ScriptConfig> initScripts;
    VariableTable variables;
};

struct EventConfig
{
    QUuid id;
    QString name;
    std::vector<ModuleConfig> modules;
    VariableTable variables;
};

struct VMEConfig
{
    QString controllerType;
    VariableTable variables;
    std::vector<EventConfig> events;
};

// Parses the preamble MVMEConfig section ({"VMEConfig": {...}}, space padded).
// Only the current schema version is accepted; older versions are refused
// rather than converted. Module metas are looked up in the given templates.
VMEConfig parse_vme_config(const std::string &json, const std::vector<ModuleMeta> &templates);

// Refuses (unsupported_input) a config with more than one VME event that
// reads out data, i.e. has an enabled module: conversions were verified with
// one VME event only (docs/design.md, "Several VME events"). A config
// is not refused for events without an enabled module.
void refuse_several_events(const VMEConfig &config);

// mvme's parser module index -> VME config module index map for one event:
// enabled modules only, in order (vme_analysis_common.cc:444-469).
std::vector<int> make_module_index_map(const EventConfig &event);

}

#endif
