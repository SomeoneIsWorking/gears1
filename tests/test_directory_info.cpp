// Tests for directory listing: FILE_DIRECTORY_INFORMATION as the console
// lays it out.
//
// The title opens the ROOT of its save mount ("save:\") and walks it. The
// runtime refused that outright -- "is a directory, not supported" -- which
// means a title checking whether its save location is usable was told it is
// not.
//
// The record, all big-endian:
//   +0x00 next_entry_offset   (u32)  0 marks the LAST entry
//   +0x04 file_index          (u32)
//   +0x08 creation_time       (u64)
//   +0x10 last_access_time    (u64)
//   +0x18 last_write_time     (u64)
//   +0x20 change_time         (u64)
//   +0x28 end_of_file         (u64)  size in bytes
//   +0x30 allocation_size     (u64)
//   +0x38 attributes          (u32)
//   +0x3C file_name_length    (u32)
//   +0x40 file_name           (ASCII, NOT null-terminated)
//
// next_entry_offset is the part worth testing hardest: a walker follows it
// until it reads zero, so a wrong value either truncates the listing or runs
// off the end of the buffer.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "ppc_config.h"
#include "ppc_context.h"

#include "directory_info.h"

PPCFuncMapping PPCFuncMappings[] = { { 0, nullptr } };

namespace
{

int g_failures = 0;

void Check(bool ok, const char* what)
{
    if (!ok)
    {
        printf("FAIL %s\n", what);
        ++g_failures;
    }
}

uint32_t ReadBE32(const uint8_t* p)
{
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

uint64_t ReadBE64(const uint8_t* p)
{
    return (uint64_t(ReadBE32(p)) << 32) | ReadBE32(p + 4);
}

void TestSingleEntryLayout()
{
    gears::DirectoryEntry entry;
    entry.name = "save.dat";
    entry.size = 0x1234;
    entry.directory = false;

    std::vector<uint8_t> memory(0x1000, 0xCD);
    constexpr uint32_t kAddress = 0x100;

    const uint32_t written =
        gears::WriteDirectoryEntry(memory.data(), kAddress, 0x400, entry, true);
    Check(written != 0, "single: an entry fits and is written");

    const uint8_t* d = memory.data() + kAddress;
    Check(ReadBE32(d + 0x00) == 0, "single: the last entry's next offset is 0");
    Check(ReadBE64(d + 0x28) == 0x1234, "single: end_of_file is the size");
    Check(ReadBE32(d + 0x3C) == 8, "single: file_name_length is in BYTES");
    Check(std::memcmp(d + 0x40, "save.dat", 8) == 0, "single: the name at +0x40");

    // A directory must be marked as one, or a title walking the tree treats it
    // as a file and tries to read it.
    Check((ReadBE32(d + 0x38) & gears::kFileAttributeDirectory) == 0,
        "single: a regular file is not marked as a directory");
}

void TestDirectoryAttribute()
{
    gears::DirectoryEntry entry;
    entry.name = "subdir";
    entry.directory = true;

    std::vector<uint8_t> memory(0x1000, 0);
    gears::WriteDirectoryEntry(memory.data(), 0x100, 0x400, entry, true);
    Check((ReadBE32(memory.data() + 0x100 + 0x38) &
           gears::kFileAttributeDirectory) != 0,
        "attr: a directory carries the directory attribute");
}

// The chaining rule: every entry but the last points at the next, and the
// offsets must be 8-byte aligned or the guest's 64-bit fields are misaligned.
void TestChaining()
{
    std::vector<gears::DirectoryEntry> entries(3);
    entries[0].name = "a";
    entries[1].name = "bb";
    entries[2].name = "ccc";

    std::vector<uint8_t> memory(0x1000, 0xCD);
    constexpr uint32_t kAddress = 0x100;

    const uint32_t used = gears::WriteDirectoryListing(
        memory.data(), kAddress, 0x400, entries.data(), entries.size());
    Check(used != 0, "chain: the listing is written");

    uint32_t offset = kAddress;
    for (size_t i = 0; i < entries.size(); ++i)
    {
        const uint8_t* d = memory.data() + offset;
        const uint32_t nameLength = ReadBE32(d + 0x3C);
        Check(nameLength == entries[i].name.size(),
            "chain: each entry carries its own name length");
        Check(std::memcmp(d + 0x40, entries[i].name.data(), nameLength) == 0,
            "chain: each entry carries its own name");

        const uint32_t next = ReadBE32(d + 0x00);
        if (i + 1 == entries.size())
        {
            Check(next == 0, "chain: the final entry terminates with 0");
        }
        else
        {
            Check(next != 0, "chain: a non-final entry points onwards");
            Check((next & 7) == 0, "chain: offsets are 8-byte aligned");
            offset += next;
        }
    }
}

// A buffer too small for even one entry must be refused, and a buffer that fits
// some entries must stop cleanly rather than write a partial record.
void TestBufferLimits()
{
    gears::DirectoryEntry entry;
    entry.name = "a_long_file_name.dat";

    std::vector<uint8_t> memory(0x1000, 0xCD);
    constexpr uint32_t kAddress = 0x100;

    Check(gears::WriteDirectoryEntry(memory.data(), kAddress, 0x10, entry, true) == 0,
        "limits: an entry that does not fit is refused");
    Check(memory[kAddress] == 0xCD, "limits: and nothing is written");

    std::vector<gears::DirectoryEntry> entries(4);
    for (size_t i = 0; i < entries.size(); ++i)
        entries[i].name = std::string(20, char('a' + i));

    // Room for two entries only.
    const uint32_t oneEntry = 0x40 + 24;
    const uint32_t used = gears::WriteDirectoryListing(
        memory.data(), kAddress, oneEntry * 2 + 8, entries.data(), entries.size());
    Check(used != 0, "limits: a partial listing still returns what fit");

    // Whatever fit must still be a well-formed chain ending in zero.
    uint32_t offset = kAddress;
    uint32_t seen = 0;
    for (;;)
    {
        const uint32_t next = ReadBE32(memory.data() + offset);
        ++seen;
        if (next == 0)
            break;
        offset += next;
        Check(seen < 10, "limits: the chain terminates");
        if (seen >= 10)
            break;
    }
    Check(seen >= 1 && seen < entries.size(),
        "limits: fewer entries than asked for, and the chain still terminates");
}

} // namespace

int main()
{
    TestSingleEntryLayout();
    TestDirectoryAttribute();
    TestChaining();
    TestBufferLimits();

    if (g_failures == 0)
    {
        printf("all directory info tests passed\n");
        return 0;
    }
    printf("%d directory info test(s) FAILED\n", g_failures);
    return 1;
}
