#pragma once

#include <cstddef>
#include <map>
#include <utility>

#include "mesh/static_mesh.h"
#include "object/class_hierarchy.h"
#include "object/object_resolver.h"

namespace gears::engine::scene
{

// Decoded StaticMesh exports, each read once and kept at a stable address
// for the cache's lifetime; shared by every subsystem that needs a mesh's
// geometry (rendering, collision).
class StaticMeshes
{
  public:
    explicit StaticMeshes(object::ClassHierarchy &classes) : classes_(classes) {}

    // Refuses an export that does not decode as a static mesh.
    const mesh::StaticMesh &Get(const object::ExportLocation &mesh);

  private:
    object::ClassHierarchy &classes_;
    std::map<std::pair<const package::Package *, std::size_t>, mesh::StaticMesh> meshes_;
};

} // namespace gears::engine::scene
