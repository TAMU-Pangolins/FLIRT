#include "flirt/module_templates.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTextStream>

namespace flirt
{

namespace
{

// read_file + read_json_file (template_system.cc:48-83). Errors yield an empty
// document, as in mvme (which only logs them).
QJsonDocument read_json_file(const QString &fileName)
{
    QFile inFile(fileName);

    if (!inFile.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};

    QTextStream inStream(&inFile);
    return QJsonDocument::fromJson(inStream.readAll().toUtf8());
}

}

ModuleMeta modulemeta_from_json(const QJsonObject &json)
{
    ModuleMeta mm;
    mm.typeName = json["typeName"].toString();

    if (json["eventHeaderFilters"].isArray())
    {
        // New style: a list of {filter, description} objects.
        for (const auto &filterDef: json["eventHeaderFilters"].toArray())
            mm.eventHeaderFilters.push_back(filterDef.toObject()["filter"].toString().toLocal8Bit());
    }
    else if (json.contains("eventHeaderFilter"))
    {
        // Old style: a single filter string.
        mm.eventHeaderFilters.push_back(json["eventHeaderFilter"].toString().toLocal8Bit());
    }

    return mm;
}

std::vector<ModuleMeta> read_module_templates(const QString &templatesDir)
{
    std::vector<ModuleMeta> result;
    const QDir baseDir(templatesDir);

    for (const auto &dirName: baseDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Readable))
    {
        QDir moduleDir(baseDir.filePath(dirName));

        if (moduleDir.exists(QStringLiteral("module_info.json")))
        {
            // Old style: module_info.json plus separate .vme files.
            auto path = moduleDir.filePath(QStringLiteral("module_info.json"));
            auto moduleInfo = read_json_file(path);

            if (moduleInfo.isEmpty())
                continue;

            auto mm = modulemeta_from_json(moduleInfo.object());
            mm.source = path;
            result.push_back(mm);
        }
        else
        {
            // New style: one or more .mvmemodule files
            // (read_modulemeta_from_mvmemodule_file, template_system.cc:266-278).
            for (const auto &c: moduleDir.entryList({ "*.mvmemodule" }, QDir::Files, QDir::Name))
            {
                auto path = moduleDir.filePath(c);
                auto moduleJson = read_json_file(path).object();
                auto mm = modulemeta_from_json(
                    moduleJson["ModuleConfig"].toObject()["ModuleMeta"].toObject());
                mm.source = path;
                result.push_back(mm);
            }
        }
    }

    return result;
}

std::optional<ModuleMeta> find_module_meta(const std::vector<ModuleMeta> &metas,
                                           const QString &typeName)
{
    for (const auto &mm: metas)
        if (mm.typeName == typeName)
            return mm;

    return {};
}

}
