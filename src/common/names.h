#ifndef FLIRT_COMMON_NAMES_H
#define FLIRT_COMMON_NAMES_H

// File name rules shared by flirt-convert and flirt-tui.

#include <string>

namespace flirt
{

// mvme's run ID for a replay: QFileInfo(archivePath).completeBaseName()
// (mvme_context.cc:1814).
std::string run_id_from_path(const std::string &archivePath);

// `path` made absolute against the working directory, without resolving
// symlinks or "..": a later chdir (root_paths.h) cannot change what it names.
std::string absolute_path(const std::string &path);

}

#endif
