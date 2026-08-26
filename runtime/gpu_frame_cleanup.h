#pragma once

#include <cstdint>
#include <vector>

#include <vulkan/vulkan.h>

#include "gpu_draw.h"
#include "gpu_draw_renderer.h"
#include "gpu_frame_slots.h"
#include "gpu_scanout.h"

namespace gears::draw
{

// Builds the fence completion for a live frame. Every handle passed here was
// referenced by the submitted command buffer and therefore stays owned until
// this completion runs. Shared scan-out publication happens at the same point,
// so consumers can never observe an image before its producer fence signals.
GpuFrameSlots::Completion
MakeFrameCleanup(Renderer &renderer, GpuScanoutResult scanout, uint32_t width, uint32_t height,
                 long sequence, std::vector<VkBuffer> stagingBuffers,
                 std::vector<VkDeviceMemory> stagingMemory, std::vector<VkBuffer> arenaBuffers,
                 std::vector<VkDeviceMemory> arenaMemory, std::vector<GuestTex> retiredTextures,
                 FrameRenderCompletion completion);

} // namespace gears::draw
