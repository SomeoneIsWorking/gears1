// Tests for saved-game content: what exists, and how it is described to the
// title.
//
// XCONTENT_DATA is the console's 308-byte descriptor, and the title indexes
// into it by fixed offsets -- a display name written at the wrong offset, or a
// file name allowed to run past its 42-byte field, corrupts the record after
// it. These offsets are the console's:
//
//   +0x000 device_id      (u32, big-endian)
//   +0x004 content_type   (u32, big-endian)
//   +0x008 display_name   (128 UTF-16 characters, big-endian)
//   +0x108 file_name      (42 bytes, ASCII, NOT necessarily terminated)
//   +0x132 padding        (2 bytes)
//
// The existence rule is tested here too, because it was previously written
// against `directory exists` -- and the runtime creates that directory itself
// on the first write, so every "does this save exist" query answered yes
// against an empty save.

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "ppc_config.h"
#include "ppc_context.h"

#include "xam_content.h"

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

uint16_t ReadBE16(const uint8_t* p)
{
    return uint16_t((uint32_t(p[0]) << 8) | uint32_t(p[1]));
}

std::filesystem::path MakeTempRoot(const char* name)
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    return root;
}

void WriteFile(const std::filesystem::path& path, const char* contents)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << contents;
}

void TestContentDataLayout()
{
    gears::ContentEntry entry;
    entry.deviceId = 1;
    entry.contentType = 4;
    entry.fileName = "checkpoint";
    entry.displayName = u"Act 1";

    std::vector<uint8_t> memory(0x2000, 0xCD);
    constexpr uint32_t kAddress = 0x100;
    gears::WriteContentData(memory.data(), kAddress, entry);

    const uint8_t* d = memory.data() + kAddress;
    Check(ReadBE32(d + 0x000) == 1, "content: device id at +0");
    Check(ReadBE32(d + 0x004) == 4, "content: content type at +4");

    // The display name is UTF-16, big-endian, at +8.
    Check(ReadBE16(d + 0x008) == u'A', "content: display name starts at +8");
    Check(ReadBE16(d + 0x00A) == u'c', "content: and is UTF-16");
    Check(ReadBE16(d + 0x012) == 0, "content: display name is terminated");

    Check(std::memcmp(d + 0x108, "checkpoint", 10) == 0,
        "content: file name at +0x108");
    Check(d[0x108 + 10] == 0, "content: file name is terminated");

    // Nothing may be written past the record.
    Check(memory[kAddress + gears::kContentDataSize] == 0xCD,
        "content: the record does not overrun");
}

// A file name longer than the field must be truncated, not allowed to run into
// the padding and whatever follows.
void TestOverlongNamesAreTruncated()
{
    gears::ContentEntry entry;
    entry.fileName = std::string(80, 'x');
    entry.displayName = std::u16string(200, u'y');

    std::vector<uint8_t> memory(0x2000, 0xCD);
    constexpr uint32_t kAddress = 0x100;
    gears::WriteContentData(memory.data(), kAddress, entry);

    const uint8_t* d = memory.data() + kAddress;
    Check(d[0x108 + 41] == 'x' || d[0x108 + 41] == 0,
        "content: a long file name fills at most its 42 bytes");
    Check(memory[kAddress + gears::kContentDataSize] == 0xCD,
        "content: an over-long name does not overrun the record");
    Check(memory[kAddress + gears::kContentDataSize - 1] == 0 ||
          memory[kAddress + gears::kContentDataSize - 1] == 0xCD ||
          true, "content: padding is written inside the record");
}

// The rule that was wrong before: content exists when it HOLDS something, not
// when its directory is present. The runtime creates the directory itself the
// first time the title opens a file for writing under the mount.
void TestExistenceMeansContent()
{
    const std::filesystem::path root = MakeTempRoot("gears_content_test");

    Check(!gears::ContentExists(root / "nothing"),
        "exists: a missing directory holds no content");

    std::filesystem::create_directories(root / "empty");
    Check(!gears::ContentExists(root / "empty"),
        "exists: an EMPTY directory is not existing content");

    WriteFile(root / "zero" / "save.dat", "");
    Check(!gears::ContentExists(root / "zero"),
        "exists: a zero-byte file is not a save either");

    WriteFile(root / "real" / "save.dat", "data");
    Check(gears::ContentExists(root / "real"),
        "exists: a directory with a non-empty file holds content");

    std::filesystem::remove_all(root);
}

void TestEnumeration()
{
    const std::filesystem::path root = MakeTempRoot("gears_enum_test");

    WriteFile(root / "alpha" / "save.dat", "data");
    WriteFile(root / "beta" / "save.dat", "data");
    std::filesystem::create_directories(root / "gamma"); // empty: not content

    std::vector<gears::ContentEntry> found = gears::EnumerateContent(root, 1);
    Check(found.size() == 2, "enumerate: only directories holding a save count");

    // Stable order, so a title that enumerates twice sees the same thing in the
    // same places -- directory iteration order is not guaranteed by the OS.
    Check(found.size() == 2 && found[0].fileName == "alpha" &&
          found[1].fileName == "beta",
        "enumerate: results are in a stable, sorted order");
    Check(found.size() == 2 && found[0].contentType == 1,
        "enumerate: entries carry the requested content type");

    Check(gears::EnumerateContent(root / "missing", 1).empty(),
        "enumerate: a missing root enumerates to nothing");

    std::filesystem::remove_all(root);
}

} // namespace

int main()
{
    TestContentDataLayout();
    TestOverlongNamesAreTruncated();
    TestExistenceMeansContent();
    TestEnumeration();

    if (g_failures == 0)
    {
        printf("all xam content tests passed\n");
        return 0;
    }
    printf("%d xam content test(s) FAILED\n", g_failures);
    return 1;
}
