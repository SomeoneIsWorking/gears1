#include "player_probe.h"

#include "fake_guest_memory.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace
{

using namespace gears::titles::gears1;

void Require(bool condition, std::string_view message)
{
    if (!condition)
    {
        std::cerr << "player probe: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

using FakeGuest = gears::tests::FakeGuestMemory;

constexpr std::uint32_t kEngine = 0x48FF7000U;
constexpr std::uint32_t kPlayers = 0x423BCE40U;
constexpr std::uint32_t kPlayer = 0x4271CE80U;
constexpr std::uint32_t kController = 0x425A6000U;
constexpr std::uint32_t kCamera = 0x48FA8700U;
constexpr std::uint32_t kPawn = 0x4742AA00U;
constexpr std::uint32_t kWeapon = 0x4645AA00U;
constexpr std::uint32_t kWorldInfo = 0x42550000U;
constexpr std::uint32_t kDrone = 0x475DD200U;

FakeGuest PlayingGuest()
{
    FakeGuest guest;
    guest.Put(kGEngineAddress, kEngine);
    guest.Put(kEngine + kEngineGamePlayersOffset, kPlayers);
    guest.Put(kEngine + kEngineGamePlayersOffset + 4U, 1U);
    guest.Put(kPlayers, kPlayer);
    guest.Put(kPlayer + kPlayerControllerOffset, kController);
    guest.Put(kController + kControllerCameraOffset, kCamera);
    // Rotations accumulate past a turn; only the low 16 bits are a heading.
    guest.Put(kController + kActorRotationOffset, 0xFFFFFE0CU);
    guest.Put(kController + kActorRotationOffset + 4U, 104707U);
    guest.Put(kCamera + kActorRotationOffset + 4U, 0xFFFF0001U);
    guest.PutFloat(kCamera + kActorLocationOffset, -1091.5F);
    guest.PutFloat(kCamera + kActorLocationOffset + 4U, 3570.0F);
    guest.PutFloat(kCamera + kActorLocationOffset + 8U, 269.0F);
    guest.Put(kController + kActorWorldInfoOffset, kWorldInfo);
    guest.PutFloat(kWorldInfo + kWorldInfoTimeSecondsOffset, 224.5F);
    guest.Put(kController + kControllerPawnOffset, kPawn);
    guest.PutFloat(kPawn + kActorLocationOffset, -1051.5F);
    guest.PutFloat(kPawn + kActorLocationOffset + 4U, 3640.0F);
    guest.PutFloat(kPawn + kActorLocationOffset + 8U, 203.0F);
    guest.Put(kPawn + kPawnHealthOffset, 146U);
    guest.Put(kPawn + kPawnTeamOffset, 0x00003931U);
    guest.Put(kPawn + kPawnWeaponOffset, kWeapon);
    // The world lists the player's pawn, then a dead drone.
    guest.Put(kWorldInfo + kWorldInfoPawnListOffset, kPawn);
    guest.Put(kPawn + kPawnNextPawnOffset, kDrone);
    guest.PutFloat(kDrone + kActorLocationOffset, -1367.0F);
    guest.PutFloat(kDrone + kActorLocationOffset + 4U, 4919.0F);
    guest.PutFloat(kDrone + kActorLocationOffset + 8U, 218.0F);
    guest.Put(kDrone + kPawnHealthOffset, 0xFFFFFFFDU);
    guest.Put(kDrone + kPawnTeamOffset, 0x01000000U);
    guest.Put(kDrone + kPawnNextPawnOffset, 0U);
    guest.Put(kWeapon + kWeaponMagazineSizeOffset, 60U);
    guest.Put(kWeapon + kWeaponMagazineRoundsFiredOffset, 42U);
    guest.Put(kWeapon + kWeaponSpareRoundsOffset, 529U);
    return guest;
}

void RequireRefused(const FakeGuest &guest, std::string_view expected)
{
    PlayerSnapshot snapshot;
    std::string error;
    Require(!ReadPlayer(guest.Reader(), snapshot, error), "accepted a broken chain");
    Require(error.find(expected) != std::string::npos,
            "refusal '" + error + "' does not name: " + std::string(expected));
}

} // namespace

int main()
{
    FakeGuest guest = PlayingGuest();
    PlayerSnapshot snapshot;
    std::string error;
    Require(ReadPlayer(guest.Reader(), snapshot, error), error);
    Require(snapshot.has_pawn && snapshot.location[0] == -1051.5F &&
                snapshot.location[1] == 3640.0F && snapshot.location[2] == 203.0F,
            "the pawn's location was not read");
    Require(snapshot.control_yaw == 104707U % 65536U && snapshot.camera_yaw == 1U,
            "yaws were not reduced to one turn");
    Require(snapshot.camera_location[0] == -1091.5F && snapshot.camera_location[2] == 269.0F,
            "the camera's location was not read");
    Require(snapshot.control_pitch == 0xFE0CU, "a downward pitch was not reduced to one turn");
    Require(snapshot.health == 146 && snapshot.team == 0U, "the pawn's vitals were not read");
    Require(snapshot.pawns.size() == 2U && snapshot.pawns[0].is_player &&
                snapshot.pawns[0].health == 146 && !snapshot.pawns[1].is_player &&
                snapshot.pawns[0].address == kPawn && snapshot.pawns[1].address == kDrone,
            "the world's pawns were not listed in order");
    Require(snapshot.pawns[1].health == -3 && snapshot.pawns[1].team == 1U &&
                snapshot.pawns[1].location[1] == 4919.0F,
            "a dead drone's health, team, and location were not read");
    Require(snapshot.world_seconds == 224.5F, "the world's time was not read");
    Require(snapshot.has_weapon && snapshot.weapon == kWeapon && snapshot.magazine_size == 60U &&
                snapshot.magazine_rounds_fired == 42U && snapshot.spare_rounds == 529U,
            "the weapon's rounds were not read");
    FakeGuest no_spares = PlayingGuest();
    no_spares.Erase(kWeapon + kWeaponSpareRoundsOffset);
    RequireRefused(no_spares, "the weapon's spare rounds at 0x4645AE98 is unreadable");

    guest.Put(kPawn + kPawnWeaponOffset, 0U);
    Require(ReadPlayer(guest.Reader(), snapshot, error) && snapshot.has_pawn &&
                !snapshot.has_weapon,
            "an unarmed pawn was not reported as unarmed");

    guest.Put(kController + kControllerPawnOffset, 0U);
    Require(ReadPlayer(guest.Reader(), snapshot, error) && !snapshot.has_pawn &&
                snapshot.control_yaw == 104707U % 65536U,
            "a dead player was not reported without a pawn");
    Require(snapshot.pawns.size() == 2U && !snapshot.pawns[0].is_player,
            "a dead player's world was not listed");

    FakeGuest cyclic = PlayingGuest();
    cyclic.Put(kDrone + kPawnNextPawnOffset, kPawn);
    RequireRefused(cyclic, "runs past 256 pawns");
    FakeGuest unreadable_drone = PlayingGuest();
    unreadable_drone.Erase(kDrone + kPawnTeamOffset);
    RequireRefused(unreadable_drone, "a listed pawn's vitals at 0x475DD5B4 is unreadable");

    FakeGuest no_player = PlayingGuest();
    no_player.Put(kEngine + kEngineGamePlayersOffset + 4U, 0U);
    RequireRefused(no_player, "no local player");
    FakeGuest no_engine = PlayingGuest();
    no_engine.Put(kGEngineAddress, 0U);
    RequireRefused(no_engine, "GEngine");
    FakeGuest no_controller = PlayingGuest();
    no_controller.Put(kPlayer + kPlayerControllerOffset, 0U);
    RequireRefused(no_controller, "the player's controller at 0x4271CEC0 is null");
    FakeGuest unreadable_pawn = PlayingGuest();
    unreadable_pawn.Erase(kPawn + kActorLocationOffset + 8U);
    RequireRefused(unreadable_pawn, "pawn's location at 0x4742AAD4 is unreadable");
    FakeGuest no_world = PlayingGuest();
    no_world.Put(kController + kActorWorldInfoOffset, 0U);
    RequireRefused(no_world, "the controller's world info");
    return EXIT_SUCCESS;
}
