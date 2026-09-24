#include "navigation_probe.h"

#include <bit>
#include <format>
#include <utility>

namespace gears::titles::gears1
{
namespace
{

[[nodiscard]] bool ReadPaths(ChainReader &chain, NavigationPoint &point)
{
    std::uint32_t specs = 0;
    std::uint32_t count = 0;
    if (!chain.Word(point.address + kNavigationPathsOffset, "a point's paths", specs) ||
        !chain.Word(point.address + kNavigationPathsOffset + 4U, "a point's path count", count))
    {
        return false;
    }
    if (count > kMaxNavigationPaths)
    {
        return chain.Fail(std::format("point 0x{:08X} lists {} paths, past {}", point.address,
                                      count, kMaxNavigationPaths));
    }
    point.paths.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index)
    {
        std::uint32_t spec = 0;
        std::uint32_t start = 0;
        std::uint32_t distance = 0;
        NavigationPath path;
        if (!chain.Object(specs + index * 4U, "a point's path", spec) ||
            !chain.Word(spec, "a path's class", path.vtable) ||
            !chain.Word(spec + kReachSpecStartOffset, "a path's start", start) ||
            !chain.Word(spec + kReachSpecEndOffset, "a path's end", path.end) ||
            !chain.Word(spec + kReachSpecDistanceOffset, "a path's distance", distance))
        {
            return false;
        }
        if (start != point.address)
        {
            return chain.Fail(std::format("path 0x{:08X} of point 0x{:08X} starts at 0x{:08X}",
                                          spec, point.address, start));
        }
        path.distance = std::bit_cast<std::int32_t>(distance);
        path.kind = PathKindOf(path.vtable);
        point.paths.push_back(path);
    }
    return true;
}

} // namespace

PathKind PathKindOf(std::uint32_t vtable)
{
    switch (vtable)
    {
    case kWalkReachSpecVtable:
        return PathKind::Walk;
    case kMantleReachSpecVtable:
        return PathKind::Mantle;
    default:
        return PathKind::Other;
    }
}

PointKind PointKindOf(std::uint32_t vtable)
{
    switch (vtable)
    {
    case kPathNodeVtable:
        return PointKind::PathNode;
    case kCoverSlotVtable:
        return PointKind::Cover;
    default:
        return PointKind::Other;
    }
}

bool ReadNavigation(const GuestMemoryReader &read, std::vector<NavigationPoint> &points,
                    std::string &error)
{
    points.clear();
    ChainReader chain(read, error);
    std::uint32_t controller = 0;
    std::uint32_t world_info = 0;
    std::uint32_t address = 0;
    if (!chain.LocalController(controller) ||
        !chain.Object(controller + kActorWorldInfoOffset, "the controller's world info",
                      world_info) ||
        !chain.Word(world_info + kWorldInfoNavigationListOffset, "the world's navigation list",
                    address))
    {
        return false;
    }
    while (address != 0)
    {
        if (points.size() == kMaxNavigationPoints)
        {
            return chain.Fail(std::format("the world's navigation list runs past {} points",
                                          kMaxNavigationPoints));
        }
        NavigationPoint point;
        point.address = address;
        if (!chain.Word(address, "a point's class", point.vtable) ||
            !chain.Location(address, "a point's location", point.location) ||
            !chain.Angle(address, 1U, "a point's yaw", point.yaw) || !ReadPaths(chain, point) ||
            !chain.Word(address + kNavigationNextOffset, "a point's successor", address))
        {
            return false;
        }
        point.kind = PointKindOf(point.vtable);
        points.push_back(std::move(point));
    }
    return true;
}

} // namespace gears::titles::gears1
