#pragma once

#include <string_view>

#include "mesh/static_mesh.h"
#include "object/property_values.h"
#include "transform.h"

namespace gears::engine::scene
{

// A Vector struct property, or `fallback` when the stream does not store it.
mesh::Vector3 VectorOr(const object::PropertyValues &properties, std::string_view name,
                       mesh::Vector3 fallback);

// A Rotator struct property, or no rotation when the stream does not store it.
Rotator RotatorOf(const object::PropertyValues &properties, std::string_view name);

// The placement an actor or component stores: location (or translation),
// rotation, and a uniform scale times a per-axis scale.
Matrix Placement(const object::PropertyValues &properties, std::string_view location,
                 std::string_view rotation, std::string_view scale, std::string_view scale3d);

} // namespace gears::engine::scene
