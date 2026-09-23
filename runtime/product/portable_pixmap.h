#pragma once

#include <string>

#include <x360port/system_session.hpp>

namespace gears::product
{

// The RGB channels of an RGBX guest image as a binary PPM (P6). The format
// needs no encoder, and the maintainer tools convert it for viewing.
[[nodiscard]] std::string EncodePortablePixmap(const x360port::SystemFrameImage &image);

} // namespace gears::product
