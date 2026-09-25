#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "gpu_texture.h"
#include "material/material_textures.h"
#include "mesh_renderer.h"
#include "object/class_hierarchy.h"
#include "object/object_resolver.h"
#include "offscreen_target.h"
#include "package/content_files.h"
#include "scene/camera.h"
#include "scene/level_scene.h"
#include "texture_bindings.h"
#include "vulkan_device.h"

namespace gears::engine::render
{

// What the prepared frame draws: placements, distinct GPU meshes and
// textures, and each drawn section's base-colour source.
struct LevelRenderCensus
{
    std::size_t draws = 0;
    std::size_t meshes = 0;
    std::size_t placements_without_lod = 0;
    std::size_t textures = 0;
    std::map<std::string, std::size_t> section_colors;
};

// Draws the static meshes a level places, each section sampling the base
// colour texture of its material, into a headless frame.
class LevelRenderer
{
  public:
    // Texture sets one frame can bind.
    static constexpr std::uint32_t kMaxTextures = 8192;

    LevelRenderer(const VulkanDevice &device, VkExtent2D extent, package::ContentFiles &files,
                  object::ClassHierarchy &classes, object::ObjectResolver &resolver);

    // Uploads the meshes and textures `scene` (built from `level`) places
    // and records its draws.
    void Prepare(const package::Package &level, const scene::LevelScene &scene);
    // Renders the prepared draws and returns the frame as RGBA rows.
    [[nodiscard]] std::vector<std::uint8_t> Render(const scene::Camera &camera);

    [[nodiscard]] VkExtent2D Extent() const noexcept { return target_.Extent(); }
    [[nodiscard]] const LevelRenderCensus &Census() const noexcept { return census_; }

  private:
    using Key = std::pair<const package::Package *, std::size_t>;

    struct PreparedMesh
    {
        std::unique_ptr<GpuMesh> gpu;
        // Each section's material, a reference of the mesh's package.
        std::vector<package::PackageIndex> materials;
    };

    struct Draw
    {
        const GpuMesh *mesh = nullptr;
        GpuSection section;
        VkDescriptorSet texture = VK_NULL_HANDLE;
        scene::Matrix world;
    };

    const PreparedMesh &MeshOf(const object::ExportLocation &mesh);
    // The texture set of a material reference, counting its colour source.
    VkDescriptorSet TextureOf(const package::Package &package, package::PackageIndex material);
    VkDescriptorSet Upload(const object::ExportLocation &texture);

    const VulkanDevice &device_;
    package::ContentFiles &files_;
    object::ClassHierarchy &classes_;
    object::ObjectResolver &resolver_;
    material::MaterialTextures materials_;
    OffscreenTarget target_;
    TextureBindings bindings_;
    MeshRenderer renderer_;
    GpuTexture untextured_;
    VkDescriptorSet untextured_set_;
    std::map<Key, PreparedMesh> meshes_;
    std::vector<std::unique_ptr<GpuTexture>> textures_;
    std::map<Key, VkDescriptorSet> texture_sets_;
    std::vector<Draw> draws_;
    LevelRenderCensus census_;
};

} // namespace gears::engine::render
