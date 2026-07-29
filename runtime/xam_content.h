// Saved-game content: what exists on this machine, and how it is described to
// the title.
//
// On the console a save is a package on a storage device, found by enumerating
// the device. Here it is a directory next to the player's other data, and the
// enumeration walks the filesystem -- but the descriptor handed back is the
// console's XCONTENT_DATA, byte for byte, because the title indexes into it.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace gears
{

// XCONTENT_DATA, 0x134 bytes:
//   +0x000 device_id     (u32)
//   +0x004 content_type  (u32)
//   +0x008 display_name  (128 UTF-16 characters)
//   +0x108 file_name     (42 bytes, ASCII)
//   +0x132 padding       (2 bytes)
constexpr uint32_t kContentDataSize = 0x134;
constexpr uint32_t kContentDisplayNameChars = 128;
constexpr uint32_t kContentFileNameBytes = 42;

// The single storage device this runtime reports. Any non-zero id will do, but
// it has to be the SAME one everywhere: a title that enumerates content on one
// device and then opens it on another finds nothing.
constexpr uint32_t kContentDeviceId = 1;

// XCONTENTFLAG creation dispositions, as the console numbers them (transcribed
// in extern/xenia/src/xenia/vfs/devices/stfs_xbox.h). These were previously
// written as kCreateNew=1, kOpenExisting=2 -- and the title passes flags 0x13,
// whose low nibble is 3. So every OPEN_EXISTING request fell through the test
// for it and was answered as though it were a creation, which is why a save
// that does not exist was reported "mounted (new)" to a caller that had asked
// to open an existing one.
enum ContentCreateDisposition : uint32_t
{
    kContentCreateNew = 1,
    kContentCreateAlways = 2,
    kContentOpenExisting = 3,
    kContentOpenAlways = 4,
    kContentTruncateExisting = 5,
};

// What a create/open request should answer, given whether the content is there.
// Separated from the guest seam so the disposition table can be tested: the
// numbers are the console's and nothing else in the runtime can catch them
// being wrong.
enum class ContentDecision
{
    Open,          // use what is there
    Create,        // make it
    NotFound,      // asked to open something that does not exist
    AlreadyExists, // asked to create something that does
    Invalid,
};

ContentDecision DecideContentCreate(uint32_t flags, bool existed);

struct ContentEntry
{
    uint32_t deviceId = kContentDeviceId;
    uint32_t contentType = 0;
    std::string fileName;        // the directory name, and the title's key
    std::u16string displayName;  // what a player would see
};

// Content EXISTS when it holds something. Testing for the directory is wrong:
// the runtime creates it itself the first time the title opens a file for
// writing under the mount, so a directory test answers "yes" for every save
// that was only ever opened and never written.
bool ContentExists(const std::filesystem::path& directory);

// Every piece of content under `saveRoot`, sorted by name so that two
// enumerations agree -- directory iteration order is not guaranteed, and a
// title that walks a list twice must see the same list.
std::vector<ContentEntry> EnumerateContent(const std::filesystem::path& saveRoot,
                                           uint32_t contentType);

// Projects one entry into the guest's XCONTENT_DATA. Names longer than their
// fields are truncated rather than allowed to run into the next record.
void WriteContentData(uint8_t* guestBase, uint32_t address,
                      const ContentEntry& entry);

} // namespace gears
