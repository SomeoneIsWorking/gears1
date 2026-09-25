#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "draw_materials.h"
#include "frame_constants.h"
#include "mesh_renderer.h"
#include "object/class_hierarchy.h"
#include "object/object_resolver.h"
#include "offscreen_target.h"
#include "package/content_files.h"
#include "scene/camera.h"
#include "scene/level_scene.h"
#include "scene/static_meshes.h"
#include "texture_bindings.h"
#include "vulkan_device.h"

namespace gears::engine::render
{

// What the prepared frame draws: section draws, distinct GPU meshes, and
// BSP components.
struct LevelRenderCensus
{
    std::size_t draws = 0;
    std::size_t meshes = 0;
    std::size_t placements_without_lod = 0;
    std::size_t models = 0;
    std::size_t models_without_triangles = 0;
};

// Draws the static meshes and BSP surfaces a level places, each section as
// its material and light map decide, into a headless frame: opaque and
// masked sections first, then blended ones in the order they were prepared.
class LevelRenderer
{
  public:
    // Texture sets one frame can bind.
    static constexpr std::uint32_t kMaxTextures = 8192;

    LevelRenderer(const VulkanDevice &device, VkExtent2D extent, package::ContentFiles &files,
                  object::ClassHierarchy &classes, object::ObjectResolver &resolver,
                  scene::StaticMeshes &static_meshes);

    // Uploads the meshes and textures `scene` (built from `level`) places
    // and records its draws.
    void Prepare(const package::Package &level, const scene::LevelScene &scene);
    // Renders the prepared draws and returns the frame as RGBA rows.
    [[nodiscard]] std::vector<std::uint8_t> Render(const scene::Camera &camera);
    // Records the prepared draws seen from `camera` into `commands`, leaving
    // the frame in ColorImage(). The previous frame recorded here must have
    // finished executing.
    void Record(VkCommandBuffer commands, const scene::Camera &camera);
    [[nodiscard]] VkImage ColorImage() const noexcept { return target_.ColorImage(); }

    [[nodiscard]] VkExtent2D Extent() const noexcept { return target_.Extent(); }
    [[nodiscard]] const LevelRenderCensus &Census() const noexcept { return census_; }
    [[nodiscard]] const MaterialCensus &Materials() const noexcept { return materials_.Census(); }

  private:
    // Records the pass itself; the caller ends it.
    void RecordDraws(VkCommandBuffer commands);

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
    void AddDraw(const GpuMesh &mesh, const GpuSection &section, const DrawMaterial &material,
                 const scene::Matrix &world);

    const VulkanDevice &device_;
    scene::StaticMeshes &static_meshes_;
    OffscreenTarget target_;
    TextureBindings bindings_;
    FrameConstants frame_;
    MeshRenderer renderer_;
    DrawMaterials materials_;
    std::map<Key, PreparedMesh> meshes_;
    std::vector<std::unique_ptr<GpuMesh>> models_;
    std::vector<Draw> opaque_draws_;
    std::vector<Draw> blended_draws_;
    LevelRenderCensus census_;
};

} // namespace gears::engine::render
