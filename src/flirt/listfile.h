#ifndef FLIRT_LISTFILE_H
#define FLIRT_LISTFILE_H

#include <mesytec-mvlc/mvlc_constants.h>
#include <stdexcept>
#include <string>

#include "flirt/integrity.h"

namespace flirt
{

// Thrown for any input FLIRT refuses to convert. The message says why.
struct unsupported_input: std::runtime_error
{
    using std::runtime_error::runtime_error;
};

// What mvme reads from a listfile archive when it is opened for replay.
struct ListfileInfo
{
    std::string archivePath;
    std::string listfileEntry;   // name of the listfile inside the archive
    std::string runId;           // used for output file names
    mesytec::mvlc::ConnectionType format;
    std::string vmeConfigJson;   // preamble MVMEConfig section
    std::string crateConfigYaml; // preamble MVLCCrateConfig section
    std::string analysisJson;    // "analysis.analysis" entry; empty if absent
    EntryCheck analysisEntry;    // its CRC-32 and length as read (entry empty if absent)
    std::string messagesLog;     // "messages.log" entry, mvme's log of the run; empty if absent
    EntryCheck messagesLogEntry; // its CRC-32 and length as read (entry empty if absent)
    size_t archiveEntries = 0;   // entries in the central directory
};

// Opens the archive read-only and extracts the above. Throws
// unsupported_input for non-MVLC listfiles, one part of a split run and
// missing configuration, and
// integrity_failure if the archive (I1) or the listfile entry (I2) cannot be
// read. A damaged analysis.analysis or messages.log is only recorded in
// analysisEntry or messagesLogEntry.
ListfileInfo read_listfile_info(const std::string &archivePath);

// run_id_from_path(), mvme's run ID for a replay, is in common/names.h.

}

#endif
