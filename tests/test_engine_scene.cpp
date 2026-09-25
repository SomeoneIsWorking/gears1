// Scene math of the native engine: actor placement in the title's frame and
// the camera's mapping into Vulkan clip space.

#include <cassert>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

#include "scene/camera.h"
#include "scene/transform.h"

namespace
{

using gears::engine::mesh::Vector3;
using gears::engine::scene::ActorPlacement;
using gears::engine::scene::Camera;
using gears::engine::scene::Matrix;
using gears::engine::scene::Rotator;

constexpr float kTolerance = 1e-4F;
constexpr std::int32_t kQuarterTurn = 16384;

bool Near(float a, float b)
{
    return std::fabs(a - b) < kTolerance;
}

bool Near(const Vector3 &a, const Vector3 &b)
{
    return Near(a.x, b.x) && Near(a.y, b.y) && Near(a.z, b.z);
}

struct Clip
{
    float x;
    float y;
    float depth;
};

Clip Project(const Matrix &view_projection, const Vector3 &p)
{
    std::array<float, 4> clip{};
    std::array<float, 4> point{p.x, p.y, p.z, 1.0F};
    for (std::size_t c = 0; c < 4U; ++c)
    {
        for (std::size_t r = 0; r < 4U; ++r)
        {
            clip[c] += point[r] * view_projection.m[r][c];
        }
    }
    return {clip[0] / clip[3], clip[1] / clip[3], clip[2] / clip[3]};
}

template <typename Action> bool RefusesArgument(Action action)
{
    try
    {
        action();
    }
    catch (const std::invalid_argument &)
    {
        return true;
    }
    return false;
}

void TestPlacement()
{
    // Positive yaw turns +X toward +Y; positive pitch raises +X toward +Z.
    Matrix yaw = ActorPlacement({}, Rotator{0, kQuarterTurn, 0}, {1.0F, 1.0F, 1.0F});
    assert(Near(yaw.TransformPoint({1.0F, 0.0F, 0.0F}), {0.0F, 1.0F, 0.0F}));
    Matrix pitch = ActorPlacement({}, Rotator{kQuarterTurn, 0, 0}, {1.0F, 1.0F, 1.0F});
    assert(Near(pitch.TransformPoint({1.0F, 0.0F, 0.0F}), {0.0F, 0.0F, 1.0F}));
    // Scale applies before rotation, translation last.
    Matrix placed =
        ActorPlacement({10.0F, 20.0F, 30.0F}, Rotator{0, kQuarterTurn, 0}, {2.0F, 3.0F, 4.0F});
    assert(Near(placed.TransformPoint({1.0F, 0.0F, 0.0F}), {10.0F, 22.0F, 30.0F}));
    assert(Near(placed.TransformPoint({0.0F, 1.0F, 0.0F}), {7.0F, 20.0F, 30.0F}));
}

void TestCamera()
{
    Camera camera;
    camera.eye = {0.0F, 0.0F, 0.0F};
    camera.target = {100.0F, 0.0F, 0.0F};
    camera.aspect = 1.0F;
    camera.near_plane = 10.0F;
    camera.far_plane = 1000.0F;
    Matrix view_projection = camera.ViewProjection();

    Clip center = Project(view_projection, {100.0F, 0.0F, 0.0F});
    assert(Near(center.x, 0.0F) && Near(center.y, 0.0F));
    assert(Near(Project(view_projection, {10.0F, 0.0F, 0.0F}).depth, 0.0F));
    assert(Near(Project(view_projection, {1000.0F, 0.0F, 0.0F}).depth, 1.0F));
    // The title's +Y is to the right of a +X view; +Z is up, which is -Y in
    // Vulkan clip space.
    assert(Project(view_projection, {100.0F, 10.0F, 0.0F}).x > 0.0F);
    assert(Project(view_projection, {100.0F, 0.0F, 10.0F}).y < 0.0F);

    Camera degenerate = camera;
    degenerate.target = degenerate.eye;
    assert(RefusesArgument([&] { (void)degenerate.ViewProjection(); }));
    Camera straight_down = camera;
    straight_down.target = {0.0F, 0.0F, -100.0F};
    assert(RefusesArgument([&] { (void)straight_down.ViewProjection(); }));
}

} // namespace

int main()
{
    TestPlacement();
    TestCamera();
    return 0;
}
