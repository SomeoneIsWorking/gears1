#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "mesh/static_mesh.h"
#include "offscreen_target.h"
#include "scene/transform.h"
#include "vulkan_device.h"

namespace gears::engine::render
{

// The index range of one mesh section.
struct GpuSection
{
    std::uint32_t first_index = 0;
    std::uint32_t index_count = 0;
};

// One static-mesh LOD in GPU-visible memory: interleaved position, normal and
// first texture coordinate, its 16-bit triangle list, and its sections.
class GpuMesh
{
  public:
    static constexpr std::uint32_t kVertexStride = 32;

    // Refuses a LOD with no triangles.
    GpuMesh(const VulkanDevice &device, const mesh::StaticMeshLod &lod);

    [[nodiscard]] VkBuffer Vertices() const noexcept { return vertices_.Handle(); }
    [[nodiscard]] VkBuffer Indices() const noexcept { return indices_.Handle(); }
    [[nodiscard]] const std::vector<GpuSection> &Sections() const noexcept { return sections_; }

  private:
    HostBuffer vertices_;
    HostBuffer indices_;
    std::vector<GpuSection> sections_;
};

// How a draw's colour combines with the target.
enum class Blend : std::uint8_t
{
    kOpaque,
    // Source over destination by the source's alpha.
    kAlpha,
    kAdditive,
    // Destination multiplied by the source colour.
    kModulative,
};

inline constexpr std::size_t kBlendCount = 4;

// How a draw uses its opacity texture.
enum class OpacityUse : std::uint8_t
{
    kNone,
    // Discards fragments whose opacity is below the clip value.
    kAlphaTest,
    // Outputs the opacity as alpha for blending.
    kBlend,
};

// Everything a section's material decides about its draw.
struct DrawMaterial
{
    VkDescriptorSet textures = VK_NULL_HANDLE;
    Blend blend = Blend::kOpaque;
    OpacityUse opacity = OpacityUse::kNone;
    float clip = 0.0F;
    // The opacity texture's channel: 0 red, 1 green, 2 blue, 3 alpha.
    std::uint32_t channel = 0;
    bool lit = true;
};

// The static-mesh pipelines of an offscreen target, one per blend: all
// depth-tested, the blended ones without depth writes; each section samples
// its material's colour and opacity textures, lit by one fixed sun.
class MeshRenderer
{
  public:
    MeshRenderer(const VulkanDevice &device, const OffscreenTarget &target,
                 VkDescriptorSetLayout texture_layout);
    ~MeshRenderer();
    MeshRenderer(const MeshRenderer &) = delete;
    MeshRenderer &operator=(const MeshRenderer &) = delete;

    // Sets the target-sized viewport; call inside the pass before drawing.
    void Begin(VkCommandBuffer commands) const;
    // Binds the pipeline of `blend`.
    void Bind(VkCommandBuffer commands, Blend blend) const;
    // Draws one section of `mesh` with `material`; the pipeline of its blend
    // must be bound.
    void Draw(VkCommandBuffer commands, const GpuMesh &mesh, const GpuSection &section,
              const DrawMaterial &material, const scene::Matrix &world,
              const scene::Matrix &view_projection) const;

  private:
    [[nodiscard]] VkPipeline CreatePipeline(Blend blend, VkRenderPass render_pass) const;

    VkDevice device_;
    VkExtent2D extent_;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    std::array<VkPipeline, kBlendCount> pipelines_{};
};

} // namespace gears::engine::render
