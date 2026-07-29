#include "user_profile.h"

#include <cstdio>
#include <cstring>

#include <lucent/log.h>

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

// Offsets inside X_USER_PROFILE_SETTING. Named rather than inlined because a
// wrong one here is invisible until the title misparses something far away.
constexpr uint32_t kSettingSource = 0;
constexpr uint32_t kSettingUserIndex = 8;
constexpr uint32_t kSettingId = 16;
constexpr uint32_t kSettingDataType = 24;
constexpr uint32_t kSettingDataUnion = 32;

// The on-disk profile. Versioned from the start: a profile that outlives a
// format change has to be recognisable as one this build cannot read, rather
// than being parsed as garbage.
constexpr uint32_t kFileMagic = 0x47505246; // 'GPRF'
constexpr uint32_t kFileVersion = 1;

} // namespace

SettingKey SettingKey::Decode(uint32_t raw)
{
    SettingKey key;
    key.id = uint16_t(raw & 0xFFFF);
    key.size = uint16_t((raw >> 16) & 0xFFF);
    key.type = UserDataType((raw >> 28) & 0xF);
    return key;
}

bool UserProfile::Has(uint32_t settingId) const
{
    return settings_.find(settingId) != settings_.end();
}

void UserProfile::SetInt32(uint32_t settingId, int32_t value)
{
    Value& v = settings_[settingId];
    v.type = UserDataType::Int32;
    v.inlineValue = value;
    v.bytes.clear();
}

bool UserProfile::GetInt32(uint32_t settingId, int32_t& value) const
{
    const auto it = settings_.find(settingId);
    if (it == settings_.end() || it->second.type != UserDataType::Int32)
        return false;
    value = int32_t(it->second.inlineValue);
    return true;
}

void UserProfile::SetBinary(uint32_t settingId, std::vector<uint8_t> value)
{
    Value& v = settings_[settingId];
    v.type = UserDataType::Binary;
    v.inlineValue = 0;
    v.bytes = std::move(value);
}

bool UserProfile::GetBinary(uint32_t settingId, std::vector<uint8_t>& value) const
{
    const auto it = settings_.find(settingId);
    if (it == settings_.end() || it->second.type != UserDataType::Binary)
        return false;
    value = it->second.bytes;
    return true;
}

bool UserProfile::Save(const std::filesystem::path& path) const
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    FILE* f = fopen(path.c_str(), "wb");
    if (f == nullptr)
    {
        lucent::warn("profile", "cannot write the profile to {}", path.string());
        return false;
    }

    const auto write32 = [f](uint32_t v) { fwrite(&v, sizeof(v), 1, f); };
    write32(kFileMagic);
    write32(kFileVersion);
    write32(uint32_t(settings_.size()));

    for (const auto& [id, value] : settings_)
    {
        write32(id);
        write32(uint32_t(value.type));
        const int64_t inlineValue = value.inlineValue;
        fwrite(&inlineValue, sizeof(inlineValue), 1, f);
        write32(uint32_t(value.bytes.size()));
        if (!value.bytes.empty())
            fwrite(value.bytes.data(), 1, value.bytes.size(), f);
    }

    fclose(f);
    lucent::debug("profile", "wrote {} setting(s) to {}", settings_.size(),
                  path.string());
    return true;
}

bool UserProfile::Load(const std::filesystem::path& path)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (f == nullptr)
        return false; // no profile yet: a normal first run

    const auto read32 = [f](uint32_t& v) {
        return fread(&v, sizeof(v), 1, f) == 1;
    };

    uint32_t magic = 0, version = 0, count = 0;
    if (!read32(magic) || !read32(version) || !read32(count) ||
        magic != kFileMagic || version != kFileVersion)
    {
        // A file this build cannot read is reported and left alone rather than
        // partially parsed or silently deleted -- the player's settings are
        // theirs, and a wrong parse is how a save file becomes garbage.
        lucent::warn("profile", "{} is not a profile this build can read"
            " (magic {:#x}, version {})", path.string(), magic, version);
        fclose(f);
        return false;
    }

    settings_.clear();
    for (uint32_t i = 0; i < count; ++i)
    {
        uint32_t id = 0, type = 0, byteCount = 0;
        int64_t inlineValue = 0;
        if (!read32(id) || !read32(type) ||
            fread(&inlineValue, sizeof(inlineValue), 1, f) != 1 ||
            !read32(byteCount))
        {
            lucent::warn("profile", "{} ends early after {} of {} settings",
                         path.string(), i, count);
            break;
        }

        Value value;
        value.type = UserDataType(type);
        value.inlineValue = inlineValue;
        value.bytes.resize(byteCount);
        if (byteCount != 0 && fread(value.bytes.data(), 1, byteCount, f) != byteCount)
        {
            lucent::warn("profile", "{} ends inside setting {:#x}", path.string(), id);
            break;
        }
        settings_[id] = std::move(value);
    }

    fclose(f);
    lucent::debug("profile", "loaded {} setting(s) from {}", settings_.size(),
                  path.string());
    return true;
}

uint32_t ProfileReadBufferSize(const uint32_t* settingIds, uint32_t count)
{
    uint32_t size = kProfileResultHeaderSize;
    for (uint32_t i = 0; i < count; ++i)
    {
        size += kProfileSettingSize;
        const SettingKey key = SettingKey::Decode(settingIds[i]);
        // Only the types that point at bytes outside the record need room for
        // them; everything else lives in the 8-byte union.
        if (key.type == UserDataType::Binary || key.type == UserDataType::WString)
            size += key.size;
    }
    return size;
}

bool WriteProfileReadResult(uint8_t* guestBase, uint32_t bufferAddress,
                            uint32_t bufferSize, const UserProfile& profile,
                            const uint32_t* settingIds, uint32_t count)
{
    const uint32_t needed = ProfileReadBufferSize(settingIds, count);
    if (bufferSize < needed)
        return false;

    uint8_t* buffer = guestBase + bufferAddress;
    std::memset(buffer, 0, needed);

    const uint32_t settingsAddress = bufferAddress + kProfileResultHeaderSize;
    StoreBE32(buffer + 0, count);
    StoreBE32(buffer + 4, settingsAddress);

    // Payload bytes for the pointer-carrying types go after every record, so
    // the records stay a fixed stride apart and the guest can index them.
    uint32_t payloadAddress =
        settingsAddress + count * kProfileSettingSize;

    for (uint32_t i = 0; i < count; ++i)
    {
        uint8_t* record = buffer + kProfileResultHeaderSize + i * kProfileSettingSize;
        const uint32_t id = settingIds[i];
        const SettingKey key = SettingKey::Decode(id);

        StoreBE32(record + kSettingId, id);
        StoreBE32(record + kSettingUserIndex, 0);

        const auto it = profile.settings_.find(id);
        if (it == profile.settings_.end())
        {
            // Never set. The title needs to tell this from a value of zero, so
            // the type is UNSET and the source says there is no value.
            StoreBE32(record + kSettingSource, uint32_t(SettingSource::NoValue));
            record[kSettingDataType] = uint8_t(UserDataType::Unset);
            continue;
        }

        StoreBE32(record + kSettingSource, uint32_t(SettingSource::Title));
        record[kSettingDataType] = uint8_t(it->second.type);

        switch (it->second.type)
        {
        case UserDataType::Int32:
        case UserDataType::Context:
        case UserDataType::Float:
            StoreBE32(record + kSettingDataUnion, uint32_t(it->second.inlineValue));
            break;

        case UserDataType::Binary:
        case UserDataType::WString:
        {
            // The record carries a length and a GUEST pointer to the bytes; the
            // bytes themselves are copied into the payload area.
            const uint32_t bytes =
                uint32_t(it->second.bytes.size()) < key.size
                    ? uint32_t(it->second.bytes.size()) : key.size;
            StoreBE32(record + kSettingDataUnion, bytes);
            StoreBE32(record + kSettingDataUnion + 4, payloadAddress);
            if (bytes != 0)
                std::memcpy(guestBase + payloadAddress, it->second.bytes.data(), bytes);
            payloadAddress += key.size;
            break;
        }

        default:
            // A stored type this function does not know how to project is
            // reported as unset rather than written as a wrong shape.
            StoreBE32(record + kSettingSource, uint32_t(SettingSource::NoValue));
            record[kSettingDataType] = uint8_t(UserDataType::Unset);
            break;
        }
    }

    return true;
}

UserProfile& Profile()
{
    static UserProfile profile;
    return profile;
}

uint32_t CopyGamertag(char* dest, uint32_t destBytes)
{
    // Nowhere even for a terminator, so XAM writes nothing at all rather than
    // one byte past the caller's buffer.
    if (destBytes == 0)
        return 0;

    const uint32_t nameBytes = uint32_t(std::strlen(kGamertag)) + 1;
    const uint32_t written = destBytes < nameBytes ? destBytes : nameBytes;

    std::memcpy(dest, kGamertag, written - 1);
    dest[written - 1] = '\0';
    return written;
}

} // namespace gears
