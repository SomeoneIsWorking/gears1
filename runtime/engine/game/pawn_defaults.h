#pragma once

#include <string>
#include <string_view>

#include "object/class_defaults.h"
#include "pawn_tuning.h"

namespace gears::engine::game
{

// The Gears 1 single-player game type, whose defaults name the player pawn
// class.
inline constexpr std::string_view kSinglePlayerGameClass = "WarfareGame.WarGameSP";
// The special move whose speed modifier scales ground speed while roadie
// running.
inline constexpr std::string_view kRoadieRunMoveClass = "WarfareGame.WarSpecialMove_RoadieRun";
// The class whose defaults hold the world's gravity.
inline constexpr std::string_view kWorldInfoClass = "Engine.WorldInfo";
// The pawn's collision cylinder subobject.
inline constexpr std::string_view kCollisionCylinderName = "CollisionCylinder";

// The pawn class `game_class`'s defaults spawn for the player. Refuses a game
// type that names none.
[[nodiscard]] std::string PlayerPawnClass(object::ClassDefaults &defaults,
                                          const std::string &game_class);

// The movement and collision of `pawn_class`, read from its class defaults:
// its collision cylinder, ground speed, acceleration, step height, walkable
// floor slope and yaw rotation rate; the roadie-run move's speed modifier;
// and the world's default gravity. Refuses a pawn that stores no collision
// cylinder size or ground speed.
[[nodiscard]] PawnTuning ReadPawnTuning(object::ClassDefaults &defaults,
                                        const std::string &pawn_class);

} // namespace gears::engine::game
