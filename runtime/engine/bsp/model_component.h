#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include "object/serialized_object.h"

namespace gears::engine::bsp
{

// One material's share of a model component: the model nodes it draws.
struct ModelElement
{
    object::PackageIndex material = 0;
    std::vector<std::uint16_t> nodes;
};

// A ModelComponent export: the model it draws from and its elements.
// Light and shadow maps are validated and skipped; they are not read yet.
class ModelComponent
{
  public:
    // Refuses a light map kind other than none or 2D, and any bytes after
    // the component's node list.
    static ModelComponent Read(const object::SerializedObject &object);

    ModelComponent(object::PackageIndex model, std::vector<ModelElement> elements)
        : model_(model), elements_(std::move(elements))
    {
    }

    [[nodiscard]] object::PackageIndex Model() const noexcept { return model_; }
    [[nodiscard]] const std::vector<ModelElement> &Elements() const noexcept { return elements_; }

  private:
    object::PackageIndex model_;
    std::vector<ModelElement> elements_;
};

} // namespace gears::engine::bsp
