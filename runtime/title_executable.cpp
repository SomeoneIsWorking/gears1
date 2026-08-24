#include "title_executable.h"

#include <algorithm>
#include <limits>

#include <sha256.h>
#include <xex.h>

namespace gears
{

bool LoadTitleExecutable(std::span<const std::uint8_t> xex, LoadedTitleExecutable &loaded,
                         std::string &error)
{
    loaded = {};

    Image image;
    Xex2ExecutionMetadata metadata;
    if (!TryLoadXex(xex.data(), xex.size(), image, metadata, error))
    {
        return false;
    }

    if (image.base > std::numeric_limits<std::uint32_t>::max() ||
        image.size > std::numeric_limits<std::uint32_t>::max() ||
        image.entry_point > std::numeric_limits<std::uint32_t>::max())
    {
        error = "XEX image geometry exceeds the Xenon 32-bit address space";
        return false;
    }

    ::Sha256Digest containerDigest;
    ::Sha256Digest imageDigest;
    if (!ComputeSha256(xex.data(), xex.size(), containerDigest, error) ||
        !ComputeSha256(image.data.get(), image.size, imageDigest, error))
    {
        return false;
    }

    std::ranges::copy(containerDigest, loaded.identity.containerDigest.begin());
    std::ranges::copy(imageDigest, loaded.identity.imageDigest.begin());
    loaded.identity.imageBase = static_cast<std::uint32_t>(image.base);
    loaded.identity.imageSize = static_cast<std::uint32_t>(image.size);
    loaded.identity.entryPoint = static_cast<std::uint32_t>(image.entry_point);
    loaded.image = std::move(image);
    error.clear();
    return true;
}

} // namespace gears
