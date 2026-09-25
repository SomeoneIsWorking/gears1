#pragma once

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

// The opaque static-mesh pipeline of an offscreen target: depth-tested,
// sampling one base-colour texture per section, lit by one fixed sun.
class MeshRenderer
{
  public:
    MeshRenderer(const VulkanDevice &device, const OffscreenTarget &target,
                 VkDescriptorSetLayout texture_layout);
    ~MeshRenderer();
    MeshRenderer(const MeshRenderer &) = delete;
    MeshRenderer &operator=(const MeshRenderer &) = delete;

    // Binds the pipeline and the target-sized viewport; call inside the pass.
    void Bind(VkCommandBuffer commands) const;
    // Draws one section of `mesh` sampling `texture`.
    void Draw(VkCommandBuffer commands, const GpuMesh &mesh, const GpuSection &section,
              VkDescriptorSet texture, const scene::Matrix &world,
              const scene::Matrix &view_projection) const;

  private:
    VkDevice device_;
    VkExtent2D extent_;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
};

} // namespace gears::engine::render
