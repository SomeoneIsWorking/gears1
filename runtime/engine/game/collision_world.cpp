#include "collision_world.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "mesh/vector_math.h"

namespace gears::engine::game
{
namespace
{

using mesh::Cross;
using mesh::Dot;
using mesh::Vector3;

// Squared lengths and determinants below this are degenerate.
constexpr float kEpsilon = 1e-6F;
// Grid coordinates are biased into 21 unsigned bits per axis.
constexpr std::int64_t kCellBias = 1 << 20;
constexpr std::uint64_t kCellMask = (1U << 21U) - 1U;

// Whether `point`, on the triangle's plane, lies inside it.
bool Inside(const CollisionTriangle &triangle, Vector3 point)
{
    const Vector3 &n = triangle.normal;
    return Dot(Cross(triangle.b - triangle.a, point - triangle.a), n) >= 0.0F &&
           Dot(Cross(triangle.c - triangle.b, point - triangle.b), n) >= 0.0F &&
           Dot(Cross(triangle.a - triangle.c, point - triangle.c), n) >= 0.0F;
}

// The earliest t in [0, 1] at which a point moving from `start` by `delta`
// lies `radius` from `center`; none when it never does or starts inside.
std::optional<float> RaySphere(Vector3 start, Vector3 delta, Vector3 center, float radius)
{
    Vector3 m = start - center;
    float a = Dot(delta, delta);
    float b = Dot(m, delta);
    float c = Dot(m, m) - radius * radius;
    if (a < kEpsilon || c < 0.0F || b >= 0.0F)
    {
        return std::nullopt;
    }
    float discriminant = b * b - a * c;
    if (discriminant < 0.0F)
    {
        return std::nullopt;
    }
    float t = (-b - std::sqrt(discriminant)) / a;
    if (t < 0.0F || t > 1.0F)
    {
        return std::nullopt;
    }
    return t;
}

// The earliest t in [0, 1] at which a point moving from `start` by `delta`
// lies `radius` from the segment p..q, between its ends; none when it never
// does there or starts inside the cylinder.
std::optional<float> RayCylinder(Vector3 start, Vector3 delta, Vector3 p, Vector3 q, float radius)
{
    Vector3 d = q - p;
    Vector3 m = start - p;
    float dd = Dot(d, d);
    float md = Dot(m, d);
    float nd = Dot(delta, d);
    float a = dd * Dot(delta, delta) - nd * nd;
    float c = dd * (Dot(m, m) - radius * radius) - md * md;
    if (std::fabs(a) < kEpsilon || c < 0.0F)
    {
        return std::nullopt;
    }
    float b = dd * Dot(m, delta) - nd * md;
    float discriminant = b * b - a * c;
    if (b >= 0.0F || discriminant < 0.0F)
    {
        return std::nullopt;
    }
    float t = (-b - std::sqrt(discriminant)) / a;
    float along = md + t * nd;
    if (t < 0.0F || t > 1.0F || along <= 0.0F || along >= dd)
    {
        return std::nullopt;
    }
    return t;
}

// The closest point to `point` on the segment p..q.
Vector3 ClosestOnSegment(Vector3 point, Vector3 p, Vector3 q)
{
    Vector3 d = q - p;
    float dd = Dot(d, d);
    float t = dd > kEpsilon ? std::clamp(Dot(point - p, d) / dd, 0.0F, 1.0F) : 0.0F;
    return p + d * t;
}

// A hit at `t` whose normal points from `surface` toward the sphere centre.
SweepHit HitAt(Vector3 start, Vector3 delta, float t, Vector3 surface)
{
    Vector3 center = start + delta * t;
    return {t, mesh::NormalizedOrZero(center - surface), surface};
}

} // namespace

std::optional<SweepHit> SweepSphereTriangle(Vector3 start, Vector3 delta, float radius,
                                            const CollisionTriangle &triangle)
{
    // The face: the side the sphere starts on faces it.
    float distance = Dot(start - triangle.a, triangle.normal);
    Vector3 normal = distance >= 0.0F ? triangle.normal : -triangle.normal;
    distance = std::fabs(distance);
    float approach = Dot(delta, normal);
    if (distance <= radius)
    {
        Vector3 touching = start - normal * distance;
        if (approach < 0.0F && Inside(triangle, touching))
        {
            return SweepHit{0.0F, normal, touching};
        }
    }
    else if (approach < 0.0F)
    {
        float t = (distance - radius) / -approach;
        if (t <= 1.0F)
        {
            Vector3 contact = start + delta * t - normal * radius;
            if (Inside(triangle, contact))
            {
                return SweepHit{t, normal, contact};
            }
        }
    }

    // Otherwise the sphere can only meet an edge or a corner.
    std::optional<SweepHit> first;
    auto consider = [&](std::optional<float> t, Vector3 surface)
    {
        if (t && (!first || *t < first->time))
        {
            first = HitAt(start, delta, *t, surface);
        }
    };
    std::array<Vector3, 3> corners{triangle.a, triangle.b, triangle.c};
    for (std::size_t i = 0; i < corners.size(); ++i)
    {
        Vector3 p = corners[i];
        Vector3 q = corners[(i + 1U) % corners.size()];
        std::optional<float> t = RayCylinder(start, delta, p, q, radius);
        if (t)
        {
            consider(t, ClosestOnSegment(start + delta * *t, p, q));
        }
        consider(RaySphere(start, delta, p, radius), p);
    }
    return first;
}

CollisionWorld CollisionWorld::Build(const scene::World &world, scene::StaticMeshes &meshes)
{
    CollisionWorld collision;
    for (const scene::WorldLevel &level : world.Levels())
    {
        for (const scene::ModelInstance &model : level.scene.Models())
        {
            collision.AddMesh(model.geometry.lod, scene::Matrix::Identity(), false);
        }
        for (const scene::MeshInstance &instance : level.scene.Instances())
        {
            const mesh::StaticMesh &decoded = meshes.Get(instance.mesh);
            if (!decoded.Lods().empty())
            {
                collision.AddMesh(decoded.Lods()[0], instance.world, true);
            }
        }
    }
    return collision;
}

void CollisionWorld::AddMesh(const mesh::StaticMeshLod &lod, const scene::Matrix &world,
                             bool collision_only)
{
    for (const mesh::MeshSection &section : lod.sections)
    {
        if (collision_only && !section.collision)
        {
            continue;
        }
        // Mesh and BSP readers have placed every section inside the index list.
        std::size_t end =
            std::size_t{section.first_index} + std::size_t{section.triangle_count} * 3U;
        for (std::size_t i = section.first_index; i < end; i += 3U)
        {
            Add(world.TransformPoint(lod.vertices[lod.indices[i]].position),
                world.TransformPoint(lod.vertices[lod.indices[i + 1U]].position),
                world.TransformPoint(lod.vertices[lod.indices[i + 2U]].position));
        }
    }
}

void CollisionWorld::Add(Vector3 a, Vector3 b, Vector3 c)
{
    Vector3 normal = Cross(b - a, c - a);
    if (Dot(normal, normal) < kEpsilon)
    {
        return;
    }
    auto index = static_cast<std::uint32_t>(triangles_.size());
    triangles_.push_back({a, b, c, mesh::NormalizedOrZero(normal)});
    Vector3 low{std::min({a.x, b.x, c.x}), std::min({a.y, b.y, c.y}), std::min({a.z, b.z, c.z})};
    Vector3 high{std::max({a.x, b.x, c.x}), std::max({a.y, b.y, c.y}), std::max({a.z, b.z, c.z})};
    Cell first = CellOf(low);
    Cell last = CellOf(high);
    for (std::int32_t x = first.x; x <= last.x; ++x)
    {
        for (std::int32_t y = first.y; y <= last.y; ++y)
        {
            for (std::int32_t z = first.z; z <= last.z; ++z)
            {
                cells_[KeyOf({x, y, z})].push_back(index);
            }
        }
    }
}

std::optional<SweepHit> CollisionWorld::SweepSphere(Vector3 start, Vector3 delta,
                                                    float radius) const
{
    Vector3 end = start + delta;
    Vector3 low{std::min(start.x, end.x) - radius, std::min(start.y, end.y) - radius,
                std::min(start.z, end.z) - radius};
    Vector3 high{std::max(start.x, end.x) + radius, std::max(start.y, end.y) + radius,
                 std::max(start.z, end.z) + radius};
    Cell first = CellOf(low);
    Cell last = CellOf(high);
    std::vector<std::uint32_t> candidates;
    for (std::int32_t x = first.x; x <= last.x; ++x)
    {
        for (std::int32_t y = first.y; y <= last.y; ++y)
        {
            for (std::int32_t z = first.z; z <= last.z; ++z)
            {
                auto cell = cells_.find(KeyOf({x, y, z}));
                if (cell != cells_.end())
                {
                    candidates.insert(candidates.end(), cell->second.begin(), cell->second.end());
                }
            }
        }
    }
    std::ranges::sort(candidates);
    auto duplicates = std::ranges::unique(candidates);
    candidates.erase(duplicates.begin(), duplicates.end());

    std::optional<SweepHit> nearest;
    for (std::uint32_t index : candidates)
    {
        std::optional<SweepHit> hit = SweepSphereTriangle(start, delta, radius, triangles_[index]);
        if (hit && (!nearest || hit->time < nearest->time))
        {
            nearest = hit;
        }
    }
    return nearest;
}

CollisionWorld::Cell CollisionWorld::CellOf(Vector3 point) noexcept
{
    auto axis = [](float value)
    { return static_cast<std::int32_t>(std::floor(value / kCellSize)); };
    return {axis(point.x), axis(point.y), axis(point.z)};
}

std::uint64_t CollisionWorld::KeyOf(Cell cell) noexcept
{
    auto biased = [](std::int32_t value)
    { return static_cast<std::uint64_t>(std::int64_t{value} + kCellBias) & kCellMask; };
    return (biased(cell.x) << 42U) | (biased(cell.y) << 21U) | biased(cell.z);
}

} // namespace gears::engine::game
