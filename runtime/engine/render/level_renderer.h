#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "gpu_texture.h"
#include "material/material_surfaces.h"
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
// textures, each drawn section's base-colour source, and its blend.
struct LevelRenderCensus
{
    std::size_t draws = 0;
    std::size_t meshes = 0;
    std::size_t placements_without_lod = 0;
    std::size_t models = 0;
    std::size_t models_without_triangles = 0;
    std::size_t textures = 0;
    std::map<std::string, std::size_t> section_colors;
    std::map<std::string, std::size_t> section_blends;
};

// Draws the static meshes and BSP surfaces a level places, each section
// sampling its material's colour and opacity textures, into a headless
// frame: opaque and masked sections first, then blended ones in the order
// they were prepared.
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
        DrawMaterial material;
        scene::Matrix world;
    };

    void PrepareMeshes(const package::Package &level, const scene::LevelScene &scene);
    void PrepareModels(const scene::LevelScene &scene);
    const PreparedMesh &MeshOf(const object::ExportLocation &mesh);
    // How a section drawing a material reference draws, counting its colour
    // source and blend.
    DrawMaterial MaterialOf(const package::Package &package, package::PackageIndex material);
    DrawMaterial FromSurface(const material::MaterialSurface &surface);
    // The GPU texture of a texture input, or the untextured texel when it
    // has none that can be sampled.
    const GpuTexture &TextureOf(const material::ColorTexture &input);
    VkDescriptorSet SetOf(const GpuTexture &color, const GpuTexture &opacity);
    void AddDraw(const GpuMesh &mesh, const GpuSection &section, const DrawMaterial &material,
                 const scene::Matrix &world);

    const VulkanDevice &device_;
    package::ContentFiles &files_;
    object::ClassHierarchy &classes_;
    object::ObjectResolver &resolver_;
    material::MaterialSurfaces materials_;
    OffscreenTarget target_;
    TextureBindings bindings_;
    MeshRenderer renderer_;
    GpuTexture untextured_;
    std::map<Key, PreparedMesh> meshes_;
    std::vector<std::unique_ptr<GpuMesh>> models_;
    // Uploaded textures by export; null for one that cannot be sampled.
    std::map<Key, std::unique_ptr<GpuTexture>> textures_;
    std::map<std::pair<const GpuTexture *, const GpuTexture *>, VkDescriptorSet> sets_;
    std::vector<Draw> opaque_draws_;
    std::vector<Draw> blended_draws_;
    LevelRenderCensus census_;
};

} // namespace gears::engine::render
