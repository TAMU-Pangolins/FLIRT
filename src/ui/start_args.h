#ifndef FLIRT_UI_START_ARGS_H
#define FLIRT_UI_START_ARGS_H

// The start request's options (the body flirt-tui builds) -> flirt-convert's
// arguments (docs/design.md, "TUI").

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace flirt
{

// The start request's options -> flirt-convert's arguments (after
// --json-events), with a flag for each option that is not its default.
// Empty with `error` set if the body is malformed.
std::vector<std::string> start_arguments(const nlohmann::json &body, std::string &error);

}

#endif
