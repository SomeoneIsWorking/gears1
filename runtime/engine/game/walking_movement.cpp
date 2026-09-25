#include "walking_movement.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "mesh/vector_math.h"

namespace gears::engine::game
{
namespace
{

using mesh::Dot;
using mesh::Vector3;

// Slides tried per move before the rest of the move is dropped.
constexpr int kMaxSlides = 4;
// Distance kept from surfaces so the next sweep does not start touching.
constexpr float kSkin = 0.5F;
// How far below the capsule a floor still holds it while walking.
constexpr float kFloorProbe = 2.0F;

Vector3 Horizontal(Vector3 v)
{
    return {v.x, v.y, 0.0F};
}

// `v` with its component into `normal` removed.
Vector3 AlongPlane(Vector3 v, Vector3 normal)
{
    float into = Dot(v, normal);
    return into < 0.0F ? v - normal * into : v;
}

// Moves `current` toward `target` by at most `step`.
Vector3 Approach(Vector3 current, Vector3 target, float step)
{
    Vector3 difference = target - current;
    float distance = mesh::Length(difference);
    return distance <= step ? target : current + difference * (step / distance);
}

} // namespace

std::optional<SweepHit> WalkingMovement::SweepCapsule(Vector3 location, Vector3 delta) const
{
    float offset = std::max(tuning_.half_height - tuning_.radius, 0.0F);
    std::array<float, 3> heights{-offset, 0.0F, offset};
    std::optional<SweepHit> first;
    for (float height : heights)
    {
        Vector3 center{location.x, location.y, location.z + height};
        std::optional<SweepHit> hit = collision_.SweepSphere(center, delta, tuning_.radius);
        if (hit && (!first || hit->time < first->time))
        {
            first = hit;
        }
    }
    return first;
}

void WalkingMovement::Tick(WalkingState &state, Vector3 wish_velocity, float seconds) const
{
    Vector3 horizontal = Approach(Horizontal(state.velocity), Horizontal(wish_velocity),
                                  tuning_.acceleration * seconds);
    float vertical = state.on_ground ? 0.0F : state.velocity.z + tuning_.gravity * seconds;
    state.velocity = {horizontal.x, horizontal.y, vertical};

    Vector3 horizontal_move = horizontal * seconds;
    bool blocked_by_wall = SlideMove(state, horizontal_move);
    if (blocked_by_wall && state.on_ground)
    {
        StepUp(state, horizontal_move);
    }
    if (!state.on_ground)
    {
        (void)SlideMove(state, {0.0F, 0.0F, state.velocity.z * seconds});
    }
    FindFloor(state);
}

bool WalkingMovement::SlideMove(WalkingState &state, Vector3 delta) const
{
    bool blocked_by_wall = false;
    for (int slide = 0; slide < kMaxSlides && Dot(delta, delta) > 1e-6F; ++slide)
    {
        std::optional<SweepHit> hit = SweepCapsule(state.location, delta);
        if (!hit)
        {
            state.location += delta;
            return blocked_by_wall;
        }
        float length = mesh::Length(delta);
        float travelled = std::max(hit->time * length - kSkin, 0.0F);
        state.location += delta * (travelled / length);
        if (hit->normal.z >= tuning_.walkable_floor_z)
        {
            state.on_ground = true;
            state.velocity.z = std::max(state.velocity.z, 0.0F);
        }
        else
        {
            blocked_by_wall = true;
        }
        state.velocity = AlongPlane(state.velocity, hit->normal);
        delta = AlongPlane(delta * (1.0F - travelled / length), hit->normal);
    }
    return blocked_by_wall;
}

void WalkingMovement::StepUp(WalkingState &state, Vector3 horizontal) const
{
    WalkingState stepped = state;
    Vector3 up{0.0F, 0.0F, tuning_.step_height};
    std::optional<SweepHit> ceiling = SweepCapsule(stepped.location, up);
    float rise =
        ceiling ? std::max(ceiling->time * tuning_.step_height - kSkin, 0.0F) : tuning_.step_height;
    stepped.location.z += rise;
    (void)SlideMove(stepped, horizontal);
    std::optional<SweepHit> floor = SweepCapsule(stepped.location, {0.0F, 0.0F, -rise});
    if (!floor || floor->normal.z < tuning_.walkable_floor_z)
    {
        return;
    }
    stepped.location.z -= std::max(floor->time * rise - kSkin, 0.0F);
    Vector3 before = Horizontal(state.location);
    if (mesh::Length(Horizontal(stepped.location) - before) >
        mesh::Length(Horizontal(state.location) - before) + kSkin)
    {
        state = stepped;
    }
}

void WalkingMovement::FindFloor(WalkingState &state) const
{
    // Walking pawns follow floors down steps; falling ones only land.
    float probe = state.on_ground ? tuning_.step_height + kFloorProbe : kFloorProbe;
    std::optional<SweepHit> floor = SweepCapsule(state.location, {0.0F, 0.0F, -probe});
    if (floor && floor->normal.z >= tuning_.walkable_floor_z && state.velocity.z <= 0.0F)
    {
        state.location.z -= std::max(floor->time * probe - kSkin, 0.0F);
        state.on_ground = true;
        state.velocity.z = 0.0F;
        return;
    }
    state.on_ground = false;
}

} // namespace gears::engine::game
