#pragma once

#include <array>
#include <cstdint>

#include "mesh/static_mesh.h"

namespace gears::engine::scene
{

// A 4x4 matrix in the engine's row-vector convention: a point p maps to
// p * M, and the translation is the last row.
struct Matrix
{
    std::array<std::array<float, 4>, 4> m{};

    static Matrix Identity();
    [[nodiscard]] Matrix operator*(const Matrix &right) const;
    [[nodiscard]] mesh::Vector3 TransformPoint(const mesh::Vector3 &p) const;
};

// Rotation in the title's 16-bit angle units (65536 per turn).
struct Rotator
{
    std::int32_t pitch = 0;
    std::int32_t yaw = 0;
    std::int32_t roll = 0;
};

// Scale, then rotate (roll about X, pitch about Y, yaw about Z), then
// translate: the placement of an actor in its level.
Matrix ActorPlacement(const mesh::Vector3 &location, const Rotator &rotation,
                      const mesh::Vector3 &scale);

} // namespace gears::engine::scene
