// Game simulation of the native engine: swept-sphere collision against
// level triangles and a walking capsule on floors, into walls, and up steps.

#include <cassert>
#include <cmath>
#include <optional>

#include "game/collision_world.h"
#include "game/pawn_tuning.h"
#include "game/walking_movement.h"

namespace
{

using gears::engine::game::CollisionTriangle;
using gears::engine::game::CollisionWorld;
using gears::engine::game::PawnTuning;
using gears::engine::game::SweepSphereTriangle;
using gears::engine::game::WalkingMovement;
using gears::engine::game::WalkingState;
using gears::engine::mesh::Vector3;

constexpr float kTolerance = 1e-3F;
constexpr float kFloorExtent = 5000.0F;
constexpr float kTickSeconds = 1.0F / 60.0F;

bool Near(float a, float b, float tolerance = kTolerance)
{
    return std::fabs(a - b) < tolerance;
}

// A square of two triangles at height `z` spanning x0..x1 and y0..y1.
void AddFloor(CollisionWorld &world, float x0, float x1, float y0, float y1, float z)
{
    world.Add({x0, y0, z}, {x1, y0, z}, {x1, y1, z});
    world.Add({x0, y0, z}, {x1, y1, z}, {x0, y1, z});
}

// A wall in the plane x = `x`, from y0..y1 and z0..z1.
void AddWall(CollisionWorld &world, float x, float y0, float y1, float z0, float z1)
{
    world.Add({x, y0, z0}, {x, y1, z0}, {x, y1, z1});
    world.Add({x, y0, z0}, {x, y1, z1}, {x, y0, z1});
}

void TestSphereMeetsFace()
{
    CollisionTriangle floor{{-100.0F, -100.0F, 0.0F},
                            {100.0F, -100.0F, 0.0F},
                            {0.0F, 100.0F, 0.0F},
                            {0.0F, 0.0F, 1.0F}};
    auto hit = SweepSphereTriangle({0.0F, 0.0F, 50.0F}, {0.0F, 0.0F, -100.0F}, 10.0F, floor);
    assert(hit && Near(hit->time, 0.4F) && Near(hit->normal.z, 1.0F));
    // From below, the other face blocks and faces down.
    auto under = SweepSphereTriangle({0.0F, 0.0F, -50.0F}, {0.0F, 0.0F, 100.0F}, 10.0F, floor);
    assert(under && Near(under->time, 0.4F) && Near(under->normal.z, -1.0F));
    // Moving away or passing beside misses.
    assert(!SweepSphereTriangle({0.0F, 0.0F, 50.0F}, {0.0F, 0.0F, 100.0F}, 10.0F, floor));
    assert(!SweepSphereTriangle({500.0F, 0.0F, 50.0F}, {0.0F, 0.0F, -100.0F}, 10.0F, floor));
}

void TestSphereMeetsEdge()
{
    CollisionTriangle floor{
        {0.0F, -100.0F, 0.0F}, {100.0F, -100.0F, 0.0F}, {0.0F, 100.0F, 0.0F}, {0.0F, 0.0F, 1.0F}};
    // Falling just outside the edge x = 0, the sphere catches the edge.
    auto hit = SweepSphereTriangle({-6.0F, 0.0F, 50.0F}, {0.0F, 0.0F, -100.0F}, 10.0F, floor);
    assert(hit && hit->time > 0.4F && hit->time < 0.5F);
    assert(hit->normal.x < 0.0F && hit->normal.z > 0.0F);
}

void TestWorldGridFindsTriangles()
{
    CollisionWorld world;
    AddFloor(world, -kFloorExtent, kFloorExtent, -kFloorExtent, kFloorExtent, 0.0F);
    assert(world.TriangleCount() == 2U);
    auto hit = world.SweepSphere({3000.0F, -2000.0F, 100.0F}, {0.0F, 0.0F, -200.0F}, 20.0F);
    assert(hit && Near(hit->time, 0.4F));
    assert(!world.SweepSphere({0.0F, 0.0F, 100.0F}, {300.0F, 0.0F, 0.0F}, 20.0F));
}

void TestLandsAndWalks()
{
    CollisionWorld world;
    AddFloor(world, -kFloorExtent, kFloorExtent, -kFloorExtent, kFloorExtent, 0.0F);
    PawnTuning tuning;
    WalkingMovement movement(world, tuning);
    WalkingState state;
    state.location = {0.0F, 0.0F, 200.0F};
    for (int tick = 0; tick < 120; ++tick)
    {
        movement.Tick(state, {}, kTickSeconds);
    }
    assert(state.on_ground);
    assert(Near(state.location.z, tuning.half_height, 1.0F));
    for (int tick = 0; tick < 60; ++tick)
    {
        movement.Tick(state, {tuning.run_speed, 0.0F, 0.0F}, kTickSeconds);
    }
    assert(state.on_ground && state.location.x > tuning.run_speed * 0.5F);
    assert(Near(state.location.z, tuning.half_height, 1.0F));
}

void TestWallBlocksAndSlides()
{
    CollisionWorld world;
    AddFloor(world, -kFloorExtent, kFloorExtent, -kFloorExtent, kFloorExtent, 0.0F);
    AddWall(world, 200.0F, -kFloorExtent, kFloorExtent, 0.0F, 400.0F);
    PawnTuning tuning;
    WalkingMovement movement(world, tuning);
    WalkingState state;
    state.location = {0.0F, 0.0F, tuning.half_height + 0.5F};
    state.on_ground = true;
    for (int tick = 0; tick < 120; ++tick)
    {
        movement.Tick(state, {tuning.run_speed, tuning.run_speed, 0.0F}, kTickSeconds);
    }
    // Stopped short of the wall, still sliding along it.
    assert(state.location.x < 200.0F - tuning.radius + 1.0F);
    assert(state.location.y > tuning.run_speed * 0.5F);
}

void TestClimbsStep()
{
    CollisionWorld world;
    AddFloor(world, -kFloorExtent, kFloorExtent, -kFloorExtent, kFloorExtent, 0.0F);
    // A 20-unit step up starting at x = 150.
    AddWall(world, 150.0F, -kFloorExtent, kFloorExtent, 0.0F, 20.0F);
    AddFloor(world, 150.0F, kFloorExtent, -kFloorExtent, kFloorExtent, 20.0F);
    PawnTuning tuning;
    WalkingMovement movement(world, tuning);
    WalkingState state;
    state.location = {0.0F, 0.0F, tuning.half_height + 0.5F};
    state.on_ground = true;
    for (int tick = 0; tick < 90; ++tick)
    {
        movement.Tick(state, {tuning.run_speed, 0.0F, 0.0F}, kTickSeconds);
    }
    assert(state.location.x > 300.0F);
    assert(Near(state.location.z, tuning.half_height + 20.0F, 1.5F));
}

} // namespace

int main()
{
    TestSphereMeetsFace();
    TestSphereMeetsEdge();
    TestWorldGridFindsTriangles();
    TestLandsAndWalks();
    TestWallBlocksAndSlides();
    TestClimbsStep();
    return 0;
}
