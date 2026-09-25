#include "camera.h"

#include <cmath>
#include <stdexcept>

#include "mesh/vector_math.h"

namespace gears::engine::scene
{
namespace
{

using mesh::Cross;
using mesh::Dot;

mesh::Vector3 Normalized(mesh::Vector3 v, const char *what)
{
    float length = mesh::Length(v);
    if (!(length > 0.0F))
    {
        throw std::invalid_argument(what);
    }
    return {v.x / length, v.y / length, v.z / length};
}

} // namespace

Matrix Camera::ViewProjection() const
{
    mesh::Vector3 forward = Normalized(target - eye, "camera eye equals its target");
    // The engine's basis is left-handed: up x forward points right.
    mesh::Vector3 right =
        Normalized(Cross({0.0F, 0.0F, 1.0F}, forward), "camera looks straight along the up axis");
    mesh::Vector3 up = Cross(forward, right);

    // View space: X right, Y down (Vulkan's clip Y), Z forward.
    Matrix view = Matrix::Identity();
    view.m[0] = {right.x, -up.x, forward.x, 0.0F};
    view.m[1] = {right.y, -up.y, forward.y, 0.0F};
    view.m[2] = {right.z, -up.z, forward.z, 0.0F};
    view.m[3] = {-Dot(eye, right), Dot(eye, up), -Dot(eye, forward), 1.0F};

    float focal = 1.0F / std::tan(vertical_fov_radians * 0.5F);
    float depth_scale = far_plane / (far_plane - near_plane);
    Matrix projection;
    projection.m[0][0] = focal / aspect;
    projection.m[1][1] = focal;
    projection.m[2][2] = depth_scale;
    projection.m[2][3] = 1.0F;
    projection.m[3][2] = -near_plane * depth_scale;
    return view * projection;
}

} // namespace gears::engine::scene
