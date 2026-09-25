#include "transform.h"

#include <cmath>
#include <numbers>

namespace gears::engine::scene
{
namespace
{

constexpr float kAngleUnitsPerTurn = 65536.0F;

float Radians(std::int32_t units)
{
    return static_cast<float>(units) * (2.0F * std::numbers::pi_v<float> / kAngleUnitsPerTurn);
}

} // namespace

Matrix Matrix::Identity()
{
    Matrix identity;
    for (std::size_t i = 0; i < 4U; ++i)
    {
        identity.m[i][i] = 1.0F;
    }
    return identity;
}

Matrix Matrix::operator*(const Matrix &right) const
{
    Matrix product;
    for (std::size_t r = 0; r < 4U; ++r)
    {
        for (std::size_t c = 0; c < 4U; ++c)
        {
            float sum = 0.0F;
            for (std::size_t k = 0; k < 4U; ++k)
            {
                sum += m[r][k] * right.m[k][c];
            }
            product.m[r][c] = sum;
        }
    }
    return product;
}

mesh::Vector3 Matrix::TransformPoint(const mesh::Vector3 &p) const
{
    return {p.x * m[0][0] + p.y * m[1][0] + p.z * m[2][0] + m[3][0],
            p.x * m[0][1] + p.y * m[1][1] + p.z * m[2][1] + m[3][1],
            p.x * m[0][2] + p.y * m[1][2] + p.z * m[2][2] + m[3][2]};
}

Matrix ActorPlacement(const mesh::Vector3 &location, const Rotator &rotation,
                      const mesh::Vector3 &scale)
{
    float sp = std::sin(Radians(rotation.pitch));
    float cp = std::cos(Radians(rotation.pitch));
    float sy = std::sin(Radians(rotation.yaw));
    float cy = std::cos(Radians(rotation.yaw));
    float sr = std::sin(Radians(rotation.roll));
    float cr = std::cos(Radians(rotation.roll));
    Matrix rotate = Matrix::Identity();
    rotate.m[0] = {cp * cy, cp * sy, sp, 0.0F};
    rotate.m[1] = {sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp, 0.0F};
    rotate.m[2] = {-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp, 0.0F};
    Matrix scaled = Matrix::Identity();
    scaled.m[0][0] = scale.x;
    scaled.m[1][1] = scale.y;
    scaled.m[2][2] = scale.z;
    Matrix placement = scaled * rotate;
    placement.m[3] = {location.x, location.y, location.z, 1.0F};
    return placement;
}

} // namespace gears::engine::scene
