#include "flirt/integrity.h"
#include "common/platform.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <vector>
#include <zlib.h>

// minizip-ng is built into mesytec-mvlc, which exports its symbols; only the
// headers are used here.
#include <mz.h>
#include <mz_strm.h>
#include <mz_zip.h>
#include <mz_zip_rw.h>

namespace flirt
{

namespace
{

std::string hex32(uint32_t value)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08x", value);
    return buf;
}

}

std::string EntryCheck::result() const
{
    if (!readError.empty())
        return "fail";
    if (!declaredKnown || !endReached)
        return "unavailable";
    if (size != declaredSize || crc != declaredCrc)
        return "fail";
    return "pass";
}

std::string EntryCheck::detail() const
{
    const std::string what = "'" + entry + "': ";

    if (!readError.empty())
        return what + "read error after " + std::to_string(size) + " bytes: " + readError;
    if (!declaredKnown)
        return what + "the central directory declares no CRC-32 and size";
    if (!endReached)
        return what + "not read to its end (" + std::to_string(size) + " bytes read)";
    if (size != declaredSize)
        return what + std::to_string(size) + " bytes read, the central directory declares "
            + std::to_string(declaredSize);
    if (crc != declaredCrc)
        return what + "CRC-32 " + hex32(crc) + " of the bytes read, the central directory declares "
            + hex32(declaredCrc);
    return what + std::to_string(size) + " bytes, CRC-32 " + hex32(crc) + " as declared";
}

CheckedReadHandle::CheckedReadHandle(mesytec::mvlc::listfile::ZipReader &zip, const std::string &entryName)
    : entry_(zip.openEntry(entryName))
{
    check_.entry = entryName;
    mz_zip_file *info = nullptr;

    if (mz_zip_reader_entry_get_info(zip.getRawReaderHandle(), &info) == MZ_OK && info
        && info->uncompressed_size >= 0)
    {
        check_.declaredKnown = true;
        check_.declaredCrc = info->crc;
        check_.declaredSize = static_cast<uint64_t>(info->uncompressed_size);
    }
}

size_t CheckedReadHandle::read(uint8_t *dest, size_t maxSize)
{
    size_t bytes = 0;

    try
    {
        bytes = entry_->read(dest, maxSize);
    }
    catch (const std::exception &e)
    {
        check_.readError = e.what();
        throw;
    }

    if (bytes)
    {
        check_.crc = crc32_z(check_.crc, dest, bytes);
        check_.size += bytes;
    }
    else if (maxSize)
        check_.endReached = true;

    return bytes;
}

size_t CheckedReadHandle::seek(size_t pos)
{
    check_.crc = 0;
    check_.size = 0;
    check_.endReached = false;
    check_.readError.clear();

    try
    {
        entry_->seek(0); // ZipReadHandle: closes and reopens the entry
    }
    catch (const std::exception &e)
    {
        check_.readError = e.what();
        throw;
    }

    // 1 MB chunks as in ZipReadHandle::seek.
    std::vector<uint8_t> buffer(std::min(pos, static_cast<size_t>(1u << 20)));
    size_t reached = 0;

    while (reached < pos)
    {
        size_t bytes = read(buffer.data(), std::min(buffer.size(), pos - reached));
        if (!bytes)
            break;
        reached += bytes;
    }

    return reached;
}

FileIdentity file_identity(const std::string &path)
{
    struct stat st;

    if (stat(path.c_str(), &st) != 0)
        throw std::runtime_error("cannot stat " + path + ": " + std::strerror(errno));

    FileIdentity result;
    result.dev = st.st_dev;
    result.ino = st.st_ino;
    result.size = st.st_size;
    result.mtime = stat_mtime(st);
    return result;
}

bool unchanged(const FileIdentity &before, const FileIdentity &after)
{
    return before.dev == after.dev && before.ino == after.ino && before.size == after.size
        && before.mtime.tv_sec == after.mtime.tv_sec && before.mtime.tv_nsec == after.mtime.tv_nsec;
}

}
