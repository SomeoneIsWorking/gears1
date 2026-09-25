#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <cstdint>
#include <utility>

#include "object/class_hierarchy.h"
#include "object/object_resolver.h"

namespace gears::engine::material
{

// Why a material has, or lacks, a base colour texture.
enum class ColorOutcome : std::uint8_t
{
    kTexture,
    // The material stores no colour input (it keeps the black default).
    kNoColorInput,
    // The colour input's expressions sample no texture (constants, vertex
    // colour, or other computed terms).
    kNoTextureInGraph,
    // The texture or an expression, parent, or instance on the path was
    // stripped by the cooker.
    kCookedOut,
    // A material instance with no parent: it draws the default material
    // until something assigns one at run time.
    kNoParent,
};

std::string_view NameOf(ColorOutcome outcome) noexcept;

struct ColorTexture
{
    ColorOutcome outcome = ColorOutcome::kNoTextureInGraph;
    // Set when the outcome is kTexture.
    object::ExportLocation texture;
};

// Finds the texture that gives a material its base colour, by walking the
// material's own expression graph from its colour input: DiffuseColor for a
// lit material, EmissiveColor for an unlit one (unlit shading outputs only
// its emissive colour). Inputs are
// followed in the order the expression stores them and the first texture
// reached is the result; a texture parameter takes the value the nearest
// material instance assigns it. This picks the base texture of a material,
// not its evaluated colour: blends, tints and other terms are not applied.
class MaterialTextures
{
  public:
    MaterialTextures(object::ClassHierarchy &classes, object::ObjectResolver &resolver)
        : classes_(classes), resolver_(resolver)
    {
    }

    // Refuses a material of a class it does not know.
    [[nodiscard]] ColorTexture BaseColor(const object::ExportLocation &material);

  private:
    using Key = std::pair<const package::Package *, std::size_t>;
    // Texture parameter name to the texture a material instance assigns.
    using Parameters = std::map<std::string, object::Resolution>;

    [[nodiscard]] ColorTexture Walk(const object::ExportLocation &material);
    [[nodiscard]] ColorTexture FromExpression(const package::Package &package,
                                              package::PackageIndex expression,
                                              const Parameters &parameters, std::size_t depth);
    [[nodiscard]] static ColorTexture FromTexture(const object::Resolution &texture);

    object::ClassHierarchy &classes_;
    object::ObjectResolver &resolver_;
    std::map<Key, ColorTexture> cache_;
};

} // namespace gears::engine::material
