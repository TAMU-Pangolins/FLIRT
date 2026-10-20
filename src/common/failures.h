#ifndef FLIRT_COMMON_FAILURES_H
#define FLIRT_COMMON_FAILURES_H

// A run that ended badly, in plain words: the headline, why (the technical
// detail, kept, with the archive's "central directory" called its index and
// thousands separated), what to do, and the integrity check that failed.
// The one table flirt-convert's result lines and flirt-tui's details (through
// src/ui/job.cc) both use (README "Input integrity checks"). It decides
// nothing: the result and the note are flirt-convert's (the summary's
// "result" and "note"; the note of a failed check is its line
// "<check>: fail: <detail>", src/apps/flirt_convert.cc Checks::printFailures).

#include <string>

namespace flirt
{

struct Failure
{
    std::string headline;   // "the listfile in the archive is damaged"
    std::string why;        // "x.mvlclst: 13,499,868 bytes read, the archive's index says 13,499,864"
    std::string todo;       // a sentence or two
    std::string check;      // "I2-listfile-entry", or empty
};

// Where the to-do is read: flirt-tui names its options and keys,
// flirt-convert its flags.
enum class FailureFor { Tui, Cli };

// For result failed, unsupported, interrupted or outputs-exist; any other
// result gives an empty Failure. diskFull: the run's messages said "No space
// left on device" (src/apps/batch.cc finish()).
Failure explain_failure(const std::string &result, const std::string &note, bool diskFull, FailureFor where);

// A check's technical detail in plain words: "'x': N bytes read, the central
// directory declares M" -> "x: N,NNN bytes read, the archive's index says M,MMM".
std::string plain_check_detail(const std::string &detail);

// "13499868" -> "13,499,868": every run of 4 or more digits that stands
// alone (not part of a name like run042_260303 or a hex number like
// 0x1a2b3c4d).
std::string separate_thousands(const std::string &text);

}

#endif
