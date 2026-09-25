#include "level_renderer.h"

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
      renderer_(device, target_, bindings_.Layout()), untextured_(device, kUntexturedColor),
      untextured_set_(bindings_.Bind(untextured_))
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

VkDescriptorSet LevelRenderer::Upload(const object::ExportLocation &texture)
{
    Key key{texture.package, texture.export_index};
    auto found = texture_sets_.find(key);
    if (found != texture_sets_.end())
    {
        return found->second;
    }
    auto object = object::SerializedObject::Read(*texture.package, texture.export_index, classes_);
    auto decoded = texture::Texture2D::Read(object);
    VkDescriptorSet set = untextured_set_;
    if (GpuTexture::CanSample(decoded.Format()))
    {
        textures_.push_back(std::make_unique<GpuTexture>(device_, decoded, files_));
        set = bindings_.Bind(*textures_.back());
        ++census_.textures;
    }
    texture_sets_.emplace(key, set);
    return set;
}

VkDescriptorSet LevelRenderer::TextureOf(const package::Package &package,
                                         package::PackageIndex material)
{
    object::Resolution resolved = resolver_.Resolve(package, material);
    if (resolved.status == object::ResolutionStatus::kNull)
    {
        ++census_.section_colors["no material"];
        return untextured_set_;
    }
    if (resolved.status == object::ResolutionStatus::kCookedOut)
    {
        ++census_.section_colors["material cooked out"];
        return untextured_set_;
    }
    material::ColorTexture color = materials_.BaseColor(resolved.location);
    if (color.outcome != material::ColorOutcome::kTexture)
    {
        ++census_.section_colors[std::string(material::NameOf(color.outcome))];
        return untextured_set_;
    }
    VkDescriptorSet set = Upload(color.texture);
    ++census_.section_colors[set == untextured_set_ ? "texture format not sampled" : "texture"];
    return set;
}

void LevelRenderer::Prepare(const package::Package &level, const scene::LevelScene &scene)
{
    PrepareMeshes(level, scene);
    PrepareModels(scene);
    census_.draws = draws_.size();
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
            VkDescriptorSet texture = overridden
                                          ? TextureOf(level, instance.material_overrides[i])
                                          : TextureOf(*instance.mesh.package, mesh.materials[i]);
            draws_.push_back({mesh.gpu.get(), mesh.gpu->Sections()[i], texture, instance.world});
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
            VkDescriptorSet texture =
                TextureOf(*model.package, model.geometry.sections[i].material);
            draws_.push_back({&gpu, gpu.Sections()[i], texture, scene::Matrix::Identity()});
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
            renderer_.Bind(commands);
            for (const Draw &draw : draws_)
            {
                renderer_.Draw(commands, *draw.mesh, draw.section, draw.texture, draw.world,
                               view_projection);
            }
            target_.EndAndCopy(commands);
        });
    return target_.Pixels();
}

} // namespace gears::engine::render
