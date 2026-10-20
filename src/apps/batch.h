#ifndef FLIRT_APPS_BATCH_H
#define FLIRT_APPS_BATCH_H

#include <string>

namespace flirt
{

// The batch parent of flirt-convert: `flirt-convert <input>... -o <outdir> ...`.
// Checks the arguments, skips runs already converted, refuses runs with
// existing outputs, then converts each remaining listfile in a child process
// (`flirt-convert --convert-one ...`), at most -j at a time. Prints a block
// per run and the end on stderr (stdout carries only --json-events) and
// returns the exit code. Never initializes ROOT.
int run_batch(int argc, char *argv[]);

// The platform this flirt-convert was built for, "linux-x86_64" or
// "macos-arm64": the Mac and Linux may differ in fTsumwx2 (docs/design.md,
// "macOS (Apple silicon)"). Recorded in the report's "flirt" block for the
// skip rule.
std::string build_platform();

// The sha256 of this flirt-convert executable, read once; empty if it cannot
// be read. The skip rule compares it for a development build (a git describe
// string that is empty or ends in -dirty).
const std::string &executable_sha256();

}

#endif
