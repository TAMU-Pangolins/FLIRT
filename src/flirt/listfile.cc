#include "flirt/listfile.h"
#include "common/names.h"

#include <algorithm>
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

// Reads a whole entry.
std::string read_entry(mvlc::listfile::ZipReader &zip, const std::string &name)
{
    auto entries = zip.entryNameList();

    if (std::find(entries.begin(), entries.end(), name) == entries.end())
        return {};

    auto rh = zip.openEntry(name);
    std::string result;
    char buf[64 * 1024];

    while (size_t n = rh->read(reinterpret_cast<uint8_t *>(buf), sizeof(buf)))
        result.append(buf, n);

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
    zip.openArchive(result.archivePath);

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

    result.analysisJson = read_entry(zip, "analysis.analysis");

    // The preamble is read again by the replay.
    auto rh = zip.openEntry(result.listfileEntry);
    auto preamble = mvlc::listfile::read_preamble(*rh);

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
