#pragma once

#include <cmath>

#include "static_mesh.h"

namespace gears::engine::mesh
{

// Component-wise arithmetic and products of engine vectors.

[[nodiscard]] inline Vector3 operator+(Vector3 a, Vector3 b) noexcept
{
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

[[nodiscard]] inline Vector3 operator-(Vector3 a, Vector3 b) noexcept
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

[[nodiscard]] inline Vector3 operator-(Vector3 v) noexcept
{
    return {-v.x, -v.y, -v.z};
}

[[nodiscard]] inline Vector3 operator*(Vector3 v, float s) noexcept
{
    return {v.x * s, v.y * s, v.z * s};
}

[[nodiscard]] inline Vector3 operator*(float s, Vector3 v) noexcept
{
    return v * s;
}

inline Vector3 &operator+=(Vector3 &a, Vector3 b) noexcept
{
    a = a + b;
    return a;
}

[[nodiscard]] inline float Dot(Vector3 a, Vector3 b) noexcept
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

[[nodiscard]] inline Vector3 Cross(Vector3 a, Vector3 b) noexcept
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

[[nodiscard]] inline float Length(Vector3 v) noexcept
{
    return std::sqrt(Dot(v, v));
}

// `v` scaled to unit length, or zero for a vector with no length.
[[nodiscard]] inline Vector3 NormalizedOrZero(Vector3 v) noexcept
{
    float length = Length(v);
    return length > 0.0F ? v * (1.0F / length) : Vector3{};
}

} // namespace gears::engine::mesh
