#include "bsp_geometry.h"

#include <format>
#include <limits>

#include "mesh/vector_math.h"
#include "package/byte_reader.h"

namespace gears::engine::bsp
{
namespace
{

// Appends one node's outline as vertices and its fan as triangles.
void AppendNode(const BspModel &model, const BspNode &node, const std::optional<LightMap2D> &light,
                mesh::StaticMeshLod &lod)
{
    const BspSurface &surface = model.Surfaces()[static_cast<std::size_t>(node.surface)];
    const mesh::Vector3 &base = model.Points()[static_cast<std::size_t>(surface.texture_base)];
    const mesh::Vector3 &axis_u = model.Vectors()[static_cast<std::size_t>(surface.texture_u)];
    const mesh::Vector3 &axis_v = model.Vectors()[static_cast<std::size_t>(surface.texture_v)];
    std::size_t first = lod.vertices.size();
    if (first + node.vertex_count > std::numeric_limits<std::uint16_t>::max() + 1U)
    {
        throw package::PackageFormatError(
            std::format("model component needs more than {} vertices",
                        std::numeric_limits<std::uint16_t>::max() + 1U));
    }
    for (std::size_t k = 0; k < node.vertex_count; ++k)
    {
        const BspVertex &source = model.Vertices()[static_cast<std::size_t>(node.vertex_pool) + k];
        mesh::MeshVertex vertex;
        vertex.position = model.Points()[static_cast<std::size_t>(source.point)];
        vertex.normal = {node.plane_x, node.plane_y, node.plane_z};
        mesh::Vector3 offset = vertex.position - base;
        vertex.uv[0] = {mesh::Dot(offset, axis_u) / kTextureUnitsPerRepeat,
                        mesh::Dot(offset, axis_v) / kTextureUnitsPerRepeat};
        if (light)
        {
            for (std::size_t axis = 0; axis < 2U; ++axis)
            {
                vertex.uv[1][axis] = source.shadow_uv[axis] * light->coordinate_scale[axis] +
                                     light->coordinate_bias[axis];
            }
        }
        lod.vertices.push_back(vertex);
    }
    for (std::size_t k = 2; k < node.vertex_count; ++k)
    {
        lod.indices.push_back(static_cast<std::uint16_t>(first));
        lod.indices.push_back(static_cast<std::uint16_t>(first + k - 1U));
        lod.indices.push_back(static_cast<std::uint16_t>(first + k));
    }
}

} // namespace

ComponentMesh TriangulateComponent(const BspModel &model, const ModelComponent &component)
{
    ComponentMesh result;
    mesh::StaticMeshLod &lod = result.lod;
    lod.tex_coord_count = 2;
    for (const ModelElement &element : component.Elements())
    {
        mesh::MeshSection section;
        section.material = element.material;
        section.first_index = static_cast<std::uint32_t>(lod.indices.size());
        for (std::uint16_t index : element.nodes)
        {
            if (index >= model.Nodes().size())
            {
                throw package::PackageFormatError(
                    std::format("model element names node {} of {}", index, model.Nodes().size()));
            }
            AppendNode(model, model.Nodes()[index], element.light_map, lod);
        }
        section.triangle_count =
            static_cast<std::uint32_t>((lod.indices.size() - section.first_index) / 3U);
        if (section.triangle_count > 0U)
        {
            lod.sections.push_back(section);
            result.light_maps.push_back(element.light_map);
        }
    }
    return result;
}

} // namespace gears::engine::bsp
