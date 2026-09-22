#pragma once

#include <filesystem>

#include <x360port/system_session.hpp>

#include "product_options.h"

namespace gears::product
{

// The Gears 1 console: the authenticated image, the title's native overrides,
// and its controller arbitration, with sound, host controllers, and storage
// chosen by the product mode. A windowed run captures the window's keyboard
// and mouse into `desktop`, which must outlive the session; offscreen passes
// nullptr.
[[nodiscard]] x360port::SystemSessionConfig
Gears1SessionConfig(const ProductOptions &options, const std::filesystem::path &storage_root,
                    x360port::DesktopInputState *desktop);

} // namespace gears::product
