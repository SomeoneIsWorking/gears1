#include "player_probe.h"

#include <bit>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <string_view>

#include <x360port/guest_endian.hpp>

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

// Guest memory as the words a test places; every other address is unreadable.
class FakeGuest
{
  public:
    void Put(std::uint32_t address, std::uint32_t word) { words_[address] = word; }
    void PutFloat(std::uint32_t address, float value)
    {
        Put(address, std::bit_cast<std::uint32_t>(value));
    }
    void Erase(std::uint32_t address) { words_.erase(address); }

    [[nodiscard]] GuestMemoryReader Reader() const
    {
        return [this](std::uint32_t address, std::span<std::byte> bytes)
        {
            auto word = words_.find(address);
            if (bytes.size() != 4U || word == words_.end())
            {
                return x360port::RuntimeFailure{x360port::RuntimeError::GuestMemoryRangeInvalid,
                                                "not placed"};
            }
            x360port::StoreGuestWord(bytes, 0, word->second);
            return x360port::RuntimeFailure{};
        };
    }

  private:
    std::map<std::uint32_t, std::uint32_t> words_;
};

constexpr std::uint32_t kEngine = 0x48FF7000U;
constexpr std::uint32_t kPlayers = 0x423BCE40U;
constexpr std::uint32_t kPlayer = 0x4271CE80U;
constexpr std::uint32_t kController = 0x425A6000U;
constexpr std::uint32_t kCamera = 0x48FA8700U;
constexpr std::uint32_t kPawn = 0x4742AA00U;
constexpr std::uint32_t kWeapon = 0x4645AA00U;
constexpr std::uint32_t kWorldInfo = 0x42550000U;

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
    guest.Put(kController + kActorRotationOffset + 4U, 104707U);
    guest.Put(kCamera + kActorRotationOffset + 4U, 0xFFFF0001U);
    guest.Put(kController + kActorWorldInfoOffset, kWorldInfo);
    guest.PutFloat(kWorldInfo + kWorldInfoTimeSecondsOffset, 224.5F);
    guest.Put(kController + kControllerPawnOffset, kPawn);
    guest.PutFloat(kPawn + kActorLocationOffset, -1051.5F);
    guest.PutFloat(kPawn + kActorLocationOffset + 4U, 3640.0F);
    guest.PutFloat(kPawn + kActorLocationOffset + 8U, 203.0F);
    guest.Put(kPawn + kPawnWeaponOffset, kWeapon);
    guest.Put(kWeapon + kWeaponMagazineRoundsFiredOffset, 42U);
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
    Require(snapshot.world_seconds == 224.5F, "the world's time was not read");
    Require(snapshot.has_weapon && snapshot.magazine_rounds_fired == 42U,
            "the weapon's rounds were not read");

    guest.Put(kPawn + kPawnWeaponOffset, 0U);
    Require(ReadPlayer(guest.Reader(), snapshot, error) && snapshot.has_pawn &&
                !snapshot.has_weapon,
            "an unarmed pawn was not reported as unarmed");

    guest.Put(kController + kControllerPawnOffset, 0U);
    Require(ReadPlayer(guest.Reader(), snapshot, error) && !snapshot.has_pawn &&
                snapshot.control_yaw == 104707U % 65536U,
            "a dead player was not reported without a pawn");

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
    RequireRefused(unreadable_pawn, "the pawn's location at 0x4742AAD4 is unreadable");
    FakeGuest no_world = PlayingGuest();
    no_world.Put(kController + kActorWorldInfoOffset, 0U);
    RequireRefused(no_world, "the controller's world info");
    return EXIT_SUCCESS;
}
