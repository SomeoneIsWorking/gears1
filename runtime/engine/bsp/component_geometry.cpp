#include "component_geometry.h"

#include "bsp_geometry.h"
#include "model_component.h"

namespace gears::engine::bsp
{

ComponentGeometry::ComponentGeometry(object::ClassHierarchy &classes,
                                     object::ObjectResolver &resolver)
    : classes_(classes), resolver_(resolver)
{
}

mesh::StaticMeshLod ComponentGeometry::Triangulate(const object::SerializedObject &component)
{
    ModelComponent decoded = ModelComponent::Read(component);
    return TriangulateComponent(ModelOf(component.Owner(), decoded.Model()), decoded);
}

const BspModel &ComponentGeometry::ModelOf(const package::Package &package,
                                           object::PackageIndex model)
{
    object::Resolution resolved = resolver_.Resolve(package, model);
    if (resolved.status != object::ResolutionStatus::kFound)
    {
        throw package::PackageFormatError("model component does not resolve to a model");
    }
    std::pair key{resolved.location.package, resolved.location.export_index};
    auto found = models_.find(key);
    if (found == models_.end())
    {
        auto object = object::SerializedObject::Read(*resolved.location.package,
                                                     resolved.location.export_index, classes_);
        found = models_.emplace(key, BspModel::Read(object)).first;
    }
    return found->second;
}

} // namespace gears::engine::bsp
