#pragma once

#include "bsp_model.h"
#include "mesh/static_mesh.h"
#include "model_component.h"

namespace gears::engine::bsp
{

// World units per texture repeat along a surface's texture axes.
inline constexpr float kTextureUnitsPerRepeat = 128.0F;

// Triangulates a model component into a mesh LOD with one section per
// element (its material a reference of the component's package). Each node
// becomes a fan over its outline, lit by its plane's normal, with texture
// coordinates projected on its surface's axes from the surface's base point.
// Refuses an element node outside the model and a component whose vertices
// do not fit 16-bit indices.
mesh::StaticMeshLod TriangulateComponent(const BspModel &model, const ModelComponent &component);

} // namespace gears::engine::bsp
