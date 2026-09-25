#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "object/class_hierarchy.h"
#include "object/object_resolver.h"
#include "object/property_values.h"

namespace gears::engine::material
{

// Why a material input has, or lacks, a texture.
enum class ColorOutcome : std::uint8_t
{
    kTexture,
    // The material stores no such input (it keeps its default).
    kNoColorInput,
    // The input's expressions sample no texture (constants, vertex colour, or
    // other computed terms).
    kNoTextureInGraph,
    // The texture or an expression, parent, or instance on the path was
    // stripped by the cooker.
    kCookedOut,
    // A material instance with no parent: it draws the default material
    // until something assigns one at run time.
    kNoParent,
};

std::string_view NameOf(ColorOutcome outcome) noexcept;

// How a material's output combines with what is already drawn, in the
// order the title's BlendMode byte stores them.
enum class BlendMode : std::uint8_t
{
    kOpaque,
    kMasked,
    kTranslucent,
    kAdditive,
    kModulative,
};

std::string_view NameOf(BlendMode blend) noexcept;

// The texture channels an expression input reads.
enum class Channel : std::uint8_t
{
    kColor,
    kRed,
    kGreen,
    kBlue,
    kAlpha,
};

struct ColorTexture
{
    ColorOutcome outcome = ColorOutcome::kNoTextureInGraph;
    // Set when the outcome is kTexture.
    object::ExportLocation texture;
    // The channel the input nearest the texture sample reads.
    Channel channel = Channel::kColor;
};

// The clip value of a masked material that stores none.
inline constexpr float kDefaultOpacityClip = 1.0F / 3.0F;

// What the renderer needs of a material: its base-colour texture, how it
// blends, and for masked and translucent materials the texture channel its
// opacity reads.
struct MaterialSurface
{
    ColorTexture color;
    BlendMode blend = BlendMode::kOpaque;
    bool two_sided = false;
    // Shaded by its emissive colour alone; scene lighting does not apply.
    bool unlit = false;
    // OpacityMask for a masked material, Opacity for a translucent one;
    // kNoColorInput for the others.
    ColorTexture opacity{ColorOutcome::kNoColorInput, {}, Channel::kColor};
    float opacity_clip = kDefaultOpacityClip;
};

// Resolves materials to surfaces by walking the base material's own
// expression graph from each input: DiffuseColor for a lit material's colour,
// EmissiveColor for an unlit one (unlit shading outputs only its emissive
// colour), OpacityMask or Opacity for its opacity. Inputs are followed in
// the order the expression stores them and the first texture reached is the
// result; a texture parameter takes the value the nearest material instance
// assigns it. Blend mode, two-sidedness and the clip value are the base
// material's. This picks textures, not evaluated values: blends, tints and
// other terms are not applied.
class MaterialSurfaces
{
  public:
    MaterialSurfaces(object::ClassHierarchy &classes, object::ObjectResolver &resolver)
        : classes_(classes), resolver_(resolver)
    {
    }

    // Refuses a material of a class it does not know.
    [[nodiscard]] const MaterialSurface &Surface(const object::ExportLocation &material);

  private:
    using Key = std::pair<const package::Package *, std::size_t>;
    // Texture parameter name to the texture a material instance assigns.
    using Parameters = std::map<std::string, object::Resolution>;

    [[nodiscard]] MaterialSurface Resolve(const object::ExportLocation &material);
    [[nodiscard]] MaterialSurface FromBase(const object::ExportLocation &material,
                                           const Parameters &parameters);
    [[nodiscard]] ColorTexture FromInput(const object::ExportLocation &material,
                                         const object::PropertyValues &properties,
                                         std::string_view input, std::string_view struct_name,
                                         const Parameters &parameters);
    [[nodiscard]] ColorTexture FromExpression(const package::Package &package,
                                              package::PackageIndex expression, Channel channel,
                                              const Parameters &parameters, std::size_t depth);
    [[nodiscard]] static ColorTexture FromTexture(const object::Resolution &texture,
                                                  Channel channel);

    object::ClassHierarchy &classes_;
    object::ObjectResolver &resolver_;
    std::map<Key, MaterialSurface> cache_;
};

} // namespace gears::engine::material
