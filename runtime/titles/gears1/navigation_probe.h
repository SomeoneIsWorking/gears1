#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "guest_chain.h"

namespace gears::titles::gears1
{

// The level's navigation graph as the engine's path finder holds it, measured
// in the running title (docs/re-frontier.md, "Navigation graph").
// WorldInfo: the first navigation point; each point links to the next.
inline constexpr std::uint32_t kWorldInfoNavigationListOffset = 0x320U;
inline constexpr std::uint32_t kNavigationNextOffset = 0x208U;
// NavigationPoint: its outgoing paths, a TArray of ReachSpec pointers.
inline constexpr std::uint32_t kNavigationPathsOffset = 0x1DCU;
// ReachSpec: the path's length in world units and the points it joins.
inline constexpr std::uint32_t kReachSpecDistanceOffset = 0x40U;
inline constexpr std::uint32_t kReachSpecStartOffset = 0x44U;
inline constexpr std::uint32_t kReachSpecEndOffset = 0x48U;
// ReachSpec classes by vtable: 2772 of sp_prison_p's 2940 paths are plain
// walks; a mantle joins the cover slots either side of low cover, crossed by
// taking cover and pressing A toward the far side. The other classes (104 and
// 12 paths) start at cover slots and are not yet understood.
inline constexpr std::uint32_t kWalkReachSpecVtable = 0x820DF328U;
inline constexpr std::uint32_t kMantleReachSpecVtable = 0x820DF898U;
// NavigationPoint classes by vtable: plain path nodes, and the slots a pawn
// takes cover at (432 of sp_prison_p's first 1150 points).
inline constexpr std::uint32_t kPathNodeVtable = 0x820952E8U;
inline constexpr std::uint32_t kCoverSlotVtable = 0x820955C0U;
// Bounds past which a list is corrupt or cyclic, not a level.
inline constexpr std::size_t kMaxNavigationPoints = 8192U;
inline constexpr std::uint32_t kMaxNavigationPaths = 64U;

// How a walker covers a path.
enum class PathKind : std::uint8_t
{
    Walk,
    Mantle,
    Other,
};

// One path the engine's walkers may take from a point.
struct NavigationPath
{
    // The point the path ends at, by guest address; 0 for a path whose end is
    // not loaded, as one on sp_prison_p was after the yard's first firefight.
    std::uint32_t end = 0;
    std::int32_t distance = 0;
    PathKind kind = PathKind::Other;
    // The ReachSpec's vtable, naming its class when kind is Other.
    std::uint32_t vtable = 0;
};

// The kind of path a ReachSpec vtable names.
[[nodiscard]] PathKind PathKindOf(std::uint32_t vtable);

// What a navigation point is to a walker.
enum class PointKind : std::uint8_t
{
    PathNode,
    Cover,
    Other,
};

// The kind of point a NavigationPoint vtable names.
[[nodiscard]] PointKind PointKindOf(std::uint32_t vtable);

// One navigation point: path nodes, cover slots, pickups, and doors alike.
struct NavigationPoint
{
    // The point object's guest address, its identity for the level's life.
    std::uint32_t address = 0;
    PointKind kind = PointKind::Other;
    // The point's vtable, naming its class when kind is Other.
    std::uint32_t vtable = 0;
    std::array<float, 3> location{};
    // The point's yaw in engine units (65536 a turn); a cover slot's is the
    // direction its cover faces: each of sp_prison_p's 140 slots beside a
    // mantle faced within 16 degrees of the far side (docs/re-frontier.md).
    std::uint16_t yaw = 0;
    std::vector<NavigationPath> paths;
};

// Reads the local player's level's navigation points in the engine's list
// order. Refuses, naming the step, before gameplay, for an unreadable step, a
// path that does not start at the point listing it, or a list or path array
// past its bound.
[[nodiscard]] bool ReadNavigation(const GuestMemoryReader &read,
                                  std::vector<NavigationPoint> &points, std::string &error);

} // namespace gears::titles::gears1
