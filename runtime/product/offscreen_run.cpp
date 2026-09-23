#include "offscreen_run.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

#include <x360port/frame_intervals.hpp>

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

constexpr std::uint32_t kFrameTimeWindowSeconds = 10;
constexpr double kMicrosecondsPerMillisecond = 1000.0;

std::string FormatQuantile(const x360port::FrameIntervalHistogram &intervals, double fraction)
{
    std::optional<x360port::FrameIntervalQuantile> quantile = intervals.Quantile(fraction);
    if (!quantile)
    {
        return "none";
    }
    return std::format("{}{:.1f} ms", quantile->open_ended ? ">=" : "",
                       quantile->microseconds / kMicrosecondsPerMillisecond);
}

// Frame times between guest swaps, as the upper edge of the 0.1 ms bucket each
// percentile falls in.
void ReportFrameTimes(std::string_view span, const x360port::FrameIntervalHistogram &intervals)
{
    lucent::info("product", "{}: {} frame times, p50 {}, p95 {}, p99 {}, max {}", span,
                 intervals.Count(), FormatQuantile(intervals, 0.5), FormatQuantile(intervals, 0.95),
                 FormatQuantile(intervals, 0.99), FormatQuantile(intervals, 1.0));
}

bool ReportRun(std::uint32_t seconds, std::uint64_t presents,
               const x360port::SystemExecutionCounts &counts,
               const x360port::FrameIntervalHistogram &intervals)
{
    ReportFrameTimes(std::format("whole run of {} s", seconds), intervals);
    lucent::info("product",
                 "offscreen run ended after {} s: {} presents; {} guest functions translated to "
                 "{} host bytes, {} translation failures; {} native-override calls",
                 seconds, presents, counts.translated_functions, counts.host_code_bytes,
                 counts.translation_failures, counts.native_override_calls);
    bool healthy = true;
    if (presents == 0)
    {
        lucent::error("product", "the title never presented a frame");
        healthy = false;
    }
    if (counts.translated_functions == 0)
    {
        lucent::error("product", "the dynarec translated no guest function");
        healthy = false;
    }
    if (counts.translation_failures != 0)
    {
        lucent::error("product", "{} guest functions failed to translate; their calls failed",
                      counts.translation_failures);
        healthy = false;
    }
    return healthy;
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
    x360port::FrameIntervalHistogram window_start = session.FrameIntervals();
    for (std::uint32_t second = 1; second <= options.run_seconds; ++second)
    {
        std::this_thread::sleep_until(start + std::chrono::seconds(second));
        std::uint64_t presents = session.PresentedFrameCount();
        x360port::SystemExecutionCounts counts = session.ExecutionCounts();
        lucent::info("product",
                     "second {}: {} presents/s, {} presents, {} translated functions, {} "
                     "native-override calls",
                     second, presents - previous_presents, presents, counts.translated_functions,
                     counts.native_override_calls);
        previous_presents = presents;
        if (second % kFrameTimeWindowSeconds == 0)
        {
            x360port::FrameIntervalHistogram now = session.FrameIntervals();
            ReportFrameTimes(
                std::format("seconds {}-{}", second - kFrameTimeWindowSeconds + 1, second),
                now.Since(window_start));
            window_start = now;
        }
        if (options.capture_interval_seconds != 0 && second % options.capture_interval_seconds == 0)
        {
            CaptureSecond(session, options, second);
        }
    }
    return ReportRun(options.run_seconds, session.PresentedFrameCount(), session.ExecutionCounts(),
                     session.FrameIntervals());
}

} // namespace gears::product
