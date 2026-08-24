#pragma once

#include <cstdint>
#include <span>
#include <string>

#include <image.h>

#include "title_profile.h"

namespace gears
{

struct LoadedTitleExecutable
{
    Image image;
    XexIdentity identity;
};

// Loads and fingerprints the exact bytes through the checked XEX loader. The
// returned Image is the same normalized image whose digest is in identity, so
// callers cannot accidentally validate one parse and execute another.
[[nodiscard]] bool LoadTitleExecutable(std::span<const std::uint8_t> xex,
                                       LoadedTitleExecutable &loaded, std::string &error);

} // namespace gears
