#ifndef FLIRT_DEFAULT_FILTERS_H
#define FLIRT_DEFAULT_FILTERS_H

// --default-filters: the analysis mvme builds when "Generate default filters"
// is applied to every module of the VME config, starting from a new analysis.
// FLIRT builds it the same way, step by step; each step cites
// the mvme source it mirrors.

#include <QJsonObject>
#include <string>
#include <vector>

#include "flirt/module_templates.h"
#include "flirt/vme_config.h"

namespace flirt
{

// One module of the VME config, and the template it got its filters from.
struct DefaultFiltersModule
{
    int eventIndex = -1;
    std::string module;
    std::string type;
    std::string templateFile;   // the default_filters.analysis used; empty: none, skipped
};

struct DefaultFilters
{
    std::string analysisJson;   // an mvme .analysis document
    std::vector<DefaultFiltersModule> modules;
};

// Builds the analysis from the templates' default_filters.analysis files.
// VMEObjectSettings (multi-event splitting, event builder) are copied from the
// archive's analysis (approved assumption: a new analysis would have none);
// it must be of version 4 or 5. Object IDs are random, as in mvme.
DefaultFilters make_default_filters(const VMEConfig &vmeConfig, const std::vector<ModuleMeta> &templates,
                                    const std::string &embeddedAnalysisJson);

// mvme's convert_to_current_version for an analysis's JSON (the AnalysisNG
// object) of version 4 (v4_to_v5, which rewrites VMEObjectSettings) or 5
// (unchanged). Other versions throw unsupported_input, prefixed with `what`.
QJsonObject convert_to_current_version(QJsonObject json, const std::string &what);

}

#endif
