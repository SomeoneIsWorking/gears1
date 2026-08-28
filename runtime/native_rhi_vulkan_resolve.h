#pragma once

#include "rhi_semantic_stream.h"

#include <cstdint>

#include <vulkan/vulkan.h>

namespace gears::native_rhi
{

// A host-owned image identity. The native backend supplies these handles; the
// guest object numbers in RhiSemanticResolve are audit evidence, not Vulkan
// resource handles or allocation instructions.
struct VulkanResolveImage
{
    VkImage image = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

enum class VulkanResolveStatus : std::uint8_t
{
    Accepted,
    MissingImage,
    SameImage,
    DepthStencilSource,
    UnsupportedOperationFlags,
    InvalidSourceSelection,
    InvalidSourceRectangle,
    InvalidDestinationPoint,
    InvalidDestinationPitch,
    InvalidDestinationHeight,
    FormatMismatch,
    UnsupportedFormat,
    BytesPerPixelMismatch,
    MultisampleUnsupported,
};

// Records the first native resolve class: a one-to-one colour image copy.
// Unsupported Xenos conversion, swizzle, scaling, and multisample semantics
// refuse here; they are not approximated by a Vulkan copy. The image layouts in
// both image records are updated to the layouts left by the recorded command.
[[nodiscard]] VulkanResolveStatus RecordColorResolve(VkCommandBuffer commands,
                                                     const RhiSemanticResolve &resolve,
                                                     VulkanResolveImage &source,
                                                     VulkanResolveImage &destination);

[[nodiscard]] const char *VulkanResolveStatusText(VulkanResolveStatus status) noexcept;

} // namespace gears::native_rhi
