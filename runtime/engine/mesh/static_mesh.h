#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "object/serialized_object.h"
#include "package/byte_reader.h"

namespace gears::engine::mesh
{

struct Vector3
{
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
};

// Three consecutive floats in the reader's byte order.
Vector3 ReadVector(package::ByteReader &reader);

struct Bounds
{
    Vector3 origin;
    Vector3 extent;
    float radius = 0.0F;
};

// One draw range of a LOD: its material and the index and vertex ranges it
// covers.
struct MeshSection
{
    object::PackageIndex material = 0;
    bool collision = false;
    std::uint32_t first_index = 0;
    std::uint32_t triangle_count = 0;
    std::uint32_t min_vertex = 0;
    std::uint32_t max_vertex = 0;
};

inline constexpr std::size_t kMaxTexCoords = 4;

struct MeshVertex
{
    Vector3 position;
    // Unit tangent basis decoded from the stored packed bytes.
    Vector3 tangent_x;
    Vector3 tangent_y;
    Vector3 normal;
    std::array<std::array<float, 2>, kMaxTexCoords> uv{};
};

struct StaticMeshLod
{
    std::vector<MeshSection> sections;
    std::size_t tex_coord_count = 0;
    std::vector<MeshVertex> vertices;
    std::vector<std::uint16_t> indices;
};

// A StaticMesh export's render data, read from its native serialization.
// The collision tree is validated and skipped; it is not render data.
class StaticMesh
{
  public:
    static StaticMesh Read(const object::SerializedObject &object);

    [[nodiscard]] const Bounds &MeshBounds() const noexcept { return bounds_; }
    [[nodiscard]] const std::vector<StaticMeshLod> &Lods() const noexcept { return lods_; }

  private:
    StaticMesh(Bounds bounds, std::vector<StaticMeshLod> lods)
        : bounds_(bounds), lods_(std::move(lods))
    {
    }

    Bounds bounds_;
    std::vector<StaticMeshLod> lods_;
};

} // namespace gears::engine::mesh
