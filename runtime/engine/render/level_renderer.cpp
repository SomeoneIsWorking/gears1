#include "level_renderer.h"

#include <optional>

#include "mesh/static_mesh.h"
#include "object/serialized_object.h"

namespace gears::engine::render
{
LevelRenderer::LevelRenderer(const VulkanDevice &device, VkExtent2D extent,
                             package::ContentFiles &files, object::ClassHierarchy &classes,
                             object::ObjectResolver &resolver)
    : device_(device), classes_(classes), target_(device, extent), bindings_(device, kMaxTextures),
      frame_(device), renderer_(device, target_, bindings_.Layout(), frame_.Layout()),
      materials_(device, files, classes, resolver, bindings_)
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
            DrawMaterial material =
                overridden ? materials_.Of(level, instance.material_overrides[i], std::nullopt)
                           : materials_.Of(*instance.mesh.package, mesh.materials[i], std::nullopt);
            AddDraw(*mesh.gpu, mesh.gpu->Sections()[i], material, instance.world);
        }
    }
}

void LevelRenderer::PrepareModels(const scene::LevelScene &scene)
{
    for (const scene::ModelInstance &model : scene.Models())
    {
        const mesh::StaticMeshLod &lod = model.geometry.lod;
        if (lod.indices.empty())
        {
            ++census_.models_without_triangles;
            continue;
        }
        models_.push_back(std::make_unique<GpuMesh>(device_, lod));
        const GpuMesh &gpu = *models_.back();
        for (std::size_t i = 0; i < gpu.Sections().size(); ++i)
        {
            AddDraw(gpu, gpu.Sections()[i],
                    materials_.Of(*model.package, lod.sections[i].material,
                                  model.geometry.light_maps[i]),
                    scene::Matrix::Identity());
        }
        ++census_.models;
    }
}

std::vector<std::uint8_t> LevelRenderer::Render(const scene::Camera &camera)
{
    frame_.Write(camera.ViewProjection());
    device_.Submit(
        [&](VkCommandBuffer commands)
        {
            target_.Begin(commands);
            renderer_.Begin(commands, frame_.Set());
            renderer_.Bind(commands, Blend::kOpaque);
            for (const Draw &draw : opaque_draws_)
            {
                renderer_.Draw(commands, *draw.mesh, draw.section, draw.material, draw.world);
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
                renderer_.Draw(commands, *draw.mesh, draw.section, draw.material, draw.world);
            }
            target_.EndAndCopy(commands);
        });
    return target_.Pixels();
}

} // namespace gears::engine::render
