#include "navigation_probe.h"

#include "fake_guest_memory.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace
{

using namespace gears::titles::gears1;
using FakeGuest = gears::tests::FakeGuestMemory;

void Require(bool condition, std::string_view message)
{
    if (!condition)
    {
        std::cerr << "navigation probe: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

constexpr std::uint32_t kEngine = 0x48FF7000U;
constexpr std::uint32_t kPlayers = 0x423BCE40U;
constexpr std::uint32_t kPlayer = 0x4271CE80U;
constexpr std::uint32_t kController = 0x425A6000U;
constexpr std::uint32_t kWorldInfo = 0x42550000U;
constexpr std::uint32_t kFirst = 0x46100000U;
constexpr std::uint32_t kSecond = 0x46200000U;
constexpr std::uint32_t kPaths = 0x46300000U;
constexpr std::uint32_t kForward = 0x46400000U;
constexpr std::uint32_t kBack = 0x46500000U;

void PutPoint(FakeGuest &guest, std::uint32_t point, std::uint32_t vtable, float x, float y,
              std::uint32_t paths, std::uint32_t path_count, std::uint32_t next)
{
    guest.Put(point, vtable);
    guest.PutFloat(point + kActorLocationOffset, x);
    guest.PutFloat(point + kActorLocationOffset + 4U, y);
    guest.PutFloat(point + kActorLocationOffset + 8U, 200.0F);
    // A yaw of a quarter turn, one full turn over: the probe keeps it within one.
    guest.Put(point + kActorRotationOffset + 4U, 0x14000U);
    guest.Put(point + kNavigationPathsOffset, paths);
    guest.Put(point + kNavigationPathsOffset + 4U, path_count);
    guest.Put(point + kNavigationNextOffset, next);
}

void PutPath(FakeGuest &guest, std::uint32_t spec, std::uint32_t start, std::uint32_t end,
             std::uint32_t distance, std::uint32_t vtable)
{
    guest.Put(spec, vtable);
    guest.Put(spec + kReachSpecStartOffset, start);
    guest.Put(spec + kReachSpecEndOffset, end);
    guest.Put(spec + kReachSpecDistanceOffset, distance);
}

// A path node and a cover slot joined both ways, walking forward and mantling
// back; the second's array holds only the way back.
FakeGuest LevelGuest()
{
    FakeGuest guest;
    guest.Put(kGEngineAddress, kEngine);
    guest.Put(kEngine + kEngineGamePlayersOffset, kPlayers);
    guest.Put(kEngine + kEngineGamePlayersOffset + 4U, 1U);
    guest.Put(kPlayers, kPlayer);
    guest.Put(kPlayer + kPlayerControllerOffset, kController);
    guest.Put(kController + kActorWorldInfoOffset, kWorldInfo);
    guest.Put(kWorldInfo + kWorldInfoNavigationListOffset, kFirst);
    PutPoint(guest, kFirst, kPathNodeVtable, -1606.0F, 2573.0F, kPaths, 1U, kSecond);
    PutPoint(guest, kSecond, kCoverSlotVtable, -1606.0F, 2730.0F, kPaths + 4U, 1U, 0U);
    guest.Put(kPaths, kForward);
    guest.Put(kPaths + 4U, kBack);
    PutPath(guest, kForward, kFirst, kSecond, 157U, kWalkReachSpecVtable);
    PutPath(guest, kBack, kSecond, kFirst, 157U, kMantleReachSpecVtable);
    return guest;
}

void RequireRefused(const FakeGuest &guest, std::string_view expected)
{
    std::vector<NavigationPoint> points;
    std::string error;
    Require(!ReadNavigation(guest.Reader(), points, error), "accepted a broken graph");
    Require(error.find(expected) != std::string::npos,
            "refusal '" + error + "' does not name: " + std::string(expected));
}

} // namespace

int main()
{
    FakeGuest guest = LevelGuest();
    std::vector<NavigationPoint> points;
    std::string error;
    Require(ReadNavigation(guest.Reader(), points, error), error);
    Require(points.size() == 2U && points[0].address == kFirst && points[1].address == kSecond,
            "the points were not listed in order");
    Require(points[0].location[0] == -1606.0F && points[1].location[1] == 2730.0F,
            "the points' locations were not read");
    Require(points[0].kind == PointKind::PathNode && points[1].kind == PointKind::Cover,
            "the path node and the cover slot were not told apart");
    Require(points[1].yaw == 0x4000U, "the cover slot's facing was not read within one turn");
    Require(points[0].paths.size() == 1U && points[0].paths[0].end == kSecond &&
                points[0].paths[0].distance == 157 && points[0].paths[0].kind == PathKind::Walk,
            "the forward path was not read");
    Require(points[1].paths.size() == 1U && points[1].paths[0].end == kFirst &&
                points[1].paths[0].kind == PathKind::Mantle,
            "the way back was not read as a mantle");

    FakeGuest special = LevelGuest();
    special.Put(kForward, 0x820DF980U);
    Require(ReadNavigation(special.Reader(), points, error) &&
                points[0].paths[0].kind == PathKind::Other &&
                points[0].paths[0].vtable == 0x820DF980U,
            "an unknown path class was not reported with its vtable");
    FakeGuest pickup = LevelGuest();
    pickup.Put(kFirst, 0x82095000U);
    Require(ReadNavigation(pickup.Reader(), points, error) && points[0].kind == PointKind::Other &&
                points[0].vtable == 0x82095000U,
            "an unknown point class was not reported with its vtable");
    FakeGuest unturned = LevelGuest();
    unturned.Erase(kSecond + kActorRotationOffset + 4U);
    RequireRefused(unturned, "a point's yaw at 0x462000DC is unreadable");
    FakeGuest unnamed = LevelGuest();
    unnamed.Erase(kSecond);
    RequireRefused(unnamed, "a point's class at 0x46200000 is unreadable");
    FakeGuest classless = LevelGuest();
    classless.Erase(kBack);
    RequireRefused(classless, "a path's class at 0x46500000 is unreadable");

    FakeGuest isolated = LevelGuest();
    isolated.Put(kSecond + kNavigationPathsOffset + 4U, 0U);
    Require(ReadNavigation(isolated.Reader(), points, error) && points[1].paths.empty(),
            "a point without paths was not read as a dead end");

    FakeGuest cyclic = LevelGuest();
    cyclic.Put(kSecond + kNavigationNextOffset, kFirst);
    RequireRefused(cyclic, "runs past 8192 points");
    FakeGuest crowded = LevelGuest();
    crowded.Put(kFirst + kNavigationPathsOffset + 4U, kMaxNavigationPaths + 1U);
    RequireRefused(crowded, "point 0x46100000 lists 65 paths, past 64");
    FakeGuest foreign = LevelGuest();
    foreign.Put(kForward + kReachSpecStartOffset, kSecond);
    RequireRefused(foreign, "path 0x46400000 of point 0x46100000 starts at 0x46200000");
    FakeGuest endless = LevelGuest();
    endless.Put(kBack + kReachSpecEndOffset, 0U);
    Require(ReadNavigation(endless.Reader(), points, error) && points[1].paths.size() == 1U &&
                points[1].paths[0].end == 0U,
            "a path whose end is not loaded was not reported with a null end");
    FakeGuest torn_end = LevelGuest();
    torn_end.Erase(kBack + kReachSpecEndOffset);
    RequireRefused(torn_end, "a path's end at 0x46500048 is unreadable");
    FakeGuest torn = LevelGuest();
    torn.Erase(kSecond + kActorLocationOffset + 4U);
    RequireRefused(torn, "a point's location at 0x462000D0 is unreadable");
    FakeGuest no_player = LevelGuest();
    no_player.Put(kEngine + kEngineGamePlayersOffset + 4U, 0U);
    RequireRefused(no_player, "no local player");
    return EXIT_SUCCESS;
}
