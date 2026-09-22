#include "offscreen_run.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>

#include <lucent/log.h>

namespace gears::product
{
namespace
{

constexpr std::size_t kCapturedChannels = 3;
constexpr std::size_t kGuestPixelBytes = 4;

// Writes the RGB channels of an RGBX guest image as a binary PPM. The format
// needs no encoder, and the maintainer tool converts it for viewing.
[[nodiscard]] bool WritePortablePixmap(const std::filesystem::path &path,
                                       const x360port::SystemFrameImage &image)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << "P6\n" << image.width << ' ' << image.height << "\n255\n";
    std::string row(static_cast<std::size_t>(image.width) * kCapturedChannels, '\0');
    for (std::uint32_t y = 0; y < image.height; ++y)
    {
        const std::byte *source = image.pixels.data() + static_cast<std::size_t>(y) * image.stride;
        for (std::uint32_t x = 0; x < image.width; ++x)
        {
            for (std::size_t channel = 0; channel < kCapturedChannels; ++channel)
            {
                row[static_cast<std::size_t>(x) * kCapturedChannels + channel] = static_cast<char>(
                    source[static_cast<std::size_t>(x) * kGuestPixelBytes + channel]);
            }
        }
        file.write(row.data(), static_cast<std::streamsize>(row.size()));
    }
    return static_cast<bool>(file);
}

void CaptureSecond(const x360port::SystemSession &session, const ProductOptions &options,
                   std::uint32_t second)
{
    x360port::SystemFrameImage image;
    if (x360port::RuntimeFailure failure = session.CaptureGuestOutput(image))
    {
        lucent::warn("product", "second {}: no guest output to capture: {}", second,
                     failure.detail);
        return;
    }
    std::filesystem::path path =
        options.capture_directory / ("second-" + std::to_string(second) + ".ppm");
    if (!WritePortablePixmap(path, image))
    {
        lucent::error("product", "second {}: could not write {}", second, path.string());
        return;
    }
    lucent::info("product", "second {}: captured {}x{} to {}", second, image.width, image.height,
                 path.string());
}

} // namespace

bool RunOffscreen(x360port::SystemSession &session, const ProductOptions &options)
{
    if (!options.capture_directory.empty())
    {
        std::error_code error;
        std::filesystem::create_directories(options.capture_directory, error);
        if (error)
        {
            lucent::error("product", "cannot create capture directory {}: {}",
                          options.capture_directory.string(), error.message());
            return false;
        }
    }
    auto start = std::chrono::steady_clock::now();
    std::uint64_t previous_presents = 0;
    for (std::uint32_t second = 1; second <= options.run_seconds; ++second)
    {
        std::this_thread::sleep_until(start + std::chrono::seconds(second));
        std::uint64_t presents = session.PresentedFrameCount();
        lucent::info("product", "second {}: {} presents/s, {} presents, {} native-override calls",
                     second, presents - previous_presents, presents, session.NativeOverrideCalls());
        previous_presents = presents;
        if (options.capture_interval_seconds != 0 && second % options.capture_interval_seconds == 0)
        {
            CaptureSecond(session, options, second);
        }
    }
    std::uint64_t presents = session.PresentedFrameCount();
    std::uint64_t override_calls = session.NativeOverrideCalls();
    lucent::info("product", "offscreen run ended after {} s: {} presents, {} native-override calls",
                 options.run_seconds, presents, override_calls);
    if (presents == 0)
    {
        lucent::error("product", "the title never presented a frame");
    }
    return presents != 0;
}

} // namespace gears::product
