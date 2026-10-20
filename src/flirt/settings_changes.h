#ifndef FLIRT_SETTINGS_CHANGES_H
#define FLIRT_SETTINGS_CHANGES_H

// Settings changed through mvme during a run, found in the archive's
// messages.log: mvme's log from the DAQ start to the stop (cleared at the
// start, mvme_context.cc:1744; written at the stop, mvlc_readout_worker.cc:510;
// at most 100000 lines, the oldest dropped, mvme_context.cc:99). With an MVLC,
// mvme runs VME scripts without pausing the DAQ (mvme.cpp:2361-2376).
// FLIRT's own glue; a pure function of the log text, no Qt.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace flirt
{

struct LoggedWrite
{
    std::string address, value;     // the log's hex text, e.g. 0x00006100, 0x00000008
};

struct SettingsChange
{
    std::string time;               // HH:MM:SS, mvme's log prefix
    std::string source;             // script "<title>", script (no Running line before it), VME Debug
    std::vector<LoggedWrite> writes;
    bool writesListed = true;       // false for an aggregated script run: mvme lists no writes
    std::optional<int64_t> commands;    // aggregated run: "Executed N commands, no errors"
    std::optional<std::string> error;   // aggregated run that ended in "Error: <e>"
};

struct SettingsChanges
{
    bool checked = false;
    std::string reason;             // why not checked; empty if checked
    std::string mvmeVersion;        // from the first line; empty if absent
    std::vector<SettingsChange> changes;
};

// Scans the log. Not checked (never "no changes") when the log is not valid
// UTF-8, when either readout-loop marker is missing, or when no register write
// of the DAQ start is recognised before the window, i.e. for a log format
// FLIRT does not know.
SettingsChanges find_settings_changes(const std::string &logText);

// The stderr line: "warning: settings changed during the run: ...; see the
// report" (at most 3 changes, then "and N more"), "note: settings changes
// during the run not checked: <reason>", or empty if checked with no change.
std::string settings_changes_line(const SettingsChanges &result);

}

#endif
