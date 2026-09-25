#include "bsp_geometry.h"

#include <format>
#include <limits>

#include "package/byte_reader.h"

namespace gears::engine::bsp
{
namespace
{

float Dot(const mesh::Vector3 &a, const mesh::Vector3 &b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

mesh::Vector3 Subtract(const mesh::Vector3 &a, const mesh::Vector3 &b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

// Appends one node's outline as vertices and its fan as triangles.
void AppendNode(const BspModel &model, const BspNode &node, mesh::StaticMeshLod &lod)
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
        std::int32_t point = model.VertexPoints()[static_cast<std::size_t>(node.vertex_pool) + k];
        mesh::MeshVertex vertex;
        vertex.position = model.Points()[static_cast<std::size_t>(point)];
        vertex.normal = {node.plane_x, node.plane_y, node.plane_z};
        mesh::Vector3 offset = Subtract(vertex.position, base);
        vertex.uv[0] = {Dot(offset, axis_u) / kTextureUnitsPerRepeat,
                        Dot(offset, axis_v) / kTextureUnitsPerRepeat};
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

mesh::StaticMeshLod TriangulateComponent(const BspModel &model, const ModelComponent &component)
{
    mesh::StaticMeshLod lod;
    lod.tex_coord_count = 1;
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
            AppendNode(model, model.Nodes()[index], lod);
        }
        section.triangle_count =
            static_cast<std::uint32_t>((lod.indices.size() - section.first_index) / 3U);
        if (section.triangle_count > 0U)
        {
            lod.sections.push_back(section);
        }
    }
    return lod;
}

} // namespace gears::engine::bsp
