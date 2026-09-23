#pragma once

#include <filesystem>
#include <string_view>

#include <x360port/system_session.hpp>

#include "product_options.h"

namespace gears::product
{

// The local player signed in before the game starts, so campaign checkpoints
// and settings save. A player keeps the profile their first run created.
inline constexpr std::string_view kLocalPlayerGamertag = "Player";

// The Gears 1 console: the authenticated image, the title's native overrides,
// and its controller arbitration, with sound, host controllers, and storage
// chosen by the product mode. A windowed run captures the window's keyboard
// and mouse into `desktop`, which must outlive the session; offscreen passes
// nullptr.
[[nodiscard]] x360port::SystemSessionConfig
Gears1SessionConfig(const ProductOptions &options, const std::filesystem::path &storage_root,
                    x360port::DesktopInputState *desktop);

} // namespace gears::product
