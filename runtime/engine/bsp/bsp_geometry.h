#pragma once

#include <optional>
#include <vector>

#include "bsp_model.h"
#include "mesh/static_mesh.h"
#include "model_component.h"

namespace gears::engine::bsp
{

// World units per texture repeat along a surface's texture axes.
inline constexpr float kTextureUnitsPerRepeat = 128.0F;

// A model component's triangles: a mesh LOD with one section per element
// that draws any, and each section's light map. Texture coordinate set 0 is
// the material's, set 1 the light map's (already scaled and biased into it).
struct ComponentMesh
{
    mesh::StaticMeshLod lod;
    std::vector<std::optional<LightMap2D>> light_maps;
};

// Triangulates a model component (section materials are references of the
// component's package). Each node becomes a fan over its outline, lit by its
// plane's normal, with material coordinates projected on its surface's axes
// from the surface's base point. Refuses an element node outside the model
// and a component whose vertices do not fit 16-bit indices.
ComponentMesh TriangulateComponent(const BspModel &model, const ModelComponent &component);

} // namespace gears::engine::bsp
