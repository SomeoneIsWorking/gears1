#include "product_options.h"

#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{

using gears::product::ParseProductOptions;
using gears::product::ProductMode;
using gears::product::ProductOptionsResult;

void Require(bool condition, std::string_view message)
{
    if (!condition)
    {
        std::cerr << "product options: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

ProductOptionsResult Parse(std::initializer_list<const char *> arguments)
{
    std::vector<const char *> storage(arguments);
    return ParseProductOptions(std::span<const char *const>(storage.data(), storage.size()));
}

void RequireRefused(std::initializer_list<const char *> arguments, std::string_view expected)
{
    ProductOptionsResult result = Parse(arguments);
    Require(!result, "an invalid command line was accepted; expected: " + std::string(expected));
    Require(result.error.find(expected) != std::string::npos,
            "refusal '" + result.error + "' does not name: " + std::string(expected));
}

} // namespace

int main()
{
    // Absolute on every host: "/tmp/..." is not absolute on Windows.
    std::string storage = (std::filesystem::temp_directory_path() / "gears-run").string();
    std::string frames = (std::filesystem::temp_directory_path() / "gears-run" / "frames").string();
    ProductOptionsResult window = Parse({"--image", "/games/gears.iso", "--title-id", "4d5307d5"});
    Require(static_cast<bool>(window), window.error);
    Require(window.options.mode == ProductMode::Window, "the default mode is not the window");
    Require(window.options.title_id == 0x4D5307D5U, "the title ID was not read as hexadecimal");
    Require(window.options.image == "/games/gears.iso", "the image path was not kept");

    ProductOptionsResult offscreen =
        Parse({"--offscreen", "--image", "/games/gears.iso", "--title-id", "4D5307D5",
               "--storage-root", storage.c_str(), "--seconds", "90", "--capture-dir",
               frames.c_str(), "--capture-every", "15"});
    Require(static_cast<bool>(offscreen), offscreen.error);
    Require(offscreen.options.mode == ProductMode::Offscreen, "--offscreen was not selected");
    Require(offscreen.options.run_seconds == 90U, "--seconds was not read");
    Require(offscreen.options.capture_interval_seconds == 15U, "--capture-every was not read");

    RequireRefused({"--title-id", "4d5307d5"}, "--image is required");
    RequireRefused({"--image", "/games/gears.iso"}, "--title-id is required");
    RequireRefused({"--image", "/games/gears.iso", "--title-id", "zz"}, "malformed");
    RequireRefused({"--image", "/games/gears.iso", "--title-id", "4d5307d5x"}, "malformed");
    RequireRefused({"--image"}, "requires a value");
    RequireRefused({"--image", "/games/gears.iso", "--title-id", "4d5307d5", "--wide"},
                   "unknown option");
    RequireRefused({"--image", "/games/gears.iso", "--title-id", "4d5307d5", "--seconds", "5"},
                   "only with --offscreen");
    RequireRefused(
        {"--offscreen", "--image", "/games/gears.iso", "--title-id", "4d5307d5", "--seconds", "5"},
        "absolute --storage-root");
    RequireRefused({"--offscreen", "--image", "/games/gears.iso", "--title-id", "4d5307d5",
                    "--storage-root", "relative", "--seconds", "5"},
                   "absolute --storage-root");
    RequireRefused({"--offscreen", "--image", "/games/gears.iso", "--title-id", "4d5307d5",
                    "--storage-root", storage.c_str()},
                   "positive --seconds");
    RequireRefused({"--offscreen", "--image", "/games/gears.iso", "--title-id", "4d5307d5",
                    "--storage-root", storage.c_str(), "--seconds", "5", "--capture-every", "1"},
                   "given together");
    std::cout << "product options: 2 accepted, 11 refused\n";
    return EXIT_SUCCESS;
}
