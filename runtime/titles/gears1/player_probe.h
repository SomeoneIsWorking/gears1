#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>

#include <x360port/runtime_failure.hpp>

namespace gears::titles::gears1
{

// Where the retail image's engine keeps its local player, measured in the
// running title (docs/re-frontier.md, "Local player"). Each offset is into the
// object the previous step names.
inline constexpr std::uint32_t kGEngineAddress = 0x82BED138U;
// UEngine: GamePlayers, a TArray of ULocalPlayer pointers (data, count).
inline constexpr std::uint32_t kEngineGamePlayersOffset = 0x29CU;
// UPlayer::Actor, the player controller.
inline constexpr std::uint32_t kPlayerControllerOffset = 0x40U;
// Controller: the possessed pawn, null while the player is dead.
inline constexpr std::uint32_t kControllerPawnOffset = 0x1A0U;
// Actor: the level's WorldInfo, and in it the world's clock in game seconds.
inline constexpr std::uint32_t kActorWorldInfoOffset = 0x8CU;
inline constexpr std::uint32_t kWorldInfoTimeSecondsOffset = 0x288U;
// PlayerController: the camera actor, whose rotation is the view.
inline constexpr std::uint32_t kControllerCameraOffset = 0x294U;
// AActor: Location (three floats) and Rotation (pitch, yaw, roll).
inline constexpr std::uint32_t kActorLocationOffset = 0xCCU;
inline constexpr std::uint32_t kActorRotationOffset = 0xD8U;
// Pawn: the held weapon; weapon: rounds fired from the current magazine.
inline constexpr std::uint32_t kPawnWeaponOffset = 0x35CU;
inline constexpr std::uint32_t kWeaponMagazineRoundsFiredOffset = 0x490U;

// Copies guest virtual memory, failing for a range that is not readable.
using GuestMemoryReader =
    std::function<x360port::RuntimeFailure(std::uint32_t address, std::span<std::byte> bytes)>;

// The local player as the engine holds it at one moment. Yaws are in engine
// units, 65536 per turn, with the world's +y a quarter turn clockwise of +x
// seen from above, so the pad's right stick direction is yaw + 16384.
struct PlayerSnapshot
{
    std::uint16_t control_yaw = 0;
    std::uint16_t camera_yaw = 0;
    // Game time since the level began; it advances one second per wall second
    // unless the simulation runs faster or slower than real time.
    float world_seconds = 0.0F;
    bool has_pawn = false;
    std::array<float, 3> location{};
    bool has_weapon = false;
    std::uint32_t magazine_rounds_fired = 0;
};

// Reads the local player. Refuses, naming the step, while the engine has no
// local player controller (before gameplay) or any step is unreadable. A dead
// player, whose controller holds no pawn, is a snapshot without a pawn.
[[nodiscard]] bool ReadPlayer(const GuestMemoryReader &read, PlayerSnapshot &snapshot,
                              std::string &error);

} // namespace gears::titles::gears1
