// Tests for the user profile store and the XamUserReadProfileSettings buffer
// protocol.
//
// This protocol is entirely decided by the console, not by us, and it is the
// kind of thing that "looks right" while handing the title a buffer it
// misparses -- which is exactly how the runtime got a 2.6 GB allocation out of
// an FString length (catalog #45). Every offset and width below is pinned
// against the console's own structures rather than against what the code
// happens to do.
//
// The layouts, from the console (and Xenia's transcription of them):
//
//   X_USER_READ_PROFILE_SETTINGS  8 bytes:  setting_count, settings_ptr
//   X_USER_PROFILE_SETTING       40 bytes:
//       +0  source (u32)
//       +8  xuid (u64), or user_index (u32) at +8
//       +16 setting_id (u32)
//       +24 data.type (u8)
//       +32 data union (8 bytes)
//
// A setting id is not opaque: it encodes its own type and payload size as
// id:16, size:12, type:4. That is what makes sizing a read buffer possible
// without a table of every setting the title might ask for.

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "ppc_config.h"
#include "ppc_context.h"

#include "user_profile.h"

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

// A setting id, built the way the console encodes one.
constexpr uint32_t MakeSettingId(uint16_t id, uint16_t size, gears::UserDataType type)
{
    return uint32_t(id) | (uint32_t(size & 0xFFF) << 16) |
           (uint32_t(type) << 28);
}

// Real shapes: a 32-bit integer setting carries its value inline, a binary one
// carries a length and a pointer to bytes that follow the headers.
constexpr uint32_t kIntSetting =
    MakeSettingId(0x0003, 0, gears::UserDataType::Int32);
constexpr uint32_t kBinarySetting =
    MakeSettingId(0x0004, 16, gears::UserDataType::Binary);

void TestSettingKeyDecoding()
{
    const gears::SettingKey intKey = gears::SettingKey::Decode(kIntSetting);
    Check(intKey.id == 0x0003, "key: id comes from the low 16 bits");
    Check(intKey.size == 0, "key: an inline type carries no payload size");
    Check(intKey.type == gears::UserDataType::Int32, "key: type is the top 4 bits");

    const gears::SettingKey binaryKey = gears::SettingKey::Decode(kBinarySetting);
    Check(binaryKey.id == 0x0004, "key: binary id");
    Check(binaryKey.size == 16, "key: size is the middle 12 bits");
    Check(binaryKey.type == gears::UserDataType::Binary, "key: binary type");

    // The size field is 12 bits, so it must not bleed into the type.
    const uint32_t wide = MakeSettingId(1, 0xFFF, gears::UserDataType::Binary);
    Check(gears::SettingKey::Decode(wide).size == 0xFFF, "key: full 12-bit size");
    Check(gears::SettingKey::Decode(wide).type == gears::UserDataType::Binary,
        "key: a maximal size does not corrupt the type");
}

void TestReadBufferSizing()
{
    // Header, plus one 40-byte setting record each, plus payload bytes only for
    // the types that have a payload.
    const uint32_t ids[] = { kIntSetting };
    Check(gears::ProfileReadBufferSize(ids, 1) == 8 + 40,
        "sizing: header plus one inline setting");

    const uint32_t both[] = { kIntSetting, kBinarySetting };
    Check(gears::ProfileReadBufferSize(both, 2) == 8 + 40 + 40 + 16,
        "sizing: a binary setting adds its payload");
}

void TestStoreRoundTrip()
{
    gears::UserProfile profile;
    Check(!profile.Has(kIntSetting), "store: an unset setting is absent");

    profile.SetInt32(kIntSetting, -12345);
    Check(profile.Has(kIntSetting), "store: a set setting is present");

    int32_t value = 0;
    Check(profile.GetInt32(kIntSetting, value), "store: reads back");
    Check(value == -12345, "store: round-trips a negative value");

    const std::vector<uint8_t> blob = { 1, 2, 3, 4, 0xFF, 0x80 };
    profile.SetBinary(kBinarySetting, blob);
    std::vector<uint8_t> out;
    Check(profile.GetBinary(kBinarySetting, out), "store: binary reads back");
    Check(out == blob, "store: binary round-trips exactly");
}

void TestPersistence()
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "gears_profile_test.bin";
    std::filesystem::remove(path);

    {
        gears::UserProfile profile;
        profile.SetInt32(kIntSetting, 42);
        profile.SetBinary(kBinarySetting, { 9, 8, 7 });
        Check(profile.Save(path), "persist: save succeeds");
    }

    gears::UserProfile loaded;
    Check(loaded.Load(path), "persist: load succeeds");

    int32_t value = 0;
    Check(loaded.GetInt32(kIntSetting, value) && value == 42,
        "persist: an integer survives a save/load cycle");
    std::vector<uint8_t> blob;
    Check(loaded.GetBinary(kBinarySetting, blob) &&
          blob == std::vector<uint8_t>({ 9, 8, 7 }),
        "persist: binary survives a save/load cycle");

    // Loading a file that is not there is a normal first run, not an error to
    // report: a profile that has never been written simply has no settings.
    std::filesystem::remove(path);
    gears::UserProfile missing;
    Check(!missing.Load(path), "persist: a missing profile loads as empty");
    Check(!missing.Has(kIntSetting), "persist: and has no settings");

    std::filesystem::remove(path);
}

// The part that actually faces the guest. A wrong offset here is invisible
// until the title deserialises nonsense somewhere else entirely.
void TestWriteResultBuffer()
{
    gears::UserProfile profile;
    profile.SetInt32(kIntSetting, 0x1234);

    const uint32_t ids[] = { kIntSetting };
    const uint32_t needed = gears::ProfileReadBufferSize(ids, 1);

    // A guest buffer placed at a non-zero address, so a result that forgot to
    // bias its internal pointer is caught rather than passing by luck.
    std::vector<uint8_t> memory(0x2000, 0xCD);
    constexpr uint32_t kBufferAddress = 0x1000;

    Check(gears::WriteProfileReadResult(memory.data(), kBufferAddress, needed,
                                        profile, ids, 1),
        "result: writing into a large enough buffer succeeds");

    const uint8_t* out = memory.data() + kBufferAddress;
    Check(ReadBE32(out + 0) == 1, "result: header reports one setting");

    // settings_ptr is a GUEST address, and must point at the first record --
    // immediately after the 8-byte header.
    Check(ReadBE32(out + 4) == kBufferAddress + 8,
        "result: settings_ptr is a guest address pointing past the header");

    const uint8_t* setting = out + 8;
    Check(ReadBE32(setting + 16) == kIntSetting, "result: setting id at +16");
    Check(setting[24] == uint8_t(gears::UserDataType::Int32),
        "result: data type at +24");
    Check(ReadBE32(setting + 32) == 0x1234, "result: the value at +32");

    // A buffer that is too small must be refused rather than partially filled.
    std::vector<uint8_t> small(0x2000, 0xCD);
    Check(!gears::WriteProfileReadResult(small.data(), kBufferAddress,
                                         needed - 1, profile, ids, 1),
        "result: a short buffer is refused");
    Check(small[kBufferAddress] == 0xCD,
        "result: and nothing is written into it");
}

// A setting the profile has never been given must come back as UNSET rather
// than as a zero of the right type -- the title distinguishes "no value" from
// "the value is 0", and that is how a fresh profile is supposed to read.
void TestUnsetSetting()
{
    gears::UserProfile profile;
    const uint32_t ids[] = { kIntSetting };
    const uint32_t needed = gears::ProfileReadBufferSize(ids, 1);

    std::vector<uint8_t> memory(0x2000, 0xCD);
    constexpr uint32_t kBufferAddress = 0x1000;
    Check(gears::WriteProfileReadResult(memory.data(), kBufferAddress, needed,
                                        profile, ids, 1),
        "unset: writing an empty profile still succeeds");

    const uint8_t* setting = memory.data() + kBufferAddress + 8;
    Check(setting[24] == uint8_t(gears::UserDataType::Unset),
        "unset: an absent setting reports type UNSET");
}

} // namespace

int main()
{
    TestSettingKeyDecoding();
    TestReadBufferSizing();
    TestStoreRoundTrip();
    TestPersistence();
    TestWriteResultBuffer();
    TestUnsetSetting();

    if (g_failures == 0)
    {
        printf("all user profile tests passed\n");
        return 0;
    }
    printf("%d user profile test(s) FAILED\n", g_failures);
    return 1;
}
