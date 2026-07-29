#include "xam_content.h"

#include <algorithm>
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

void StoreBE16(uint8_t* p, uint16_t v)
{
    p[0] = uint8_t(v >> 8);
    p[1] = uint8_t(v);
}

} // namespace

ContentDecision DecideContentCreate(uint32_t flags, bool existed)
{
    switch (flags & 0xF)
    {
    case kContentCreateNew:
        return existed ? ContentDecision::AlreadyExists : ContentDecision::Create;
    case kContentCreateAlways:
        // "Always" means always: an existing one is replaced, not rejected.
        return ContentDecision::Create;
    case kContentOpenExisting:
        return existed ? ContentDecision::Open : ContentDecision::NotFound;
    case kContentOpenAlways:
        return existed ? ContentDecision::Open : ContentDecision::Create;
    case kContentTruncateExisting:
        return existed ? ContentDecision::Create : ContentDecision::NotFound;
    default:
        return ContentDecision::Invalid;
    }
}

bool ContentExists(const std::filesystem::path& directory)
{
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec))
        return false;

    for (const auto& entry : std::filesystem::directory_iterator(directory, ec))
    {
        if (entry.is_regular_file(ec) && entry.file_size(ec) > 0)
            return true;
    }
    return false;
}

std::vector<ContentEntry> EnumerateContent(const std::filesystem::path& saveRoot,
                                           uint32_t contentType)
{
    std::vector<ContentEntry> found;

    std::error_code ec;
    if (!std::filesystem::is_directory(saveRoot, ec))
        return found;

    for (const auto& entry : std::filesystem::directory_iterator(saveRoot, ec))
    {
        if (!entry.is_directory(ec) || !ContentExists(entry.path()))
            continue;

        ContentEntry content;
        content.contentType = contentType;
        content.fileName = entry.path().filename().string();
        // Nothing records a display name yet, so the directory name stands in.
        // It is what the player named it through the title in the first place.
        content.displayName.assign(content.fileName.begin(),
                                   content.fileName.end());
        found.push_back(std::move(content));
    }

    std::sort(found.begin(), found.end(),
              [](const ContentEntry& a, const ContentEntry& b) {
                  return a.fileName < b.fileName;
              });
    return found;
}

void WriteContentData(uint8_t* guestBase, uint32_t address,
                      const ContentEntry& entry)
{
    uint8_t* data = guestBase + address;
    std::memset(data, 0, kContentDataSize);

    StoreBE32(data + 0x000, entry.deviceId);
    StoreBE32(data + 0x004, entry.contentType);

    // One character short of the field, so there is always a terminator: a
    // title that reads this as a C string must find an end inside the record.
    const size_t nameChars =
        std::min<size_t>(entry.displayName.size(), kContentDisplayNameChars - 1);
    for (size_t i = 0; i < nameChars; ++i)
        StoreBE16(data + 0x008 + i * 2, uint16_t(entry.displayName[i]));

    const size_t fileChars =
        std::min<size_t>(entry.fileName.size(), kContentFileNameBytes - 1);
    std::memcpy(data + 0x108, entry.fileName.data(), fileChars);
}

} // namespace gears
