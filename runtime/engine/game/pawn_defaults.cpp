#include "pawn_defaults.h"

#include <format>
#include <optional>
#include <span>

#include "package/byte_reader.h"
#include "player_pawn.h"

namespace gears::engine::game
{
namespace
{

// A stored float the title must provide; refuses when no default stores it.
float Required(const object::DefaultChain &chain, std::string_view name, std::string_view owner)
{
    if (!chain.Has(name))
    {
        throw package::PackageFormatError(
            std::format("the defaults of {} store no {}", owner, name));
    }
    return chain.Float(name);
}

// The yaw of a Rotator struct default in radians per second.
float YawRate(const object::DefaultChain &chain, float fallback)
{
    std::span<const std::uint8_t> rotator = chain.Struct("RotationRate", "Rotator", 12U);
    if (rotator.empty())
    {
        return fallback;
    }
    package::ByteReader reader(rotator, package::ByteOrder::Big);
    (void)reader.ReadI32(); // pitch
    return RadiansOf(reader.ReadI32());
}

} // namespace

std::string PlayerPawnClass(object::ClassDefaults &defaults, const std::string &game_class)
{
    std::optional<std::string> pawn = defaults.Of(game_class).ObjectPath("DefaultPawnClass");
    if (!pawn)
    {
        throw package::PackageFormatError(
            std::format("game type {} names no default pawn class", game_class));
    }
    return *pawn;
}

PawnTuning ReadPawnTuning(object::ClassDefaults &defaults, const std::string &pawn_class)
{
    const object::DefaultChain &pawn = defaults.Of(pawn_class);
    const object::DefaultChain &cylinder =
        defaults.Subobject(pawn_class, kCollisionCylinderName);
    const object::DefaultChain &roadie_run = defaults.Of(std::string(kRoadieRunMoveClass));
    const object::DefaultChain &world = defaults.Of(std::string(kWorldInfoClass));
    PawnTuning stock;
    PawnTuning tuning;
    tuning.radius = Required(cylinder, "CollisionRadius", pawn_class);
    tuning.half_height = Required(cylinder, "CollisionHeight", pawn_class);
    tuning.run_speed = Required(pawn, "GroundSpeed", pawn_class);
    tuning.roadie_run_speed =
        tuning.run_speed * Required(roadie_run, "SpeedModifier", kRoadieRunMoveClass);
    tuning.acceleration = pawn.Float("AccelRate", stock.acceleration);
    tuning.gravity = Required(world, "DefaultGravityZ", kWorldInfoClass);
    tuning.step_height = pawn.Float("MaxStepHeight", stock.step_height);
    tuning.walkable_floor_z = pawn.Float("WalkableFloorZ", stock.walkable_floor_z);
    tuning.turn_rate = YawRate(pawn, stock.turn_rate);
    return tuning;
}

} // namespace gears::engine::game
