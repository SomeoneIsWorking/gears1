#include "bsp_model.h"

#include <format>
#include <utility>

#include "package/byte_reader.h"

namespace gears::engine::bsp
{
namespace
{

using package::ByteOrder;
using package::ByteReader;

// Stored sizes measured on the retail disc.
constexpr std::size_t kBoundsSize = 28;
constexpr std::size_t kVectorSize = 12;
constexpr std::size_t kNodeSize = 68;
constexpr std::size_t kSurfaceSize = 52;
constexpr std::size_t kVertexSize = 24;

std::vector<mesh::Vector3> ReadVectors(ByteReader &reader)
{
    std::size_t count = reader.ReadCount(kVectorSize);
    std::vector<mesh::Vector3> vectors;
    vectors.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        vectors.push_back(mesh::ReadVector(reader));
    }
    return vectors;
}

BspNode ReadNode(ByteReader &reader)
{
    BspNode node;
    node.plane_x = reader.ReadF32();
    node.plane_y = reader.ReadF32();
    node.plane_z = reader.ReadF32();
    node.plane_w = reader.ReadF32();
    (void)reader.ReadU64(); // zone mask
    node.vertex_pool = reader.ReadI32();
    node.surface = reader.ReadI32();
    (void)reader.ReadBytes(24); // vertex-buffer index, component, and BSP links
    (void)reader.ReadU8();      // front zone
    (void)reader.ReadU8();      // back zone
    node.vertex_count = reader.ReadU8();
    (void)reader.ReadU8();     // node flags
    (void)reader.ReadBytes(8); // leaves
    return node;
}

BspSurface ReadSurface(ByteReader &reader)
{
    BspSurface surface;
    surface.material = reader.ReadI32();
    surface.poly_flags = reader.ReadU32();
    surface.texture_base = reader.ReadI32();
    surface.normal = reader.ReadI32();
    surface.texture_u = reader.ReadI32();
    surface.texture_v = reader.ReadI32();
    (void)reader.ReadBytes(kSurfaceSize - 24U); // brush poly, actor, plane, shadow-map scale
    return surface;
}

bool Inside(std::int64_t index, std::size_t size)
{
    return index >= 0 && static_cast<std::uint64_t>(index) < size;
}

} // namespace

BspModel::BspModel(std::vector<mesh::Vector3> vectors, std::vector<mesh::Vector3> points,
                   std::vector<BspNode> nodes, std::vector<BspSurface> surfaces,
                   std::vector<BspVertex> vertices)
    : vectors_(std::move(vectors)), points_(std::move(points)), nodes_(std::move(nodes)),
      surfaces_(std::move(surfaces)), vertices_(std::move(vertices))
{
    Validate();
}

BspModel BspModel::Read(const object::SerializedObject &object)
{
    ByteReader reader(object.NativeData(), ByteOrder::Big);
    (void)reader.ReadBytes(kBoundsSize);
    std::vector<mesh::Vector3> vectors = ReadVectors(reader);
    std::vector<mesh::Vector3> points = ReadVectors(reader);
    std::size_t node_count = reader.ReadCount(kNodeSize);
    std::vector<BspNode> nodes;
    nodes.reserve(node_count);
    for (std::size_t i = 0; i < node_count; ++i)
    {
        nodes.push_back(ReadNode(reader));
    }
    // The surface array is saved with its owning model ahead of its count.
    if (reader.ReadI32() != object.Index())
    {
        reader.Fail("surface array is not owned by its model");
    }
    std::size_t surface_count = reader.ReadCount(kSurfaceSize);
    std::vector<BspSurface> surfaces;
    surfaces.reserve(surface_count);
    for (std::size_t i = 0; i < surface_count; ++i)
    {
        surfaces.push_back(ReadSurface(reader));
    }
    std::size_t vertex_count = reader.ReadCount(kVertexSize);
    std::vector<BspVertex> vertices;
    vertices.reserve(vertex_count);
    for (std::size_t i = 0; i < vertex_count; ++i)
    {
        BspVertex vertex;
        vertex.point = reader.ReadI32();
        (void)reader.ReadI32(); // side
        vertex.shadow_uv = {reader.ReadF32(), reader.ReadF32()};
        (void)reader.ReadBytes(kVertexSize - 16U); // back-face shadow coordinate
        vertices.push_back(vertex);
    }
    return {std::move(vectors), std::move(points), std::move(nodes), std::move(surfaces),
            std::move(vertices)};
}

void BspModel::Validate() const
{
    for (const BspNode &node : nodes_)
    {
        if (node.vertex_count == 0U)
        {
            continue;
        }
        if (!Inside(node.surface, surfaces_.size()) || node.vertex_pool < 0 ||
            !Inside(std::int64_t{node.vertex_pool} + node.vertex_count - 1, vertices_.size()))
        {
            throw package::PackageFormatError(
                std::format("node with surface {} and {} vertices at {} lies outside {} surfaces "
                            "and {} vertices",
                            node.surface, node.vertex_count, node.vertex_pool, surfaces_.size(),
                            vertices_.size()));
        }
        const BspSurface &surface = surfaces_[static_cast<std::size_t>(node.surface)];
        if (!Inside(surface.texture_base, points_.size()) ||
            !Inside(surface.texture_u, vectors_.size()) ||
            !Inside(surface.texture_v, vectors_.size()))
        {
            throw package::PackageFormatError(
                std::format("surface {} places its texture outside {} points and {} vectors",
                            node.surface, points_.size(), vectors_.size()));
        }
        for (std::size_t k = 0; k < node.vertex_count; ++k)
        {
            std::int32_t point = vertices_[static_cast<std::size_t>(node.vertex_pool) + k].point;
            if (!Inside(point, points_.size()))
            {
                throw package::PackageFormatError(
                    std::format("node vertex names point {} of {}", point, points_.size()));
            }
        }
    }
}

} // namespace gears::engine::bsp
