#pragma once

#include <cstddef>
#include <map>
#include <utility>

#include "bsp_model.h"
#include "mesh/static_mesh.h"
#include "object/class_hierarchy.h"
#include "object/object_resolver.h"
#include "object/serialized_object.h"

namespace gears::engine::bsp
{

// Turns ModelComponent exports into triangle meshes, reading each model they
// draw from once however many components share it.
class ComponentGeometry
{
  public:
    ComponentGeometry(object::ClassHierarchy &classes, object::ObjectResolver &resolver);

    // The component's triangles, one section per element; each section's
    // material is a reference of the component's package. Refuses a
    // component whose model does not resolve to an export.
    mesh::StaticMeshLod Triangulate(const object::SerializedObject &component);

  private:
    const BspModel &ModelOf(const package::Package &package, object::PackageIndex model);

    object::ClassHierarchy &classes_;
    object::ObjectResolver &resolver_;
    std::map<std::pair<const package::Package *, std::size_t>, BspModel> models_;
};

} // namespace gears::engine::bsp
