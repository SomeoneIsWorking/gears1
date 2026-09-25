#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "mesh/static_mesh.h"
#include "object/serialized_object.h"

namespace gears::engine::bsp
{

// One convex BSP polygon: its plane, the run of vertex-pool entries that
// outline it, and the surface that gives it a material and texture axes.
struct BspNode
{
    float plane_x = 0.0F;
    float plane_y = 0.0F;
    float plane_z = 0.0F;
    float plane_w = 0.0F;
    std::int32_t vertex_pool = 0;
    std::int32_t surface = 0;
    std::uint8_t vertex_count = 0;
};

// A surface shared by coplanar nodes: its material (a reference of the
// model's package) and the point and vectors that place its texture.
struct BspSurface
{
    object::PackageIndex material = 0;
    std::uint32_t poly_flags = 0;
    std::int32_t texture_base = 0;
    std::int32_t normal = 0;
    std::int32_t texture_u = 0;
    std::int32_t texture_v = 0;
};

// One vertex-pool entry: the point it stands on and its light-map (shadow)
// texture coordinate before the element's scale and bias.
struct BspVertex
{
    std::int32_t point = 0;
    std::array<float, 2> shadow_uv{};
};

// The render-relevant part of a Model export: shared vectors and points,
// nodes, surfaces, and the vertex pool whose entries index the points.
// Everything after the vertex pool (zones, leaves, collision hulls) is not
// read.
class BspModel
{
  public:
    // Refuses a node whose surface or vertex run, or a vertex whose point,
    // lies outside its table.
    static BspModel Read(const object::SerializedObject &object);

    BspModel(std::vector<mesh::Vector3> vectors, std::vector<mesh::Vector3> points,
             std::vector<BspNode> nodes, std::vector<BspSurface> surfaces,
             std::vector<BspVertex> vertices);

    [[nodiscard]] const std::vector<mesh::Vector3> &Vectors() const noexcept { return vectors_; }
    [[nodiscard]] const std::vector<mesh::Vector3> &Points() const noexcept { return points_; }
    [[nodiscard]] const std::vector<BspNode> &Nodes() const noexcept { return nodes_; }
    [[nodiscard]] const std::vector<BspSurface> &Surfaces() const noexcept { return surfaces_; }
    [[nodiscard]] const std::vector<BspVertex> &Vertices() const noexcept { return vertices_; }

  private:
    // Refuses tables that reference outside one another.
    void Validate() const;

    std::vector<mesh::Vector3> vectors_;
    std::vector<mesh::Vector3> points_;
    std::vector<BspNode> nodes_;
    std::vector<BspSurface> surfaces_;
    std::vector<BspVertex> vertices_;
};

} // namespace gears::engine::bsp
