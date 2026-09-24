#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>

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
// Actor: the level's WorldInfo.
inline constexpr std::uint32_t kActorWorldInfoOffset = 0x8CU;
// AActor: Location (three floats) and Rotation (pitch, yaw, roll).
inline constexpr std::uint32_t kActorLocationOffset = 0xCCU;
inline constexpr std::uint32_t kActorRotationOffset = 0xD8U;

// Copies guest virtual memory, failing for a range that is not readable.
using GuestMemoryReader =
    std::function<x360port::RuntimeFailure(std::uint32_t address, std::span<std::byte> bytes)>;

// Follows guest pointers one named step at a time. The first failing step
// leaves its reason in the error the reader was given.
class ChainReader
{
  public:
    ChainReader(const GuestMemoryReader &read, std::string &error) : read_(read), error_(error) {}

    // A big-endian guest word.
    [[nodiscard]] bool Word(std::uint32_t address, std::string_view step, std::uint32_t &value);
    // A word that must hold a non-null pointer.
    [[nodiscard]] bool Object(std::uint32_t address, std::string_view step, std::uint32_t &object);
    // An actor's location.
    [[nodiscard]] bool Location(std::uint32_t actor, std::string_view step,
                                std::array<float, 3> &location);
    // One rotation component of an actor (0 pitch, 1 yaw), reduced to one turn.
    [[nodiscard]] bool Angle(std::uint32_t actor, std::uint32_t component, std::string_view step,
                             std::uint16_t &angle);
    // The first local player's controller; refuses before gameplay has one.
    [[nodiscard]] bool LocalController(std::uint32_t &controller);
    // Records a refusal that is not an unreadable step.
    [[nodiscard]] bool Fail(std::string reason);

  private:
    const GuestMemoryReader &read_;
    std::string &error_;
};

} // namespace gears::titles::gears1
