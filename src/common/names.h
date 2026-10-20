#ifndef FLIRT_COMMON_NAMES_H
#define FLIRT_COMMON_NAMES_H

// File name rules shared by flirt-convert and flirt-tui.

#include <string>
#include <vector>

namespace flirt
{

// Standard names (docs/design.md, "Standard names"): the names FLIRT handles
// (a listfile's, the output folder's, the files given to options) use only
// ASCII letters, digits, '_', '-', '.' and spaces; not empty, not only dots,
// not starting with '-' (it reads as an option), not starting or ending with
// a space. Folders above them may hold any name: only a path's own name is
// checked.
//
// Empty for a standard name, else why not, naming the first character that
// is not allowed: "\"#\" is not allowed".
std::string nonstandard_name_reason(const std::string &name);

// Empty for a standard name, else the one message every program prints:
// "<name, printable>: name not accepted: <reason> (use letters, digits, _ - .
// and spaces)".
std::string name_refusal(const std::string &name);

// A path's own name: its last component, trailing slashes ignored
// ("a/b/" -> "b"); empty for "/" or "".
std::string own_name(const std::string &path);

// A directory's listfile names, as the shell's glob gives them: *.zip, not
// starting with '.'.
bool is_listfile_name(const std::string &name);

// Appends an input as flirt-convert and flirt-info take it: a listfile, or a
// directory's *.zip regular files as the shell's glob gives them (no names
// starting with '.'), not recursive, in natural order. Throws
// std::runtime_error for a missing path, an unreadable directory or one
// without *.zip files.
void add_listfile_input(const std::string &path, std::vector<std::string> &inputs);

// Digit runs compare numerically ("run9" < "run10"); ties fall back to bytes.
bool natural_less(const std::string &a, const std::string &b);

// mvme's run ID for a replay: QFileInfo(archivePath).completeBaseName()
// (mvme_context.cc:1814).
std::string run_id_from_path(const std::string &archivePath);

// `path` made absolute against the working directory, without resolving
// symlinks or "..": a later chdir (root_paths.h) cannot change what it names.
std::string absolute_path(const std::string &path);

}

#endif
