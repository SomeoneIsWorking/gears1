#include "offscreen_run.h"

#include "portable_pixmap.h"

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

// Writes a capture as a binary PPM.
[[nodiscard]] bool WritePortablePixmap(const std::filesystem::path &path,
                                       const x360port::SystemFrameImage &image)
{
    std::string pixmap = EncodePortablePixmap(image);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(pixmap.data(), static_cast<std::streamsize>(pixmap.size()));
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

[[nodiscard]] std::string_view MismatchPlace(titles::gears1::AudioMixMismatch::Where where)
{
    switch (where)
    {
    case titles::gears1::AudioMixMismatch::Where::Output:
        return "output block";
    case titles::gears1::AudioMixMismatch::Where::Input:
        return "input block";
    case titles::gears1::AudioMixMismatch::Where::ReturnValue:
        return "return value";
    }
    return "unknown place";
}

bool ReportAudioMixCheck(const titles::gears1::AudioMixDifferential &check)
{
    titles::gears1::AudioMixDifferentialCounts counts = check.Counts();
    lucent::info("product",
                 "audio mix differential: {} calls compared with the original body, {} "
                 "disagreed, {} not compared because a side failed",
                 counts.compared, counts.mismatched, counts.failed);
    if (std::optional<titles::gears1::AudioMixMismatch> first = check.FirstMismatch())
    {
        lucent::error("product",
                      "first disagreement: output {:#010x} input {:#010x}, {} byte {}: native "
                      "{:#010x}, original {:#010x}; r3 native {:#x}, original {:#x}",
                      first->output, first->input, MismatchPlace(first->where), first->byte_offset,
                      first->native_word, first->original_word, first->native_return,
                      first->original_return);
    }
    if (counts.compared == 0)
    {
        lucent::error("product", "the audio mix differential compared no call");
        return false;
    }
    return counts.mismatched == 0 && counts.failed == 0;
}

} // namespace

bool RunOffscreen(x360port::SystemSession &session, const ProductOptions &options,
                  const titles::gears1::AudioMixDifferential *audio_mix_check)
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
    bool healthy = ReportRun(options.run_seconds, session.PresentedFrameCount(),
                             session.ExecutionCounts(), session.FrameIntervals());
    if (audio_mix_check != nullptr && !ReportAudioMixCheck(*audio_mix_check))
    {
        healthy = false;
    }
    return healthy;
}

} // namespace gears::product
