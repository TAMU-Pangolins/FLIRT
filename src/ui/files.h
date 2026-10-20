#ifndef FLIRT_UI_FILES_H
#define FLIRT_UI_FILES_H

// flirt-tui's file browser: folders listed and made, a report read
// (docs/design.md, "TUI").

#include <nlohmann/json.hpp>
#include <string>

namespace flirt
{

// An API answer: an HTTP status and its JSON body ({"error": ...} if not 200).
struct ApiResult
{
    int status = 200;
    nlohmann::json body;
};

ApiResult api_error(int status, const std::string &message);

// "/a/./b//c/../d/" -> "/a/b/d", without looking at the file system; empty if
// the path is not absolute.
std::string normalize_path(const std::string &path);

// {path, parent, entries: [{name, type, size, mtime, id, listfile}]}.
ApiResult list_directory(const std::string &path);

// Creates the one directory `path` (its parent must exist) -> {path}.
ApiResult make_directory(const std::string &path);

// A conversion report's contents (a path ending in .flirt.json, at most 16 MiB).
// status 200: `contents` holds the file; otherwise `result` holds the error.
int read_report(const std::string &path, std::string &contents, ApiResult &result);

}

#endif
