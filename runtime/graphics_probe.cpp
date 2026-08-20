#include "graphics_probe.h"

#include "gpu_draw.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <set>
#include <utility>

namespace gears
{
namespace
{

std::atomic<uint64_t> g_requested{0};
std::atomic<uint64_t> g_published{0};
std::mutex g_mutex;
std::condition_variable g_ready;
std::shared_ptr<const GraphicsProbeFrame> g_latest;

uint64_t HashPixels(const std::vector<uint8_t> &pixels)
{
    // FNV-1a is used as an identity tag, not a cryptographic digest. Its job is
    // to let a driver cheaply tell whether two probes returned the same bytes.
    uint64_t hash = 14695981039346656037ull;
    for (const uint8_t value : pixels)
    {
        hash ^= value;
        hash *= 1099511628211ull;
    }
    return hash;
}

} // namespace

uint64_t RequestGraphicsProbe()
{
    return g_requested.fetch_add(1, std::memory_order_acq_rel) + 1;
}

uint64_t PendingGraphicsProbeRequest()
{
    const uint64_t requested = g_requested.load(std::memory_order_acquire);
    return requested > g_published.load(std::memory_order_acquire) ? requested : 0;
}

void PublishGraphicsProbe(uint64_t request, const FrameDrawInputs &frame, bool rendered,
                          const std::vector<uint8_t> &pixels, uint32_t width, uint32_t height)
{
    if (request == 0)
        return;

    auto snapshot = std::make_shared<GraphicsProbeFrame>();
    snapshot->request = request;
    snapshot->guestFrame = frame.sequence < 0 ? 0 : uint64_t(frame.sequence);
    snapshot->rendered = rendered;
    snapshot->width = width;
    snapshot->height = height;
    snapshot->draws = frame.draws.size();

    std::set<std::pair<uint64_t, uint64_t>> shaderPairs;
    for (const FrameDrawItem &draw : frame.draws)
        shaderPairs.emplace(draw.vsHash, draw.psHash);
    snapshot->shaderPairs = shaderPairs.size();

    const size_t expected = size_t(width) * height * 4;
    if (rendered && width != 0 && height != 0 && pixels.size() == expected)
    {
        snapshot->rgba = pixels;
        snapshot->pixelHash = HashPixels(pixels);
        uint64_t red = 0;
        uint64_t green = 0;
        uint64_t blue = 0;
        for (size_t offset = 0; offset < pixels.size(); offset += 4)
        {
            red += pixels[offset];
            green += pixels[offset + 1];
            blue += pixels[offset + 2];
            if (pixels[offset] != 0 || pixels[offset + 1] != 0 || pixels[offset + 2] != 0)
                ++snapshot->nonBlackPixels;
        }
        const double count = double(width) * height;
        snapshot->meanRed = double(red) / count;
        snapshot->meanGreen = double(green) / count;
        snapshot->meanBlue = double(blue) / count;
    }
    else
    {
        snapshot->rendered = false;
    }

    {
        std::lock_guard<std::mutex> guard(g_mutex);
        if (!g_latest || request >= g_latest->request)
            g_latest = std::move(snapshot);
    }
    g_published.store(std::max(g_published.load(std::memory_order_relaxed), request),
                      std::memory_order_release);
    g_ready.notify_all();
}

std::shared_ptr<const GraphicsProbeFrame> LatestGraphicsProbe()
{
    std::lock_guard<std::mutex> guard(g_mutex);
    return g_latest;
}

std::shared_ptr<const GraphicsProbeFrame> WaitForGraphicsProbe(uint64_t request,
                                                               std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(g_mutex);
    if (!g_ready.wait_for(lock, timeout,
                          [request] { return g_latest && g_latest->request >= request; }))
        return {};
    return g_latest;
}

} // namespace gears
