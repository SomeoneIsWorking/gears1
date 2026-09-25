#pragma once

#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>

#include "bsp/model_component.h"
#include "gpu_texture.h"
#include "material/material_surfaces.h"
#include "mesh_renderer.h"
#include "object/class_hierarchy.h"
#include "object/object_resolver.h"
#include "package/content_files.h"
#include "texture_bindings.h"
#include "vulkan_device.h"

namespace gears::engine::render
{

// What the prepared sections' materials drew from: each section's colour
// source, blend, and lighting, and the distinct textures uploaded.
struct MaterialCensus
{
    std::size_t textures = 0;
    std::map<std::string, std::size_t> colors;
    std::map<std::string, std::size_t> blends;
    std::map<std::string, std::size_t> lighting;
};

// Turns a section's material and light map into how it draws: uploads the
// textures they sample (each once, colour as sRGB, opacity and light maps
// as linear values), binds them together, and sets blend, alpha test, and
// lighting. A section with no usable texture samples a neutral texel.
class DrawMaterials
{
  public:
    DrawMaterials(const VulkanDevice &device, package::ContentFiles &files,
                  object::ClassHierarchy &classes, object::ObjectResolver &resolver,
                  TextureBindings &bindings);

    // A section drawing `material` and, when given, lit by `light_map`; both
    // are references of `package`.
    [[nodiscard]] DrawMaterial Of(const package::Package &package, package::PackageIndex material,
                                  const std::optional<bsp::LightMap2D> &light_map);

    [[nodiscard]] const MaterialCensus &Census() const noexcept { return census_; }

  private:
    using TextureKey = std::tuple<const package::Package *, std::size_t, ColorSpace>;

    // Sets the surface's textures, blend, and opacity use.
    void ApplySurface(const material::MaterialSurface &surface, DrawMaterial &draw,
                      DrawTextures &textures);
    // Lights the section by its light map when every coefficient texture
    // resolves and can be sampled; otherwise leaves it as it is.
    void ApplyLightMap(const package::Package &package, const bsp::LightMap2D &light_map,
                       DrawMaterial &draw, DrawTextures &textures);
    // The GPU texture of an export, or null when its format cannot be
    // sampled.
    const GpuTexture *Upload(const object::ExportLocation &texture, ColorSpace space);
    const GpuTexture &TextureOr(const material::ColorTexture &input, ColorSpace space);
    VkDescriptorSet SetOf(const DrawTextures &textures);

    const VulkanDevice &device_;
    package::ContentFiles &files_;
    object::ClassHierarchy &classes_;
    object::ObjectResolver &resolver_;
    TextureBindings &bindings_;
    material::MaterialSurfaces surfaces_;
    GpuTexture neutral_;
    std::map<TextureKey, std::unique_ptr<GpuTexture>> textures_;
    std::map<DrawTextures, VkDescriptorSet> sets_;
    MaterialCensus census_;
};

} // namespace gears::engine::render
