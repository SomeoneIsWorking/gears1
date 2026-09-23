#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

namespace gears::product
{

enum class ProductMode : std::uint8_t
{
    // The player's product: a game window, sound, and host controllers.
    Window,
    // A maintainer run: no window, silent audio, only scripted input, for a
    // fixed duration, reporting presents and native-override calls.
    Offscreen,
};

// The product's complete command line. The bootstrap supplies the
// authenticated image and its title ID; everything else is optional.
struct ProductOptions
{
    std::filesystem::path image;
    std::uint32_t title_id = 0;
    ProductMode mode = ProductMode::Window;
    // Offscreen only: an explicit writable root in place of the player's
    // user-data directory, so a measurement never touches real saves.
    std::filesystem::path storage_root;
    std::uint32_t run_seconds = 0;
    std::filesystem::path capture_directory;
    std::uint32_t capture_interval_seconds = 0;
    // Offscreen only: write a Linux perf map of the translated guest code.
    bool perf_map = false;
    // Offscreen only: serve the loopback control channel on this port.
    std::uint16_t control_port = 0;
};

struct ProductOptionsResult
{
    ProductOptions options;
    // Empty on success; otherwise the reason the command line was refused.
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
};

// Parses argv after the program name. Every option is refused unless its
// value is well formed and consistent with the selected mode.
[[nodiscard]] ProductOptionsResult ParseProductOptions(std::span<const char *const> arguments);

} // namespace gears::product
