#pragma once

#include "transform.h"

namespace gears::engine::scene
{

// A perspective view in the engine's frame (X forward, Y right, Z up) that
// maps to Vulkan clip space: X right, Y down, depth 0 at `near` to 1 at `far`.
struct Camera
{
    mesh::Vector3 eye;
    mesh::Vector3 target;
    float vertical_fov_radians = 1.2F;
    float aspect = 16.0F / 9.0F;
    float near_plane = 10.0F;
    float far_plane = 200000.0F;

    // The row-vector view-projection product: a point p reaches clip space as
    // p * ViewProjection(). Refuses an eye that coincides with its target or
    // looks straight along Z.
    [[nodiscard]] Matrix ViewProjection() const;
};

} // namespace gears::engine::scene
