#pragma once

#include <filesystem>
#include <string_view>

#include <x360port/system_session.hpp>

#include "product_options.h"
#include "titles/gears1/audio_mix_differential.h"

namespace gears::product
{

// The local player signed in before the game starts, so campaign checkpoints
// and settings save. A player keeps the profile their first run created.
inline constexpr std::string_view kLocalPlayerGamertag = "Player";

// The Gears 1 console: the authenticated image, the title's native overrides,
// and its controller arbitration, with sound, host controllers, and storage
// chosen by the product mode. A windowed run captures the window's keyboard
// and mouse into `desktop`, which must outlive the session; offscreen passes
// nullptr. A non-null `audio_mix_check` runs the audio mix through that
// differential instead of the native mix alone; it must outlive the session.
[[nodiscard]] x360port::SystemSessionConfig
Gears1SessionConfig(const ProductOptions &options, const std::filesystem::path &storage_root,
                    x360port::DesktopInputState *desktop,
                    titles::gears1::AudioMixDifferential *audio_mix_check);

} // namespace gears::product
