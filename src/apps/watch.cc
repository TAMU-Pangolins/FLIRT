// Watch-folder mode of flirt-convert (docs/design.md, "Watch mode").
//
// mvme writes a run with mesytec-mvlc's SplitZipCreator
// (mvlc_readout_worker.cc:485-526): the listfile entry grows during the run,
// and at the stop closeArchive() closes it, the closeArchiveCallback adds
// messages.log, analysis.analysis and mvme_run_notes.txt, and
// mz_zip_writer_close writes the central directory last
// (mvlc_listfile_zip.cc:424-433). So an archive whose central directory reads
// is complete; a running or crashed recording has none.

#include "watch.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <mesytec-mvlc/mvlc_listfile_zip.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "common/names.h"
#include "common/platform.h"
#include "flirt/listfile.h"

namespace flirt
{

namespace
{

std::string format_size(uint64_t bytes)
{
    char text[32];
    if (bytes >= 1000000000u)
        std::snprintf(text, sizeof(text), "%.1f GB", static_cast<double>(bytes) / 1e9);
    else
        std::snprintf(text, sizeof(text), "%.1f MB", static_cast<double>(bytes) / 1e6);
    return text;
}

}

bool archive_is_finished(const std::string &path, uint64_t *dataBytes)
{
    // The same open as the conversion's I1 (listfile.cc). ZipReader opens the
    // file with MZ_OPEN_MODE_READ and logs nothing; its destructor closes it.
    mesytec::mvlc::listfile::ZipReader zip;
    try
    {
        zip.openArchive(path);
    }
    catch (const std::exception &)
    {
        return false;
    }

    // Only a size estimate: a damaged entry is the conversion's to report (I2).
    if (dataBytes)
    {
        *dataBytes = 0;
        try
        {
            const auto name = zip.firstListfileEntryName();
            if (!name.empty())
            {
                zip.openEntry(name);
                *dataBytes = zip.entryInfo().uncompressedSize;
                zip.closeCurrentEntry();
            }
        }
        catch (const std::exception &)
        {
            *dataBytes = 0;
        }
    }
    return true;
}

bool free_bytes(const std::string &dir, uint64_t &bytes, std::string &error)
{
    struct statvfs st;
    if (statvfs(dir.c_str(), &st) != 0)
    {
        error = std::strerror(errno);
        return false;
    }
    bytes = static_cast<uint64_t>(st.f_bavail) * static_cast<uint64_t>(st.f_frsize);
    return true;
}

std::string format_gb(uint64_t bytes)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f GB", static_cast<double>(bytes) / 1e9);
    return text;
}

Watcher::Watcher(const std::string &dir, double settleSeconds)
    : dir_(dir), settle_(settleSeconds)
{ }

void Watcher::scan(WatchClock::time_point now, std::vector<std::string> &lines,
                   std::vector<Ready> &ready, std::vector<uint64_t> &gone)
{
    DIR *dir = opendir(dir_.c_str());
    if (!dir)
    {
        if (!unreadable_)
            lines.push_back("error: cannot read " + dir_ + ": " + std::strerror(errno) + "; still trying");
        unreadable_ = true;
        return;
    }
    if (unreadable_)
        lines.push_back("watching " + dir_ + " again");
    unreadable_ = false;

    // The batch's directory rule (add_input): *.zip regular files, no names
    // starting with '.', not recursive.
    std::map<std::string, FileSignature> seen;
    while (auto entry = readdir(dir))
    {
        const std::string name = entry->d_name;
        struct stat st;
        if (is_listfile_name(name) && stat((dir_ + "/" + name).c_str(), &st) == 0 && S_ISREG(st.st_mode))
            seen[name] = { st.st_ino, st.st_size,
                           static_cast<int64_t>(stat_mtime(st).tv_sec) * 1000000000 + stat_mtime(st).tv_nsec };
    }
    closedir(dir);

    for (auto it = candidates_.begin(); it != candidates_.end();)
    {
        if (seen.count(it->first))
        {
            ++it;
            continue;
        }
        if (it->second.state == State::Busy)
            gone.push_back(it->second.id);
        it = candidates_.erase(it);
    }

    std::vector<std::string> readyNames;
    for (const auto &[name, sig]: seen)
    {
        auto found = candidates_.find(name);
        if (found == candidates_.end())
        {
            Candidate c;
            c.id = nextId_++;
            c.sig = sig;
            c.stableSince = now;
            candidates_.emplace(name, c);
            continue;
        }

        auto &c = found->second;
        if (c.sig != sig)
        {
            c.sig = sig;
            c.stableSince = c.lastChange = now;
            c.changed = true;
            if (c.state == State::NotFinished || c.state == State::Handled)
                c.state = State::Waiting;
        }
        if (c.state != State::Waiting || now - c.stableSince < settle_)
            continue;

        if (archive_is_finished(dir_ + "/" + name, &c.dataBytes))
        {
            c.state = State::Busy;
            c.readySig = sig;
            readyNames.push_back(name);
        }
        else
        {
            c.state = State::NotFinished;
            if (toldNotFinished_.insert(name).second)
                lines.push_back(run_id_from_path(name) + ": not finished (no ZIP central directory yet):"
                                " mvme is still writing it or was stopped without closing it;"
                                " it is converted once mvme closes it");
        }
    }

    std::sort(readyNames.begin(), readyNames.end(), natural_less);
    for (const auto &name: readyNames)
        ready.push_back({ candidates_[name].id, dir_ + "/" + name, candidates_[name].dataBytes });
}

void Watcher::done(uint64_t id)
{
    for (auto &[name, c]: candidates_)
        if (c.id == id && c.state == State::Busy)
            c.state = c.sig == c.readySig ? State::Handled : State::Waiting;
}

std::string Watcher::being_written(WatchClock::time_point now) const
{
    const std::pair<const std::string, Candidate> *latest = nullptr;
    for (const auto &entry: candidates_)
        if ((entry.second.state == State::Waiting || entry.second.state == State::NotFinished)
            && entry.second.changed && (!latest || entry.second.lastChange > latest->second.lastChange))
            latest = &entry;
    if (!latest || now - latest->second.lastChange > std::chrono::minutes(1))
        return {};
    return run_id_from_path(latest->first) + " being written ("
        + format_size(static_cast<uint64_t>(latest->second.sig.size)) + ")";
}

void Watcher::at_stop(std::vector<std::string> &unfinished, std::vector<std::string> &finished)
{
    std::vector<std::string> names;
    for (const auto &[name, c]: candidates_)
        if (c.state == State::Waiting || c.state == State::NotFinished)
            names.push_back(name);
    std::sort(names.begin(), names.end(), natural_less);

    for (const auto &name: names)
    {
        const auto path = dir_ + "/" + name;
        if (candidates_[name].state == State::NotFinished || !archive_is_finished(path))
            unfinished.push_back(path);
        else
            finished.push_back(path);
    }
}

}
