#include "gpu_frame_cleanup.h"

#include <memory>
#include <utility>

namespace gears::draw
{
namespace
{

struct FrameCleanup
{
    Renderer *renderer = nullptr;
    GpuScanoutResult scanout;
    uint32_t width = 0;
    uint32_t height = 0;
    long sequence = -1;
    std::vector<VkBuffer> stagingBuffers;
    std::vector<VkDeviceMemory> stagingMemory;
    std::vector<VkBuffer> arenaBuffers;
    std::vector<VkDeviceMemory> arenaMemory;
    std::vector<GuestTex> retiredTextures;
    FrameRenderCompletion completion;

    void Finish(bool success)
    {
        if (success && scanout.image != VK_NULL_HANDLE &&
            !renderer->persistent->scanout.Publish(scanout, width, height, sequence))
            success = false;

        const VkDevice device = renderer->device;
        for (size_t i = 0; i < stagingBuffers.size(); ++i)
        {
            vkDestroyBuffer(device, stagingBuffers[i], nullptr);
            vkFreeMemory(device, stagingMemory[i], nullptr);
        }
        for (size_t i = 0; i < arenaBuffers.size(); ++i)
        {
            vkDestroyBuffer(device, arenaBuffers[i], nullptr);
            vkFreeMemory(device, arenaMemory[i], nullptr);
        }
        for (GuestTex &texture : retiredTextures)
        {
            vkDestroyImageView(device, texture.view, nullptr);
            vkDestroyImage(device, texture.image, nullptr);
            vkFreeMemory(device, texture.mem, nullptr);
        }
        completion(success);
    }
};

} // namespace

GpuFrameSlots::Completion
MakeFrameCleanup(Renderer &renderer, GpuScanoutResult scanout, uint32_t width, uint32_t height,
                 long sequence, std::vector<VkBuffer> stagingBuffers,
                 std::vector<VkDeviceMemory> stagingMemory, std::vector<VkBuffer> arenaBuffers,
                 std::vector<VkDeviceMemory> arenaMemory, std::vector<GuestTex> retiredTextures,
                 FrameRenderCompletion completion)
{
    auto cleanup = std::make_shared<FrameCleanup>(
        FrameCleanup{&renderer, std::move(scanout), width, height, sequence,
                     std::move(stagingBuffers), std::move(stagingMemory), std::move(arenaBuffers),
                     std::move(arenaMemory), std::move(retiredTextures), std::move(completion)});
    return [cleanup = std::move(cleanup)](bool success) { cleanup->Finish(success); };
}

} // namespace gears::draw
