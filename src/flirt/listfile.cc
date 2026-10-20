#include "flirt/listfile.h"
#include "common/names.h"

#include <algorithm>
#include <memory>
#include <mesytec-mvlc/mvlc_listfile.h>
#include <mesytec-mvlc/mvlc_listfile_zip.h>

namespace mvlc = mesytec::mvlc;

namespace flirt
{

namespace
{

bool ends_with(const std::string &s, const std::string &suffix)
{
    return s.size() >= suffix.size()
        && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Reads a whole entry, recording its CRC-32 and length (I3). A read error is
// recorded too; the caller decides what a damaged entry means.
std::string read_entry(mvlc::listfile::ZipReader &zip, const std::string &name, EntryCheck &check)
{
    auto entries = zip.entryNameList();

    if (std::find(entries.begin(), entries.end(), name) == entries.end())
        return {};

    std::unique_ptr<CheckedReadHandle> rh;
    std::string result;
    char buf[64 * 1024];

    try
    {
        rh = std::make_unique<CheckedReadHandle>(zip, name);

        while (size_t n = rh->read(reinterpret_cast<uint8_t *>(buf), sizeof(buf)))
            result.append(buf, n);
    }
    catch (const std::exception &e)
    {
        if (!rh) // the entry could not be opened; read errors are in rh->check()
        {
            check = {};
            check.entry = name;
            check.readError = e.what();
        }
    }

    if (rh)
        check = rh->check();

    return result;
}

// Ends in _part + 3 digits + .mvlclst.
bool is_split_part_entry(const std::string &name)
{
    const std::string suffix = ".mvlclst";
    if (!ends_with(name, suffix) || name.size() < suffix.size() + 8)
        return false;
    const auto part = name.size() - suffix.size() - 8;
    return name.compare(part, 5, "_part") == 0
        && std::all_of(name.begin() + part + 5, name.begin() + part + 8,
                       [](unsigned char c) { return c >= '0' && c <= '9'; });
}

std::string section_contents(const mvlc::listfile::Preamble &preamble, uint8_t subtype)
{
    auto sysEvent = preamble.findSystemEvent(subtype);

    if (!sysEvent)
        return {};

    return std::string(reinterpret_cast<const char *>(sysEvent->contents.data()),
                       sysEvent->contents.size());
}

}

ListfileInfo read_listfile_info(const std::string &archivePath)
{
    ListfileInfo result;
    // Absolute: the archive is opened again later, perhaps while another
    // thread has changed the working directory (root_paths.h).
    result.archivePath = absolute_path(archivePath);
    result.runId = run_id_from_path(archivePath);

    mvlc::listfile::ZipReader zip;

    try
    {
        zip.openArchive(result.archivePath);
    }
    catch (const std::exception &e)
    {
        throw integrity_failure(checks::Archive, "fail",
                                "cannot read the ZIP archive or its central directory ("
                                + std::string(e.what()) + "): if mvme is still writing this run,"
                                " it is not finished yet; otherwise mvme's listfile recovery"
                                " (mvme_listfile_recovery_gui) may be able to recover the data");
    }

    result.archiveEntries = zip.entryNameList().size();

    // First entry with a listfile extension (mvme listfile_replay.cc:111-125).
    for (const auto &name: zip.entryNameList())
    {
        if (ends_with(name, ".mvmelst") || ends_with(name, ".mvlclst")
            || ends_with(name, ".mvlclst.lz4") || ends_with(name, ".mvlc_eth")
            || ends_with(name, ".mvlc_usb"))
        {
            result.listfileEntry = name;
            break;
        }
    }

    if (result.listfileEntry.empty())
        throw unsupported_input("no listfile found inside " + archivePath);

    // mvme's listfile splitting names the parts' entries <prefix>_partNNN.mvlclst
    // (mvlc_listfile_zip.cc:466) and replays all parts as one run
    // (mvlc_listfile_worker.cc:243, SplitZipReader); one part alone is not that run.
    if (is_split_part_entry(result.listfileEntry))
        throw unsupported_input(result.listfileEntry + " is one part of a run split by mvme's listfile"
                                " splitting; split runs are not supported yet (mvme replays the parts as"
                                " one run): turn listfile splitting off in mvme");

    if (ends_with(result.listfileEntry, ".mvmelst"))
        throw unsupported_input("classic .mvmelst listfiles are not supported (MVLC only): "
                                + result.listfileEntry);

    // The ZIP layer's CRC-32 covers the LZ4 frames, not the data the reader
    // delivers, so the listfile cannot be verified (docs/design.md, "Input
    // integrity, strict by default").
    if (ends_with(result.listfileEntry, ".lz4"))
        throw unsupported_input("LZ4-compressed listfiles are not supported: their integrity cannot"
                                " be verified at the ZIP layer (" + result.listfileEntry + ")");

    result.analysisJson = read_entry(zip, "analysis.analysis", result.analysisEntry);
    // Added at the stop with the analysis (mvlc_readout_worker.cc:497-514);
    // read for the settings-changes check only (settings_changes.h).
    result.messagesLog = read_entry(zip, "messages.log", result.messagesLogEntry);

    // The preamble is read again, and CRC-checked, by the replay (I2); read
    // errors here are reported as such.
    std::unique_ptr<CheckedReadHandle> rh;
    mvlc::listfile::Preamble preamble;

    try
    {
        rh = std::make_unique<CheckedReadHandle>(zip, result.listfileEntry);
        preamble = mvlc::listfile::read_preamble(*rh);
    }
    catch (const std::exception &e)
    {
        if (!rh || !rh->check().readError.empty())
            throw integrity_failure(checks::ListfileEntry, "fail", "cannot read '"
                                    + result.listfileEntry + "': " + e.what());
        throw;
    }

    // Format detection (listfile_replay.cc:141-147).
    if (preamble.magic == mvlc::listfile::get_filemagic_eth())
        result.format = mvlc::ConnectionType::ETH;
    else if (preamble.magic == mvlc::listfile::get_filemagic_usb())
        result.format = mvlc::ConnectionType::USB;
    else
        throw unsupported_input("unknown listfile format in " + result.listfileEntry);

    result.vmeConfigJson = section_contents(preamble, mvlc::system_event::subtype::MVMEConfig);
    result.crateConfigYaml = section_contents(preamble, mvlc::system_event::subtype::MVLCCrateConfig);

    if (result.vmeConfigJson.empty())
        throw unsupported_input("listfile has no MVMEConfig section");
    if (result.crateConfigYaml.empty())
        throw unsupported_input("listfile has no MVLCCrateConfig section");

    return result;
}

}
