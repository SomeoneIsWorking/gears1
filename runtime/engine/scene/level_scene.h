#pragma once

#include <cstddef>
#include <vector>

#include "mesh/static_mesh.h"
#include "object/class_hierarchy.h"
#include "object/object_resolver.h"
#include "package/package.h"
#include "transform.h"

namespace gears::engine::scene
{

// One placed static mesh: the mesh export that supplies its geometry, where
// it stands in the world, and the materials its component overrides (by
// section, 0 where the mesh's own material applies), as references of the
// level package.
struct MeshInstance
{
    object::ExportLocation mesh;
    Matrix world;
    std::vector<package::PackageIndex> material_overrides;
};

// The level's BSP surfaces one model component draws, already in world
// space; each section's material is a reference of `package`.
struct ModelInstance
{
    const package::Package *package = nullptr;
    mesh::StaticMeshLod geometry;
};

struct SceneCensus
{
    std::size_t model_components = 0;
    std::size_t components = 0;
    // Components of class default objects: templates, not placements.
    std::size_t templates = 0;
    std::size_t placed = 0;
    std::size_t without_mesh = 0;
    // Placements whose mesh the cooker stripped (editor-only helpers).
    std::size_t cooked_out_mesh = 0;
    std::size_t outside_actor = 0;
};

// The static geometry a level package places: every static mesh component
// owned by an actor, positioned by that actor and the component's own offset,
// and the BSP surfaces of every model component.
class LevelScene
{
  public:
    static LevelScene Build(const package::Package &level, object::ClassHierarchy &classes,
                            object::ObjectResolver &resolver);

    [[nodiscard]] const std::vector<MeshInstance> &Instances() const noexcept { return instances_; }
    [[nodiscard]] const std::vector<ModelInstance> &Models() const noexcept { return models_; }
    [[nodiscard]] const SceneCensus &Census() const noexcept { return census_; }

  private:
    std::vector<MeshInstance> instances_;
    std::vector<ModelInstance> models_;
    SceneCensus census_;
};

} // namespace gears::engine::scene
