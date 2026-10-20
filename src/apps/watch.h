#ifndef FLIRT_APPS_WATCH_H
#define FLIRT_APPS_WATCH_H

// Watch-folder mode of flirt-convert (`--watch <dir>`, docs/design.md, "Watch
// mode"): finds the listfile archives mvme has finished writing, and the free
// space check. batch.cc converts what it finds.

#include <chrono>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <sys/types.h>
#include <vector>

namespace flirt
{

using WatchClock = std::chrono::steady_clock;

// A file's identity as a scan sees it, without opening it.
struct FileSignature
{
    ino_t inode = 0;
    off_t size = 0;
    int64_t mtimeNs = 0;

    bool operator==(const FileSignature &o) const
    { return inode == o.inode && size == o.size && mtimeNs == o.mtimeNs; }
    bool operator!=(const FileSignature &o) const { return !(*this == o); }
};

// True if mesytec-mvlc's ZipReader::openArchive reads the archive's central
// directory: mvme writes it last, when it closes the run
// (mvlc_listfile_zip.cc:424-433, mz_zip_writer_close). Read-only, prints nothing.
// dataBytes: the uncompressed size the central directory declares for the
// first listfile entry, 0 if unknown.
bool archive_is_finished(const std::string &path, uint64_t *dataBytes = nullptr);

// f_bavail * f_frsize of the file system holding dir; false with the error.
bool free_bytes(const std::string &dir, uint64_t &bytes, std::string &error);

// "12.3 GB" (10^9 bytes, one decimal).
std::string format_gb(uint64_t bytes);

class Watcher
{
    public:
        struct Ready
        {
            uint64_t id;        // pass to done()
            std::string path;
            uint64_t dataBytes; // the listfile entry's uncompressed size, 0 if unknown
        };

        Watcher(const std::string &dir, double settleSeconds);

        // Reads the directory (readdir and stat only) and checks the
        // candidates that have been unchanged for the settle time. Lines to
        // print go to `lines`, files that became ready to `ready` (natural
        // order), ids of busy files that disappeared to `gone`.
        void scan(WatchClock::time_point now, std::vector<std::string> &lines,
                  std::vector<Ready> &ready, std::vector<uint64_t> &gone);

        // The file handed out as ready is handled (converted, skipped, refused,
        // failed, ...): ignored while it keeps the signature it had then.
        void done(uint64_t id);

        // "<runId> being written (<size>)" for the unfinished candidate that
        // changed last, if within the last minute; empty otherwise.
        std::string being_written(WatchClock::time_point now) const;

        // At a stop: the candidates that are not finished (checking the ones
        // not checked since their last change), and the finished ones not yet
        // handed out.
        void at_stop(std::vector<std::string> &unfinished, std::vector<std::string> &finished);

    private:
        enum class State { Waiting, NotFinished, Busy, Handled };

        struct Candidate
        {
            uint64_t id = 0;
            FileSignature sig, readySig;
            WatchClock::time_point stableSince;
            WatchClock::time_point lastChange;     // seen changing by a scan
            bool changed = false;
            State state = State::Waiting;
            uint64_t dataBytes = 0;     // when it became ready
        };

        std::string dir_;
        std::chrono::duration<double> settle_;
        uint64_t nextId_ = 1;
        std::map<std::string, Candidate> candidates_;   // by file name
        std::set<std::string> toldNotFinished_;         // once per file per session
        bool unreadable_ = false;
};

}

#endif
