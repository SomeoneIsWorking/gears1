#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "object/serialized_object.h"
#include "static_mesh.h"

namespace gears::engine::mesh
{

// A rotation as a unit quaternion.
struct Quaternion
{
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
    float w = 1.0F;
};

// One bone of the reference skeleton: its pose relative to its parent. The
// root is its own parent (index 0).
struct MeshBone
{
    std::string name;
    Quaternion orientation;
    Vector3 position;
    std::uint32_t child_count = 0;
    std::int32_t parent = 0;
};

// Bone influences a skinned vertex stores.
inline constexpr std::size_t kMaxInfluences = 4;

// A vertex of the skinned vertex buffer: its reference-pose position and
// tangent basis, one texture coordinate, and up to four bone influences.
// Each influence's bone indexes its chunk's bone map; weights sum to 255.
struct SkinnedVertex
{
    Vector3 position;
    Vector3 tangent_x;
    Vector3 tangent_y;
    Vector3 normal;
    std::array<float, 2> uv{};
    std::array<std::uint8_t, kMaxInfluences> bones{};
    std::array<std::uint8_t, kMaxInfluences> weights{};
};

// A range of vertices skinned by one set of bones: rigid vertices (one
// influence) first, then soft ones.
struct SkinChunk
{
    std::uint32_t base_vertex = 0;
    // Chunk-local bone index to reference-skeleton bone index.
    std::vector<std::uint16_t> bone_map;
    std::uint32_t rigid_vertices = 0;
    std::uint32_t soft_vertices = 0;
    std::uint32_t max_influences = 0;
};

// One draw range of a skeletal LOD: an index into the mesh's materials, the
// chunk whose bones skin it, and the triangles it covers.
struct SkeletalSection
{
    std::uint16_t material = 0;
    std::uint16_t chunk = 0;
    std::uint32_t first_index = 0;
    std::uint32_t triangle_count = 0;
};

struct SkeletalMeshLod
{
    std::vector<SkeletalSection> sections;
    std::vector<std::uint16_t> indices;
    std::vector<SkinChunk> chunks;
    std::vector<SkinnedVertex> vertices;
    // Reference-skeleton bones this LOD needs posed, in ascending order.
    std::vector<std::uint8_t> required_bones;
};

// A SkeletalMesh export's render data, read from its native serialization:
// bounds, its origin rotation, materials, the reference skeleton, and each
// LOD's skinned geometry. The stored origin offset is zero on every mesh
// measured so far and is not kept. Cooked meshes keep only the GPU vertex buffer; the per-chunk
// source vertex arrays are empty.
class SkeletalMesh
{
  public:
    // Refuses a mesh with no LOD, or whose layout differs from the measured
    // one anywhere, including bytes left after the bone-name map.
    static SkeletalMesh Read(const object::SerializedObject &object);

    [[nodiscard]] const Bounds &MeshBounds() const noexcept { return bounds_; }
    // The rotation (pitch, yaw, roll in 65536ths of a turn) that turns the
    // mesh's own axes into its component's.
    [[nodiscard]] const std::array<std::int32_t, 3> &OriginRotation() const noexcept
    {
        return origin_rotation_;
    }
    [[nodiscard]] const std::vector<object::PackageIndex> &Materials() const noexcept
    {
        return materials_;
    }
    [[nodiscard]] const std::vector<MeshBone> &Skeleton() const noexcept { return skeleton_; }
    // Never empty.
    [[nodiscard]] const std::vector<SkeletalMeshLod> &Lods() const noexcept { return lods_; }

  private:
    SkeletalMesh(Bounds bounds, std::array<std::int32_t, 3> origin_rotation,
                 std::vector<object::PackageIndex> materials, std::vector<MeshBone> skeleton,
                 std::vector<SkeletalMeshLod> lods)
        : bounds_(bounds), origin_rotation_(origin_rotation), materials_(std::move(materials)),
          skeleton_(std::move(skeleton)), lods_(std::move(lods))
    {
    }

    Bounds bounds_;
    std::array<std::int32_t, 3> origin_rotation_;
    std::vector<object::PackageIndex> materials_;
    std::vector<MeshBone> skeleton_;
    std::vector<SkeletalMeshLod> lods_;
};

// The LOD's reference pose as static geometry: each section's material is
// the mesh material it names, and the vertices keep their stored
// reference-pose positions.
[[nodiscard]] StaticMeshLod ReferencePose(const SkeletalMesh &mesh, const SkeletalMeshLod &lod);

} // namespace gears::engine::mesh
