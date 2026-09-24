#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "guest_chain.h"

namespace gears::titles::gears1
{

// Offsets measured in the running title (docs/re-frontier.md, "Local
// player"), each into the object the previous step names.

// Controller: the possessed pawn, null while the player is dead.
inline constexpr std::uint32_t kControllerPawnOffset = 0x1A0U;
// WorldInfo: the world's clock in game seconds.
inline constexpr std::uint32_t kWorldInfoTimeSecondsOffset = 0x288U;
// PlayerController: the camera actor, whose location and rotation are the view.
inline constexpr std::uint32_t kControllerCameraOffset = 0x294U;
// Pawn: the held weapon. Weapon: its magazine's size, rounds fired from the
// current magazine, and spare rounds a reload (RB) draws from.
inline constexpr std::uint32_t kPawnWeaponOffset = 0x35CU;
inline constexpr std::uint32_t kWeaponMagazineSizeOffset = 0x450U;
inline constexpr std::uint32_t kWeaponMagazineRoundsFiredOffset = 0x490U;
inline constexpr std::uint32_t kWeaponSpareRoundsOffset = 0x498U;
// WorldInfo: the first pawn in the world; each pawn links to the next.
inline constexpr std::uint32_t kWorldInfoPawnListOffset = 0x328U;
inline constexpr std::uint32_t kPawnNextPawnOffset = 0x1B0U;
// Pawn: current health (a signed int, zero or below once dead) and a team
// byte, the first byte of its word: 0 for COG and 1 for Locust drones.
inline constexpr std::uint32_t kPawnHealthOffset = 0x288U;
inline constexpr std::uint32_t kPawnTeamOffset = 0x3B4U;
// A pawn list longer than this is corrupt or cyclic, not a level.
inline constexpr std::size_t kMaxWorldPawns = 256U;

// One pawn of the world: the player's, a squad mate's, or an enemy's.
struct PawnReading
{
    // The pawn object's guest address, its identity while it is listed.
    std::uint32_t address = 0;
    std::array<float, 3> location{};
    std::int32_t health = 0;
    std::uint8_t team = 0;
    bool is_player = false;
};

// The local player as the engine holds it at one moment. Yaws and pitch are in
// engine units, 65536 per turn, with the world's +y a quarter turn clockwise
// of +x seen from above, so the pad's right stick direction is yaw + 16384.
// Pitch rises as the view looks up.
struct PlayerSnapshot
{
    std::uint16_t control_yaw = 0;
    std::uint16_t control_pitch = 0;
    std::uint16_t camera_yaw = 0;
    // Where the view is from: over the pawn's right shoulder, not at the pawn.
    std::array<float, 3> camera_location{};
    // Game time since the level began; it advances one second per wall second
    // unless the simulation runs faster or slower than real time.
    float world_seconds = 0.0F;
    bool has_pawn = false;
    std::array<float, 3> location{};
    std::int32_t health = 0;
    std::uint8_t team = 0;
    bool has_weapon = false;
    // The held weapon, by guest address; it changes when the player switches.
    std::uint32_t weapon = 0;
    std::uint32_t magazine_size = 0;
    std::uint32_t magazine_rounds_fired = 0;
    std::uint32_t spare_rounds = 0;
    // Every pawn in the player's world, in the engine's list order.
    std::vector<PawnReading> pawns;
};

// Reads the local player. Refuses, naming the step, while the engine has no
// local player controller (before gameplay) or any step is unreadable. A dead
// player, whose controller holds no pawn, is a snapshot without a pawn; the
// world's pawns are still read. A pawn list longer than kMaxWorldPawns refuses.
[[nodiscard]] bool ReadPlayer(const GuestMemoryReader &read, PlayerSnapshot &snapshot,
                              std::string &error);

} // namespace gears::titles::gears1
