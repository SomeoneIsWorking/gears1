#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "object/serialized_object.h"

namespace gears::engine::bsp
{

// Coefficients of a baked light map in a component's package.
inline constexpr std::size_t kLightMapCoefficients = 3;

// A 2D light map: its coefficient textures, each with the RGB scale that
// restores its range, and the scale and bias that map a vertex's shadow
// texture coordinate into the textures.
struct LightMap2D
{
    std::array<object::PackageIndex, kLightMapCoefficients> textures{};
    std::array<std::array<float, 3>, kLightMapCoefficients> scales{};
    std::array<float, 2> coordinate_scale{};
    std::array<float, 2> coordinate_bias{};
};

// One material's share of a model component: the model nodes it draws and
// the light map they were baked into, if any.
struct ModelElement
{
    object::PackageIndex material = 0;
    std::vector<std::uint16_t> nodes;
    std::optional<LightMap2D> light_map;
};

// A ModelComponent export: the model it draws from and its elements with
// their light maps. Shadow maps and irrelevant lights are skipped.
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
