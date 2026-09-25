#pragma once

#include "mesh/skeletal_mesh.h"
#include "pawn_defaults.h"
#include "player_pawn.h"
#include "scene/transform.h"

namespace gears::engine::game
{

// Where `pawn`'s skeletal mesh draws: turned from its own axes into its
// component's by the mesh's origin rotation, offset by the component's
// translation, then placed at the pawn's location facing its facing yaw.
[[nodiscard]] scene::Matrix BodyPlacement(const PlayerPawn &pawn,
                                          const PawnAppearance &appearance,
                                          const mesh::SkeletalMesh &mesh);

} // namespace gears::engine::game
