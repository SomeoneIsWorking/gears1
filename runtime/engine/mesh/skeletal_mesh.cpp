#include "skeletal_mesh.h"

#include <algorithm>
#include <format>
#include <span>
#include <string_view>
#include <utility>

#include "object/bulk_data.h"
#include "package/byte_reader.h"

namespace gears::engine::mesh
{
namespace
{

using package::ByteReader;

// Stored sizes measured on the retail disc.
constexpr std::size_t kBoneSize = 48;
constexpr std::size_t kSectionSize = 10;
constexpr std::size_t kChunkMinSize = 28;
constexpr std::size_t kEdgeSize = 16;
constexpr std::size_t kSkinnedVertexSize = 40;
constexpr std::size_t kNameMapEntrySize = 12;
// The smallest LOD: its array counts, sizes and an empty bulk-data header.
constexpr std::size_t kLodMinSize = 60;

// A packed unit vector: x in the lowest byte, each byte mapping 0..255 to
// -1..1 (the static mesh encoding).
Vector3 UnpackNormal(std::uint32_t packed)
{
    auto component = [packed](unsigned shift)
    { return static_cast<float>((packed >> shift) & 0xFFU) / 127.5F - 1.0F; };
    return {component(0U), component(8U), component(16U)};
}

// Four bytes stored as one word, the first element in its lowest byte.
std::array<std::uint8_t, kMaxInfluences> UnpackBytes(std::uint32_t packed)
{
    std::array<std::uint8_t, kMaxInfluences> bytes{};
    for (std::size_t i = 0; i < kMaxInfluences; ++i)
    {
        bytes[i] = static_cast<std::uint8_t>((packed >> (8U * i)) & 0xFFU);
    }
    return bytes;
}

std::vector<std::uint16_t> ReadU16Array(ByteReader &reader)
{
    std::size_t count = reader.ReadCount(2U);
    std::vector<std::uint16_t> values;
    values.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        values.push_back(reader.ReadU16());
    }
    return values;
}

std::vector<std::uint8_t> ReadU8Array(ByteReader &reader)
{
    std::size_t count = reader.ReadCount(1U);
    std::span<const std::uint8_t> bytes = reader.ReadBytes(count);
    return {bytes.begin(), bytes.end()};
}

// An array cooked meshes store empty; refuses one with elements, whose
// layout is unmeasured.
void RequireEmpty(ByteReader &reader, std::string_view what)
{
    if (reader.ReadU32() != 0U)
    {
        reader.Fail(std::format("{} is not empty in a cooked mesh", what));
    }
}

MeshBone ReadBone(ByteReader &reader, const object::SerializedObject &object)
{
    MeshBone bone;
    bone.name = object.Owner().NameText(package::ReadNameReference(reader));
    (void)reader.ReadU32(); // flags
    bone.orientation.x = reader.ReadF32();
    bone.orientation.y = reader.ReadF32();
    bone.orientation.z = reader.ReadF32();
    bone.orientation.w = reader.ReadF32();
    bone.position = ReadVector(reader);
    bone.child_count = reader.ReadU32();
    bone.parent = reader.ReadI32();
    return bone;
}

SkinChunk ReadChunk(ByteReader &reader)
{
    SkinChunk chunk;
    chunk.base_vertex = reader.ReadU32();
    RequireEmpty(reader, "a chunk's rigid source vertices");
    RequireEmpty(reader, "a chunk's soft source vertices");
    chunk.bone_map = ReadU16Array(reader);
    chunk.rigid_vertices = reader.ReadU32();
    chunk.soft_vertices = reader.ReadU32();
    chunk.max_influences = reader.ReadU32();
    if (chunk.max_influences == 0U || chunk.max_influences > kMaxInfluences)
    {
        reader.Fail(std::format("chunk has {} influences per vertex", chunk.max_influences));
    }
    return chunk;
}

SkinnedVertex ReadSkinnedVertex(ByteReader &reader)
{
    SkinnedVertex vertex;
    vertex.position = ReadVector(reader);
    vertex.tangent_x = UnpackNormal(reader.ReadU32());
    vertex.tangent_y = UnpackNormal(reader.ReadU32());
    vertex.normal = UnpackNormal(reader.ReadU32());
    vertex.uv = {reader.ReadF32(), reader.ReadF32()};
    vertex.bones = UnpackBytes(reader.ReadU32());
    vertex.weights = UnpackBytes(reader.ReadU32());
    return vertex;
}

void Validate(ByteReader &reader, const SkeletalMeshLod &lod, std::size_t bone_count,
              std::size_t material_count)
{
    for (const SkeletalSection &section : lod.sections)
    {
        std::size_t count = std::size_t{section.triangle_count} * 3U;
        if (section.first_index > lod.indices.size() ||
            count > lod.indices.size() - section.first_index)
        {
            reader.Fail("section covers indices beyond the index buffer");
        }
        if (section.chunk >= lod.chunks.size() || section.material >= material_count)
        {
            reader.Fail(std::format("section names chunk {} of {} and material {} of {}",
                                    section.chunk, lod.chunks.size(), section.material,
                                    material_count));
        }
    }
    for (std::uint16_t index : lod.indices)
    {
        if (index >= lod.vertices.size())
        {
            reader.Fail(std::format("index {} exceeds {} vertices", index, lod.vertices.size()));
        }
    }
    for (const SkinChunk &chunk : lod.chunks)
    {
        std::size_t end = std::size_t{chunk.base_vertex} + chunk.rigid_vertices +
                          chunk.soft_vertices;
        if (end > lod.vertices.size())
        {
            reader.Fail("chunk covers vertices beyond the vertex buffer");
        }
        for (std::uint16_t bone : chunk.bone_map)
        {
            if (bone >= bone_count)
            {
                reader.Fail(std::format("chunk maps bone {} of {}", bone, bone_count));
            }
        }
        for (std::size_t v = chunk.base_vertex; v < end; ++v)
        {
            for (std::size_t i = 0; i < kMaxInfluences; ++i)
            {
                if (lod.vertices[v].weights[i] != 0U &&
                    lod.vertices[v].bones[i] >= chunk.bone_map.size())
                {
                    reader.Fail(std::format("vertex {} names chunk bone {} of {}", v,
                                            lod.vertices[v].bones[i], chunk.bone_map.size()));
                }
            }
        }
    }
}

SkeletalMeshLod ReadLod(ByteReader &reader, std::size_t data_base, std::size_t bone_count,
                        std::size_t material_count)
{
    SkeletalMeshLod lod;
    std::size_t section_count = reader.ReadCount(kSectionSize);
    lod.sections.reserve(section_count);
    for (std::size_t i = 0; i < section_count; ++i)
    {
        SkeletalSection section;
        section.material = reader.ReadU16();
        section.chunk = reader.ReadU16();
        section.first_index = reader.ReadU32();
        section.triangle_count = reader.ReadU16();
        lod.sections.push_back(section);
    }
    lod.indices = ReadU16Array(reader);
    (void)ReadU16Array(reader); // shadow volume indices
    (void)ReadU16Array(reader); // bones any chunk uses
    (void)ReadU8Array(reader);  // shadow triangle double-sided flags
    std::size_t chunk_count = reader.ReadCount(kChunkMinSize);
    lod.chunks.reserve(chunk_count);
    for (std::size_t i = 0; i < chunk_count; ++i)
    {
        lod.chunks.push_back(ReadChunk(reader));
    }
    (void)reader.ReadU32(); // size
    std::uint32_t vertex_count = reader.ReadU32();
    std::size_t edge_count = reader.ReadCount(kEdgeSize);
    (void)reader.ReadBytes(edge_count * kEdgeSize);
    lod.required_bones = ReadU8Array(reader);
    // Each vertex's source point, for editing tools.
    (void)object::BulkData::Read(reader, data_base);
    std::size_t stored_vertices = reader.ReadCount(kSkinnedVertexSize);
    if (stored_vertices != vertex_count)
    {
        reader.Fail(std::format("LOD counts {} vertices but its buffer holds {}", vertex_count,
                                stored_vertices));
    }
    lod.vertices.reserve(stored_vertices);
    for (std::size_t i = 0; i < stored_vertices; ++i)
    {
        lod.vertices.push_back(ReadSkinnedVertex(reader));
    }
    Validate(reader, lod, bone_count, material_count);
    return lod;
}

} // namespace

SkeletalMesh SkeletalMesh::Read(const object::SerializedObject &object)
{
    ByteReader reader(object.NativeData(), package::ByteOrder::Big);
    std::size_t data_base = object.NativeBase();
    Bounds bounds;
    bounds.origin = ReadVector(reader);
    bounds.extent = ReadVector(reader);
    bounds.radius = reader.ReadF32();
    std::size_t material_count = reader.ReadCount(4U);
    std::vector<object::PackageIndex> materials;
    materials.reserve(material_count);
    for (std::size_t i = 0; i < material_count; ++i)
    {
        materials.push_back(reader.ReadI32());
    }
    Vector3 origin = ReadVector(reader);
    if (origin.x != 0.0F || origin.y != 0.0F || origin.z != 0.0F)
    {
        reader.Fail("mesh origin offset is not zero");
    }
    std::array<std::int32_t, 3> origin_rotation{reader.ReadI32(), reader.ReadI32(),
                                                reader.ReadI32()};
    std::size_t bone_count = reader.ReadCount(kBoneSize);
    std::vector<MeshBone> skeleton;
    skeleton.reserve(bone_count);
    for (std::size_t i = 0; i < bone_count; ++i)
    {
        skeleton.push_back(ReadBone(reader, object));
        if (skeleton.back().parent < 0 ||
            static_cast<std::size_t>(skeleton.back().parent) >= std::max<std::size_t>(i, 1U))
        {
            reader.Fail(std::format("bone {} names parent {}", i, skeleton.back().parent));
        }
    }
    (void)reader.ReadI32(); // skeleton depth
    std::size_t lod_count = reader.ReadCount(kLodMinSize);
    if (lod_count == 0U)
    {
        reader.Fail("skeletal mesh has no LOD");
    }
    std::vector<SkeletalMeshLod> lods;
    lods.reserve(lod_count);
    for (std::size_t i = 0; i < lod_count; ++i)
    {
        lods.push_back(ReadLod(reader, data_base, bone_count, material_count));
    }
    // Bone name to index; the skeleton already names its bones.
    std::size_t name_count = reader.ReadCount(kNameMapEntrySize);
    (void)reader.ReadBytes(name_count * kNameMapEntrySize);
    if (reader.Remaining() != 0U)
    {
        reader.Fail(std::format("{} byte(s) follow the skeletal mesh", reader.Remaining()));
    }
    return {bounds, origin_rotation, std::move(materials), std::move(skeleton), std::move(lods)};
}

StaticMeshLod ReferencePose(const SkeletalMesh &mesh, const SkeletalMeshLod &lod)
{
    StaticMeshLod pose;
    pose.tex_coord_count = 1;
    pose.indices = lod.indices;
    pose.vertices.reserve(lod.vertices.size());
    for (const SkinnedVertex &skinned : lod.vertices)
    {
        MeshVertex vertex;
        vertex.position = skinned.position;
        vertex.tangent_x = skinned.tangent_x;
        vertex.tangent_y = skinned.tangent_y;
        vertex.normal = skinned.normal;
        vertex.uv[0] = skinned.uv;
        pose.vertices.push_back(vertex);
    }
    for (const SkeletalSection &section : lod.sections)
    {
        MeshSection static_section;
        static_section.material = mesh.Materials()[section.material];
        static_section.first_index = section.first_index;
        static_section.triangle_count = section.triangle_count;
        static_section.max_vertex =
            lod.vertices.empty() ? 0U : static_cast<std::uint32_t>(lod.vertices.size() - 1U);
        pose.sections.push_back(static_section);
    }
    return pose;
}

} // namespace gears::engine::mesh
