#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include "mesh/static_mesh.h"
#include "scene/static_meshes.h"
#include "scene/transform.h"
#include "scene/world.h"

namespace gears::engine::game
{

struct CollisionTriangle
{
    mesh::Vector3 a;
    mesh::Vector3 b;
    mesh::Vector3 c;
    // Unit normal of the winding a, b, c.
    mesh::Vector3 normal;
};

// The first contact of a swept shape: the fraction of the move completed
// before touching, the surface normal facing the shape, and the contact
// point on the surface.
struct SweepHit
{
    float time = 1.0F;
    mesh::Vector3 normal;
    mesh::Vector3 point;
};

// The level's solid triangles in world space, bucketed on a uniform grid:
// every BSP surface and every static mesh section flagged for collision.
// Both faces of a triangle block.
class CollisionWorld
{
  public:
    // World units per grid cell edge.
    static constexpr float kCellSize = 512.0F;

    static CollisionWorld Build(const scene::World &world, scene::StaticMeshes &meshes);

    // Adds a triangle; one with no area is ignored.
    void Add(mesh::Vector3 a, mesh::Vector3 b, mesh::Vector3 c);

    // A sphere of `radius` centred at `start` moving by `delta`: the first
    // triangle it touches, none when the path is clear. A sphere that starts
    // overlapping a triangle is blocked only when it moves further into it.
    [[nodiscard]] std::optional<SweepHit> SweepSphere(mesh::Vector3 start, mesh::Vector3 delta,
                                                      float radius) const;

    [[nodiscard]] std::size_t TriangleCount() const noexcept { return triangles_.size(); }

  private:
    struct Cell
    {
        std::int32_t x;
        std::int32_t y;
        std::int32_t z;
    };

    [[nodiscard]] static Cell CellOf(mesh::Vector3 point) noexcept;
    [[nodiscard]] static std::uint64_t KeyOf(Cell cell) noexcept;
    void AddMesh(const mesh::StaticMeshLod &lod, const scene::Matrix &world, bool collision_only);

    std::vector<CollisionTriangle> triangles_;
    std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> cells_;
};

// A sphere of `radius` centred at `start` moving by `delta` against one
// triangle: the first contact within the move, if any.
std::optional<SweepHit> SweepSphereTriangle(mesh::Vector3 start, mesh::Vector3 delta, float radius,
                                            const CollisionTriangle &triangle);

} // namespace gears::engine::game
