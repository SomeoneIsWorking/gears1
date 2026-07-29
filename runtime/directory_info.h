// Directory listings, in the console's FILE_DIRECTORY_INFORMATION form.
//
// The title opens the root of its save mount and walks it. Until this existed
// the runtime refused a directory handle outright, so a title asking "what is
// in my save location" was told the location could not be opened at all.
#pragma once

#include <cstdint>
#include <string>

namespace gears
{

// X_FILE_ATTRIBUTES, the subset that matters here.
constexpr uint32_t kFileAttributeNormal = 0x00000080;
constexpr uint32_t kFileAttributeDirectory = 0x00000010;

// The fixed part of FILE_DIRECTORY_INFORMATION; the name follows at +0x40.
constexpr uint32_t kDirectoryInfoHeaderSize = 0x40;

struct DirectoryEntry
{
    std::string name;
    uint64_t size = 0;
    bool directory = false;
};

// Writes one record. `last` decides whether next_entry_offset terminates the
// chain. Returns the number of bytes used, or 0 if the record does not fit --
// in which case NOTHING is written, because a half-written record is one the
// guest cannot detect.
uint32_t WriteDirectoryEntry(uint8_t* guestBase, uint32_t address,
                             uint32_t available, const DirectoryEntry& entry,
                             bool last);

// Writes as many entries as fit, correctly chained, always terminating. Returns
// the bytes used, or 0 if not even one entry fits.
uint32_t WriteDirectoryListing(uint8_t* guestBase, uint32_t address,
                               uint32_t available, const DirectoryEntry* entries,
                               size_t count);

} // namespace gears
