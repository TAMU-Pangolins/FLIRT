#ifndef FLIRT_INTEGRITY_H
#define FLIRT_INTEGRITY_H

// Input integrity checks (docs/design.md, "Input integrity, strict by default"): the CRC-32 and length of ZIP
// entries as FLIRT reads them, and the identity of input files.

#include <cstdint>
#include <ctime>
#include <stdexcept>
#include <string>
#include <sys/types.h>
#include <mesytec-mvlc/mvlc_listfile_zip.h>

namespace flirt
{

// Names of the checks in the conversion report.
namespace checks
{
constexpr auto Archive = "I1-archive";                  // archive and central directory readable
constexpr auto ListfileEntry = "I2-listfile-entry";     // CRC-32 and length of the listfile as replayed
constexpr auto AnalysisEntry = "I3-analysis-entry";     // same for analysis.analysis
constexpr auto Parser = "I4-parser";                    // no parser exceptions, every parse result Ok
constexpr auto InputUnchanged = "I5-input-unchanged";   // size, mtime, inode before and after
constexpr auto OutputsNotInputs = "I6-outputs-not-inputs";
constexpr auto DataQuality = "data-quality";            // warnings only: counters describing the recording
constexpr auto WaveformConfig = "waveform-config";      // warnings only: VME sampling setting vs analysis vs traces
}

// Thrown when a check fails in a way that leaves nothing to convert, e.g. an
// unreadable archive. `check` names the check as in the conversion report,
// `result` is "fail" or "unavailable".
struct integrity_failure: std::runtime_error
{
    std::string check;
    std::string result;

    integrity_failure(const std::string &check_, const std::string &result_, const std::string &detail)
        : std::runtime_error(detail), check(check_), result(result_)
    { }
};

// A ZIP entry as the central directory declares it and as reading it
// delivered it. mesytec-mvlc's ZipReader accepts CRC errors
// (mvlc_listfile_zip.cc:836-842), so FLIRT compares the two itself.
struct EntryCheck
{
    std::string entry;          // empty: the entry was not read
    bool declaredKnown = false;
    uint32_t declaredCrc = 0;
    uint64_t declaredSize = 0;
    uint32_t crc = 0;           // CRC-32 of the bytes delivered since the last rewind
    uint64_t size = 0;          // bytes delivered since the last rewind
    bool endReached = false;    // a read returned no data
    std::string readError;      // what() of a read that threw

    // "pass", "fail" or "unavailable" (not read to its end, or nothing declared).
    std::string result() const;
    std::string detail() const;
};

// ReadHandle for one ZIP entry that computes the CRC-32 (zlib) and length of
// every byte it delivers, with no extra I/O. seek(pos) rewinds the entry and
// reads up to pos through read(), as ZipReadHandle::seek does
// (mvlc_listfile_zip.cc:561-579), so the same bytes are delivered and the CRC
// always covers one continuous pass from the start of the entry.
class CheckedReadHandle: public mesytec::mvlc::listfile::ReadHandle
{
    public:
        // Opens the entry; the declared values come from the central
        // directory minizip-ng has already read (mz_zip_reader_entry_get_info).
        CheckedReadHandle(mesytec::mvlc::listfile::ZipReader &zip, const std::string &entryName);

        size_t read(uint8_t *dest, size_t maxSize) override;
        size_t seek(size_t pos) override;

        const EntryCheck &check() const { return check_; }

    private:
        mesytec::mvlc::listfile::ReadHandle *entry_;
        EntryCheck check_;
};

// Identity of an input file, taken before and after the conversion.
struct FileIdentity
{
    dev_t dev = 0;
    ino_t ino = 0;
    off_t size = 0;
    timespec mtime = {};
};

// Throws std::runtime_error if the file cannot be stat()ed.
FileIdentity file_identity(const std::string &path);

// Size, modification time and inode are unchanged.
bool unchanged(const FileIdentity &before, const FileIdentity &after);

}

#endif
