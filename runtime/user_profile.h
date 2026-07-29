// The player's profile settings, and the buffer protocol the title reads them
// through.
//
// A console profile is a small key/value store that survives across runs. The
// title reads it with XamUserReadProfileSettings and writes it with
// XamUserWriteProfileSettings, and until this existed the runtime answered both
// with "there is nothing and nowhere to put it" -- honest, but it left the
// title with no profile at all, which is a subsystem missing rather than a
// subsystem empty.
//
// On a PC the natural home for it is a file next to the saves, so settings
// persist the way a player expects without pretending a memory unit exists.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace gears
{

// The console's setting types. The value is written into the guest structure
// verbatim, so these numbers are the console's, not ours.
enum class UserDataType : uint8_t
{
    Context = 0,
    Int32 = 1,
    Int64 = 2,
    Double = 3,
    WString = 4,
    Float = 5,
    Binary = 6,
    DateTime = 7,
    Unset = 0xFF,
};

// Where a returned value came from. A title can tell "never set" from "set to
// zero" only through this field, so a fresh profile has to report NoValue
// rather than a zero of the right type.
enum class SettingSource : uint32_t
{
    NoValue = 0,
    Default = 1,
    Title = 2,
    PermissionDenied = 3,
};

// A setting id is not opaque: it encodes its own payload type and size, which
// is what lets a read buffer be sized without knowing every setting a title
// might ask for.
struct SettingKey
{
    uint16_t id = 0;
    uint16_t size = 0;      // 12 bits: payload bytes, for the types that have one
    UserDataType type = UserDataType::Unset;

    static SettingKey Decode(uint32_t raw);
};

// The console's structure sizes, which the guest indexes by.
constexpr uint32_t kProfileResultHeaderSize = 8;   // count + settings pointer
constexpr uint32_t kProfileSettingSize = 40;

class UserProfile
{
public:
    bool Has(uint32_t settingId) const;

    void SetInt32(uint32_t settingId, int32_t value);
    bool GetInt32(uint32_t settingId, int32_t& value) const;

    void SetBinary(uint32_t settingId, std::vector<uint8_t> value);
    bool GetBinary(uint32_t settingId, std::vector<uint8_t>& value) const;

    // Persistence. Load returns false when there is no profile yet, which is a
    // normal first run rather than a failure.
    bool Save(const std::filesystem::path& path) const;
    bool Load(const std::filesystem::path& path);

private:
    struct Value
    {
        UserDataType type = UserDataType::Unset;
        int64_t inlineValue = 0;
        std::vector<uint8_t> bytes;
    };

    std::map<uint32_t, Value> settings_;

    friend bool WriteProfileReadResult(uint8_t*, uint32_t, uint32_t,
                                       const UserProfile&, const uint32_t*,
                                       uint32_t);
};

// How many bytes a read of these settings needs: the header, one fixed-size
// record each, and payload bytes for the types that carry one.
uint32_t ProfileReadBufferSize(const uint32_t* settingIds, uint32_t count);

// Fills a guest buffer with the result. `bufferAddress` is the GUEST address of
// the buffer, needed because the structure contains a pointer to itself.
// Returns false without writing anything if the buffer is too small -- a
// partially filled result is worse than none, since the title cannot tell.
bool WriteProfileReadResult(uint8_t* guestBase, uint32_t bufferAddress,
                            uint32_t bufferSize, const UserProfile& profile,
                            const uint32_t* settingIds, uint32_t count);

// The process-wide profile, loaded from disk on first use.
UserProfile& Profile();

} // namespace gears
