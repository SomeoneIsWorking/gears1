#include "static_mesh.h"

#include <bit>
#include <format>

#include "object/bulk_data.h"
#include "package/byte_reader.h"

namespace gears::engine::mesh
{
namespace
{

using package::ByteReader;

// Stored sizes measured on the retail disc (version-15 meshes).
constexpr std::size_t kCollisionNodeSize = 36;
constexpr std::size_t kCollisionTriangleSize = 16;
constexpr std::size_t kSectionSize = 28;
constexpr std::size_t kEdgeSize = 16;
constexpr std::size_t kVertexBaseSize = 28;
constexpr std::size_t kTexCoordSize = 8;
constexpr std::int32_t kMeshVersion = 15;

float ReadFloat(ByteReader &reader)
{
    return std::bit_cast<float>(reader.ReadU32());
}

Vector3 ReadVector(ByteReader &reader)
{
    Vector3 v;
    v.x = ReadFloat(reader);
    v.y = ReadFloat(reader);
    v.z = ReadFloat(reader);
    return v;
}

// A packed unit vector: x in the lowest byte, each byte mapping 0..255 to
// -1..1.
Vector3 UnpackNormal(std::uint32_t packed)
{
    auto component = [packed](unsigned shift)
    { return static_cast<float>((packed >> shift) & 0xFFU) / 127.5F - 1.0F; };
    return {component(0U), component(8U), component(16U)};
}

void SkipArray(ByteReader &reader, std::size_t element_size)
{
    std::size_t count = reader.ReadCount(element_size);
    (void)reader.ReadBytes(count * element_size);
}

MeshSection ReadSection(ByteReader &reader)
{
    MeshSection section;
    section.material = reader.ReadI32();
    section.collision = reader.ReadU32() != 0U;
    (void)reader.ReadU32(); // the collision setting before the last edit
    section.first_index = reader.ReadU32();
    section.triangle_count = reader.ReadU32();
    section.min_vertex = reader.ReadU32();
    section.max_vertex = reader.ReadU32();
    return section;
}

void ReadVertices(ByteReader &reader, StaticMeshLod &lod)
{
    lod.tex_coord_count = reader.ReadU32();
    std::size_t stride = reader.ReadU32();
    std::size_t vertex_count = reader.ReadU32();
    if (lod.tex_coord_count == 0U || lod.tex_coord_count > kMaxTexCoords ||
        stride != kVertexBaseSize + lod.tex_coord_count * kTexCoordSize)
    {
        reader.Fail(std::format("vertex stride {} does not fit {} texture coordinate set(s)",
                                stride, lod.tex_coord_count));
    }
    if (reader.ReadCount(stride) != vertex_count)
    {
        reader.Fail("vertex array length disagrees with the vertex count");
    }
    lod.vertices.resize(vertex_count);
    for (MeshVertex &vertex : lod.vertices)
    {
        vertex.position = ReadVector(reader);
        (void)reader.ReadU32(); // pads the position to 16 bytes
        vertex.tangent_x = UnpackNormal(reader.ReadU32());
        vertex.tangent_y = UnpackNormal(reader.ReadU32());
        vertex.normal = UnpackNormal(reader.ReadU32());
        for (std::size_t i = 0; i < lod.tex_coord_count; ++i)
        {
            vertex.uv[i][0] = ReadFloat(reader);
            vertex.uv[i][1] = ReadFloat(reader);
        }
    }
    if (reader.ReadU32() != vertex_count)
    {
        reader.Fail("LOD vertex count disagrees with its vertex buffer");
    }
}

StaticMeshLod ReadLod(ByteReader &reader, std::size_t data_base)
{
    StaticMeshLod lod;
    // The uncooked source triangles; cooked meshes store them empty.
    (void)object::BulkData::Read(reader, data_base);
    std::size_t section_count = reader.ReadCount(kSectionSize);
    lod.sections.reserve(section_count);
    for (std::size_t i = 0; i < section_count; ++i)
    {
        lod.sections.push_back(ReadSection(reader));
    }
    ReadVertices(reader, lod);
    std::size_t index_count = reader.ReadCount(2U);
    lod.indices.reserve(index_count);
    for (std::size_t i = 0; i < index_count; ++i)
    {
        std::span<const std::uint8_t> bytes = reader.ReadBytes(2U);
        auto index = static_cast<std::uint16_t>((bytes[0] << 8U) | bytes[1]);
        if (index >= lod.vertices.size())
        {
            reader.Fail(std::format("index {} exceeds {} vertices", index, lod.vertices.size()));
        }
        lod.indices.push_back(index);
    }
    SkipArray(reader, 2U);        // wireframe indices
    SkipArray(reader, kEdgeSize); // edges
    SkipArray(reader, 1U);        // shadow double-sided flags
    for (const MeshSection &section : lod.sections)
    {
        if (section.first_index + section.triangle_count * 3U > lod.indices.size())
        {
            reader.Fail("section covers indices beyond the index buffer");
        }
    }
    return lod;
}

} // namespace

StaticMesh StaticMesh::Read(const object::SerializedObject &object)
{
    ByteReader reader(object.NativeData(), package::ByteOrder::Big);
    Bounds bounds;
    bounds.origin = ReadVector(reader);
    bounds.extent = ReadVector(reader);
    bounds.radius = ReadFloat(reader);
    (void)reader.ReadI32(); // collision body setup, also a property
    SkipArray(reader, kCollisionNodeSize);
    SkipArray(reader, kCollisionTriangleSize);
    std::int32_t version = reader.ReadI32();
    if (version != kMeshVersion)
    {
        reader.Fail(std::format("static mesh version {} is not {}", version, kMeshVersion));
    }
    std::size_t lod_count = reader.ReadCount(1U);
    std::vector<StaticMeshLod> lods;
    lods.reserve(lod_count);
    for (std::size_t i = 0; i < lod_count; ++i)
    {
        lods.push_back(ReadLod(reader, object.NativeBase()));
    }
    // Trailer measured as five words on every retail mesh read so far.
    (void)reader.ReadBytes(20U);
    if (reader.Remaining() != 0U)
    {
        reader.Fail(std::format("{} byte(s) follow the mesh", reader.Remaining()));
    }
    return {bounds, std::move(lods)};
}

} // namespace gears::engine::mesh
