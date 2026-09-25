#include "level_renderer.h"

#include <optional>

#include "mesh/static_mesh.h"
#include "object/serialized_object.h"
#include "texture/texture2d.h"

namespace gears::engine::render
{
namespace
{

// The base colour of a section whose material gives it no texture.
constexpr std::array<std::uint8_t, 4> kUntexturedColor{150, 146, 140, 255};

} // namespace

LevelRenderer::LevelRenderer(const VulkanDevice &device, VkExtent2D extent,
                             package::ContentFiles &files, object::ClassHierarchy &classes,
                             object::ObjectResolver &resolver)
    : device_(device), files_(files), classes_(classes), resolver_(resolver),
      materials_(classes, resolver), target_(device, extent), bindings_(device, kMaxTextures),
      renderer_(device, target_, bindings_.Layout()), untextured_(device, kUntexturedColor)
{
}

const LevelRenderer::PreparedMesh &LevelRenderer::MeshOf(const object::ExportLocation &mesh)
{
    Key key{mesh.package, mesh.export_index};
    auto found = meshes_.find(key);
    if (found != meshes_.end())
    {
        return found->second;
    }
    auto object = object::SerializedObject::Read(*mesh.package, mesh.export_index, classes_);
    auto decoded = mesh::StaticMesh::Read(object);
    PreparedMesh prepared;
    if (!decoded.Lods().empty())
    {
        const mesh::StaticMeshLod &lod = decoded.Lods()[0];
        prepared.gpu = std::make_unique<GpuMesh>(device_, lod);
        for (const mesh::MeshSection &section : lod.sections)
        {
            prepared.materials.push_back(section.material);
        }
        ++census_.meshes;
    }
    return meshes_.emplace(key, std::move(prepared)).first->second;
}

const GpuTexture &LevelRenderer::TextureOf(const material::ColorTexture &input)
{
    if (input.outcome != material::ColorOutcome::kTexture)
    {
        return untextured_;
    }
    Key key{input.texture.package, input.texture.export_index};
    auto found = textures_.find(key);
    if (found == textures_.end())
    {
        auto object = object::SerializedObject::Read(*input.texture.package,
                                                     input.texture.export_index, classes_);
        auto decoded = texture::Texture2D::Read(object);
        std::unique_ptr<GpuTexture> uploaded;
        if (GpuTexture::CanSample(decoded.Format()))
        {
            uploaded = std::make_unique<GpuTexture>(device_, decoded, files_);
            ++census_.textures;
        }
        found = textures_.emplace(key, std::move(uploaded)).first;
    }
    return found->second ? *found->second : untextured_;
}

VkDescriptorSet LevelRenderer::SetOf(const GpuTexture &color, const GpuTexture &opacity)
{
    std::pair key{&color, &opacity};
    auto found = sets_.find(key);
    if (found == sets_.end())
    {
        found = sets_.emplace(key, bindings_.Bind(color, opacity)).first;
    }
    return found->second;
}

DrawMaterial LevelRenderer::FromSurface(const material::MaterialSurface &surface)
{
    const GpuTexture &color = TextureOf(surface.color);
    std::string color_source(material::NameOf(surface.color.outcome));
    if (surface.color.outcome == material::ColorOutcome::kTexture && &color == &untextured_)
    {
        color_source = "texture format not sampled";
    }
    ++census_.section_colors[color_source];
    ++census_.section_blends[std::string(material::NameOf(surface.blend))];

    DrawMaterial draw;
    draw.lit = !surface.unlit;
    draw.clip = surface.opacity_clip;
    // A computed opacity is not evaluated; the section keeps full opacity.
    const GpuTexture &opacity = TextureOf(surface.opacity);
    bool textured_opacity = &opacity != &untextured_;
    switch (surface.opacity.channel)
    {
    case material::Channel::kColor:
    case material::Channel::kRed:
        draw.channel = 0;
        break;
    case material::Channel::kGreen:
        draw.channel = 1;
        break;
    case material::Channel::kBlue:
        draw.channel = 2;
        break;
    case material::Channel::kAlpha:
        draw.channel = 3;
        break;
    }
    switch (surface.blend)
    {
    case material::BlendMode::kOpaque:
        break;
    case material::BlendMode::kMasked:
        draw.opacity = textured_opacity ? OpacityUse::kAlphaTest : OpacityUse::kNone;
        break;
    case material::BlendMode::kTranslucent:
        draw.blend = Blend::kAlpha;
        draw.opacity = textured_opacity ? OpacityUse::kBlend : OpacityUse::kNone;
        break;
    case material::BlendMode::kAdditive:
        draw.blend = Blend::kAdditive;
        break;
    case material::BlendMode::kModulative:
        draw.blend = Blend::kModulative;
        break;
    }
    draw.textures = SetOf(color, opacity);
    return draw;
}

DrawMaterial LevelRenderer::MaterialOf(const package::Package &package,
                                       package::PackageIndex material)
{
    object::Resolution resolved = resolver_.Resolve(package, material);
    if (resolved.status != object::ResolutionStatus::kFound)
    {
        ++census_.section_colors[resolved.status == object::ResolutionStatus::kNull
                                     ? "no material"
                                     : "material cooked out"];
        DrawMaterial untextured;
        untextured.textures = SetOf(untextured_, untextured_);
        return untextured;
    }
    return FromSurface(materials_.Surface(resolved.location));
}

void LevelRenderer::AddDraw(const GpuMesh &mesh, const GpuSection &section,
                            const DrawMaterial &material, const scene::Matrix &world)
{
    (material.blend == Blend::kOpaque ? opaque_draws_ : blended_draws_)
        .push_back({&mesh, section, material, world});
}

void LevelRenderer::Prepare(const package::Package &level, const scene::LevelScene &scene)
{
    PrepareMeshes(level, scene);
    PrepareModels(scene);
    census_.draws = opaque_draws_.size() + blended_draws_.size();
}

void LevelRenderer::PrepareMeshes(const package::Package &level, const scene::LevelScene &scene)
{
    for (const scene::MeshInstance &instance : scene.Instances())
    {
        const PreparedMesh &mesh = MeshOf(instance.mesh);
        if (!mesh.gpu)
        {
            ++census_.placements_without_lod;
            continue;
        }
        for (std::size_t i = 0; i < mesh.gpu->Sections().size(); ++i)
        {
            // A component's own material for a section replaces the mesh's.
            bool overridden =
                i < instance.material_overrides.size() && instance.material_overrides[i] != 0;
            DrawMaterial material = overridden
                                        ? MaterialOf(level, instance.material_overrides[i])
                                        : MaterialOf(*instance.mesh.package, mesh.materials[i]);
            AddDraw(*mesh.gpu, mesh.gpu->Sections()[i], material, instance.world);
        }
    }
}

void LevelRenderer::PrepareModels(const scene::LevelScene &scene)
{
    for (const scene::ModelInstance &model : scene.Models())
    {
        if (model.geometry.indices.empty())
        {
            ++census_.models_without_triangles;
            continue;
        }
        models_.push_back(std::make_unique<GpuMesh>(device_, model.geometry));
        const GpuMesh &gpu = *models_.back();
        for (std::size_t i = 0; i < gpu.Sections().size(); ++i)
        {
            AddDraw(gpu, gpu.Sections()[i],
                    MaterialOf(*model.package, model.geometry.sections[i].material),
                    scene::Matrix::Identity());
        }
        ++census_.models;
    }
}

std::vector<std::uint8_t> LevelRenderer::Render(const scene::Camera &camera)
{
    scene::Matrix view_projection = camera.ViewProjection();
    device_.Submit(
        [&](VkCommandBuffer commands)
        {
            target_.Begin(commands);
            renderer_.Begin(commands);
            renderer_.Bind(commands, Blend::kOpaque);
            for (const Draw &draw : opaque_draws_)
            {
                renderer_.Draw(commands, *draw.mesh, draw.section, draw.material, draw.world,
                               view_projection);
            }
            // Blended sections draw over the finished opaque depth.
            std::optional<Blend> bound;
            for (const Draw &draw : blended_draws_)
            {
                if (bound != draw.material.blend)
                {
                    renderer_.Bind(commands, draw.material.blend);
                    bound = draw.material.blend;
                }
                renderer_.Draw(commands, *draw.mesh, draw.section, draw.material, draw.world,
                               view_projection);
            }
            target_.EndAndCopy(commands);
        });
    return target_.Pixels();
}

} // namespace gears::engine::render
