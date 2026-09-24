#include "guest_chain.h"

#include <bit>
#include <format>
#include <utility>

#include <x360port/guest_endian.hpp>

namespace gears::titles::gears1
{

bool ChainReader::Word(std::uint32_t address, std::string_view step, std::uint32_t &value)
{
    std::array<std::byte, 4> bytes{};
    if (x360port::RuntimeFailure failure = read_(address, bytes))
    {
        error_ = std::format("{} at 0x{:08X} is unreadable: {}", step, address, failure.detail);
        return false;
    }
    value = x360port::LoadGuestWord(bytes, 0);
    return true;
}

bool ChainReader::Object(std::uint32_t address, std::string_view step, std::uint32_t &object)
{
    if (!Word(address, step, object))
    {
        return false;
    }
    if (object == 0)
    {
        return Fail(std::format("{} at 0x{:08X} is null", step, address));
    }
    return true;
}

bool ChainReader::Location(std::uint32_t actor, std::string_view step,
                           std::array<float, 3> &location)
{
    for (std::uint32_t axis = 0; axis < location.size(); ++axis)
    {
        std::uint32_t bits = 0;
        if (!Word(actor + kActorLocationOffset + axis * 4U, step, bits))
        {
            return false;
        }
        location[axis] = std::bit_cast<float>(bits);
    }
    return true;
}

bool ChainReader::Angle(std::uint32_t actor, std::uint32_t component, std::string_view step,
                        std::uint16_t &angle)
{
    std::uint32_t value = 0;
    if (!Word(actor + kActorRotationOffset + component * 4U, step, value))
    {
        return false;
    }
    angle = static_cast<std::uint16_t>(value);
    return true;
}

bool ChainReader::LocalController(std::uint32_t &controller)
{
    std::uint32_t engine = 0;
    std::uint32_t player_count = 0;
    std::uint32_t players = 0;
    std::uint32_t player = 0;
    if (!Object(kGEngineAddress, "GEngine", engine) ||
        !Word(engine + kEngineGamePlayersOffset + 4U, "the engine's player count", player_count))
    {
        return false;
    }
    if (player_count == 0)
    {
        return Fail("the engine has no local player");
    }
    return Object(engine + kEngineGamePlayersOffset, "the engine's players", players) &&
           Object(players, "the first local player", player) &&
           Object(player + kPlayerControllerOffset, "the player's controller", controller);
}

bool ChainReader::Fail(std::string reason)
{
    error_ = std::move(reason);
    return false;
}

} // namespace gears::titles::gears1
