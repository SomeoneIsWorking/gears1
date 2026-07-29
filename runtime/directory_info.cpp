#include "directory_info.h"

#include <cstring>

namespace gears
{
namespace
{

void StoreBE32(uint8_t* p, uint32_t v)
{
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v);
}

void StoreBE64(uint8_t* p, uint64_t v)
{
    StoreBE32(p, uint32_t(v >> 32));
    StoreBE32(p + 4, uint32_t(v));
}

// Records carry 64-bit fields, so each one has to start 8-byte aligned or the
// guest reads them split across a boundary.
uint32_t RecordSize(const DirectoryEntry& entry)
{
    const uint32_t raw = kDirectoryInfoHeaderSize + uint32_t(entry.name.size());
    return (raw + 7) & ~7u;
}

} // namespace

uint32_t WriteDirectoryEntry(uint8_t* guestBase, uint32_t address,
                             uint32_t available, const DirectoryEntry& entry,
                             bool last)
{
    const uint32_t size = RecordSize(entry);
    if (size > available)
        return 0;

    uint8_t* record = guestBase + address;
    std::memset(record, 0, size);

    // Zero terminates the chain; anything else is the distance to the next
    // record, which is what a walker adds to its cursor.
    StoreBE32(record + 0x00, last ? 0 : size);
    StoreBE32(record + 0x04, 0); // file index

    // Timestamps are left zero rather than invented. A made-up modification
    // time is worse than an obviously-unset one: a title that sorts saves by
    // date would order them by a number this runtime made up.
    StoreBE64(record + 0x28, entry.size); // end of file
    StoreBE64(record + 0x30, entry.size); // allocation size

    StoreBE32(record + 0x38, entry.directory ? kFileAttributeDirectory
                                             : kFileAttributeNormal);
    StoreBE32(record + 0x3C, uint32_t(entry.name.size()));
    std::memcpy(record + 0x40, entry.name.data(), entry.name.size());

    return size;
}

uint32_t WriteDirectoryListing(uint8_t* guestBase, uint32_t address,
                               uint32_t available, const DirectoryEntry* entries,
                               size_t count)
{
    // How many actually fit, decided BEFORE writing anything: the last record
    // written has to be marked as last, and that is only knowable once the
    // count is known.
    size_t fitting = 0;
    uint32_t needed = 0;
    for (size_t i = 0; i < count; ++i)
    {
        const uint32_t size = RecordSize(entries[i]);
        if (needed + size > available)
            break;
        needed += size;
        ++fitting;
    }

    if (fitting == 0)
        return 0;

    uint32_t offset = 0;
    for (size_t i = 0; i < fitting; ++i)
    {
        const uint32_t written =
            WriteDirectoryEntry(guestBase, address + offset, available - offset,
                                entries[i], i + 1 == fitting);
        if (written == 0)
            break;
        offset += written;
    }
    return offset;
}

} // namespace gears
