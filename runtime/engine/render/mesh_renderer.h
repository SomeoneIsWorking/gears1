#pragma once

#include <cstdint>

#include "mesh/static_mesh.h"
#include "offscreen_target.h"
#include "scene/transform.h"
#include "vulkan_device.h"

namespace gears::engine::render
{

// One static-mesh LOD in GPU-visible memory: interleaved position, normal and
// first texture coordinate, and its 16-bit triangle list.
class GpuMesh
{
  public:
    static constexpr std::uint32_t kVertexStride = 32;

    // Refuses a LOD with no triangles.
    GpuMesh(const VulkanDevice &device, const mesh::StaticMeshLod &lod);

    [[nodiscard]] VkBuffer Vertices() const noexcept { return vertices_.Handle(); }
    [[nodiscard]] VkBuffer Indices() const noexcept { return indices_.Handle(); }
    [[nodiscard]] std::uint32_t IndexCount() const noexcept { return index_count_; }

  private:
    HostBuffer vertices_;
    HostBuffer indices_;
    std::uint32_t index_count_;
};

// The opaque static-mesh pipeline of an offscreen target: depth-tested,
// lit by one fixed sun, one draw per placed mesh.
class MeshRenderer
{
  public:
    MeshRenderer(const VulkanDevice &device, const OffscreenTarget &target);
    ~MeshRenderer();
    MeshRenderer(const MeshRenderer &) = delete;
    MeshRenderer &operator=(const MeshRenderer &) = delete;

    // Binds the pipeline and the target-sized viewport; call inside the pass.
    void Bind(VkCommandBuffer commands) const;
    void Draw(VkCommandBuffer commands, const GpuMesh &mesh, const scene::Matrix &world,
              const scene::Matrix &view_projection) const;

  private:
    VkDevice device_;
    VkExtent2D extent_;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
};

} // namespace gears::engine::render
