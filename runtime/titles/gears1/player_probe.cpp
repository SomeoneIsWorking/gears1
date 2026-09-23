#include "player_probe.h"

#include <bit>
#include <format>
#include <string_view>

#include <x360port/guest_endian.hpp>

namespace gears::titles::gears1
{
namespace
{

class ChainReader
{
  public:
    ChainReader(const GuestMemoryReader &read, std::string &error) : read_(read), error_(error) {}

    [[nodiscard]] bool Word(std::uint32_t address, std::string_view step, std::uint32_t &value)
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

    [[nodiscard]] bool Object(std::uint32_t address, std::string_view step, std::uint32_t &object)
    {
        if (!Word(address, step, object))
        {
            return false;
        }
        if (object == 0)
        {
            error_ = std::format("{} at 0x{:08X} is null", step, address);
            return false;
        }
        return true;
    }

    [[nodiscard]] bool Yaw(std::uint32_t actor, std::string_view step, std::uint16_t &yaw)
    {
        std::uint32_t value = 0;
        if (!Word(actor + kActorRotationOffset + 4U, step, value))
        {
            return false;
        }
        yaw = static_cast<std::uint16_t>(value);
        return true;
    }

  private:
    const GuestMemoryReader &read_;
    std::string &error_;
};

} // namespace

bool ReadPlayer(const GuestMemoryReader &read, PlayerSnapshot &snapshot, std::string &error)
{
    snapshot = {};
    ChainReader chain(read, error);
    std::uint32_t engine = 0;
    std::uint32_t players = 0;
    std::uint32_t player_count = 0;
    std::uint32_t player = 0;
    std::uint32_t controller = 0;
    std::uint32_t camera = 0;
    std::uint32_t pawn = 0;
    if (!chain.Object(kGEngineAddress, "GEngine", engine) ||
        !chain.Word(engine + kEngineGamePlayersOffset + 4U, "the engine's player count",
                    player_count))
    {
        return false;
    }
    if (player_count == 0)
    {
        error = "the engine has no local player";
        return false;
    }
    if (!chain.Object(engine + kEngineGamePlayersOffset, "the engine's players", players) ||
        !chain.Object(players, "the first local player", player) ||
        !chain.Object(player + kPlayerControllerOffset, "the player's controller", controller) ||
        !chain.Object(controller + kControllerCameraOffset, "the controller's camera", camera) ||
        !chain.Yaw(controller, "the controller's rotation", snapshot.control_yaw) ||
        !chain.Yaw(camera, "the camera's rotation", snapshot.camera_yaw) ||
        !chain.Word(controller + kControllerPawnOffset, "the controller's pawn", pawn))
    {
        return false;
    }
    if (pawn == 0)
    {
        return true;
    }
    snapshot.has_pawn = true;
    for (std::uint32_t axis = 0; axis < snapshot.location.size(); ++axis)
    {
        std::uint32_t bits = 0;
        if (!chain.Word(pawn + kActorLocationOffset + axis * 4U, "the pawn's location", bits))
        {
            return false;
        }
        snapshot.location[axis] = std::bit_cast<float>(bits);
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
