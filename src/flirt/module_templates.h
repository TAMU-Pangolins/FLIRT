#ifndef FLIRT_MODULE_TEMPLATES_H
#define FLIRT_MODULE_TEMPLATES_H

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <optional>
#include <vector>

namespace flirt
{

// The part of mvme's vats::VMEModuleMeta (template_system.h:79-102) that
// shapes the output: the module type and its multi-event header filters.
struct ModuleMeta
{
    QString typeName;
    std::vector<QByteArray> eventHeaderFilters; // filter strings, in template order
    QString source;                              // template file, or "embedded ModuleMeta"
};

// The module metas of an mvme templates directory, in the order mvme's
// read_templates_from_path() yields them (template_system.cc:285-356).
// mvme's own template_system.cc cannot be built here: it needs Qt Widgets.
std::vector<ModuleMeta> read_module_templates(const QString &templatesDir);

// template_system.cc:214-240
ModuleMeta modulemeta_from_json(const QJsonObject &json);

// The first meta with the given typeName (template_system.cc:505-514).
std::optional<ModuleMeta> find_module_meta(const std::vector<ModuleMeta> &metas,
                                           const QString &typeName);

}

#endif
