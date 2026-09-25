#include "static_meshes.h"

#include "object/serialized_object.h"

namespace gears::engine::scene
{

const mesh::StaticMesh &StaticMeshes::Get(const object::ExportLocation &mesh)
{
    std::pair key{mesh.package, mesh.export_index};
    auto found = meshes_.find(key);
    if (found == meshes_.end())
    {
        auto object = object::SerializedObject::Read(*mesh.package, mesh.export_index, classes_);
        found = meshes_.emplace(key, mesh::StaticMesh::Read(object)).first;
    }
    return found->second;
}

} // namespace gears::engine::scene
