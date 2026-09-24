#include "player_probe.h"

#include <bit>
#include <format>
#include <string_view>
#include <vector>

namespace gears::titles::gears1
{
namespace
{

[[nodiscard]] bool ReadVitals(ChainReader &chain, std::uint32_t pawn, std::string_view step,
                              std::int32_t &health, std::uint8_t &team)
{
    std::uint32_t health_bits = 0;
    std::uint32_t team_word = 0;
    if (!chain.Word(pawn + kPawnHealthOffset, step, health_bits) ||
        !chain.Word(pawn + kPawnTeamOffset, step, team_word))
    {
        return false;
    }
    health = std::bit_cast<std::int32_t>(health_bits);
    team = static_cast<std::uint8_t>(team_word >> 24U);
    return true;
}

[[nodiscard]] bool ReadPawns(ChainReader &chain, std::uint32_t world_info,
                             std::uint32_t player_pawn, std::vector<PawnReading> &pawns)
{
    std::uint32_t pawn = 0;
    if (!chain.Word(world_info + kWorldInfoPawnListOffset, "the world's pawn list", pawn))
    {
        return false;
    }
    while (pawn != 0)
    {
        if (pawns.size() == kMaxWorldPawns)
        {
            return chain.Fail(
                std::format("the world's pawn list runs past {} pawns", kMaxWorldPawns));
        }
        PawnReading reading;
        reading.address = pawn;
        reading.is_player = pawn == player_pawn;
        if (!chain.Location(pawn, "a listed pawn's location", reading.location) ||
            !ReadVitals(chain, pawn, "a listed pawn's vitals", reading.health, reading.team) ||
            !chain.Word(pawn + kPawnNextPawnOffset, "a listed pawn's successor", pawn))
        {
            return false;
        }
        pawns.push_back(reading);
    }
    return true;
}

} // namespace

bool ReadPlayer(const GuestMemoryReader &read, PlayerSnapshot &snapshot, std::string &error)
{
    snapshot = {};
    ChainReader chain(read, error);
    std::uint32_t controller = 0;
    std::uint32_t camera = 0;
    std::uint32_t world_info = 0;
    std::uint32_t world_seconds = 0;
    std::uint32_t pawn = 0;
    if (!chain.LocalController(controller) ||
        !chain.Object(controller + kControllerCameraOffset, "the controller's camera", camera) ||
        !chain.Angle(controller, 1U, "the controller's rotation", snapshot.control_yaw) ||
        !chain.Angle(controller, 0U, "the controller's rotation", snapshot.control_pitch) ||
        !chain.Angle(camera, 1U, "the camera's rotation", snapshot.camera_yaw) ||
        !chain.Location(camera, "the camera's location", snapshot.camera_location) ||
        !chain.Object(controller + kActorWorldInfoOffset, "the controller's world info",
                      world_info) ||
        !chain.Word(world_info + kWorldInfoTimeSecondsOffset, "the world's time", world_seconds) ||
        !chain.Word(controller + kControllerPawnOffset, "the controller's pawn", pawn))
    {
        return false;
    }
    snapshot.world_seconds = std::bit_cast<float>(world_seconds);
    if (!ReadPawns(chain, world_info, pawn, snapshot.pawns))
    {
        return false;
    }
    if (pawn == 0)
    {
        return true;
    }
    snapshot.has_pawn = true;
    if (!chain.Location(pawn, "the pawn's location", snapshot.location) ||
        !ReadVitals(chain, pawn, "the pawn's vitals", snapshot.health, snapshot.team))
    {
        return false;
    }
    std::uint32_t weapon = 0;
    if (!chain.Word(pawn + kPawnWeaponOffset, "the pawn's weapon", weapon))
    {
        return false;
    }
    if (weapon == 0)
    {
        return true;
    }
    snapshot.has_weapon = true;
    return chain.Word(weapon + kWeaponMagazineRoundsFiredOffset, "the weapon's rounds fired",
                      snapshot.magazine_rounds_fired);
}

} // namespace gears::titles::gears1
