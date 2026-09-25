#include "draw_materials.h"

#include <array>

#include "object/serialized_object.h"
#include "texture/texture2d.h"

namespace gears::engine::render
{
namespace
{

// The base colour of a section whose material gives it no texture.
constexpr std::array<std::uint8_t, 4> kNeutralColor{150, 146, 140, 255};

std::uint32_t ChannelIndex(material::Channel channel) noexcept
{
    switch (channel)
    {
    case material::Channel::kColor:
    case material::Channel::kRed:
        break;
    case material::Channel::kGreen:
        return 1;
    case material::Channel::kBlue:
        return 2;
    case material::Channel::kAlpha:
        return 3;
    }
    return 0;
}

std::string_view NameOf(Lighting lighting) noexcept
{
    switch (lighting)
    {
    case Lighting::kUnlit:
        return "unlit";
    case Lighting::kSun:
        return "fixed sun";
    case Lighting::kLightMap:
        return "light map";
    }
    return "unknown";
}

} // namespace

DrawMaterials::DrawMaterials(const VulkanDevice &device, package::ContentFiles &files,
                             object::ClassHierarchy &classes, object::ObjectResolver &resolver,
                             TextureBindings &bindings)
    : device_(device), files_(files), classes_(classes), resolver_(resolver), bindings_(bindings),
      surfaces_(classes, resolver), neutral_(device, kNeutralColor)
{
}

DrawMaterial DrawMaterials::Of(const package::Package &package, package::PackageIndex material,
                               const std::optional<bsp::LightMap2D> &light_map)
{
    DrawMaterial draw;
    DrawTextures textures{&neutral_, &neutral_, {&neutral_, &neutral_, &neutral_}};
    object::Resolution resolved = resolver_.Resolve(package, material);
    if (resolved.status == object::ResolutionStatus::kFound)
    {
        ApplySurface(surfaces_.Surface(resolved.location), draw, textures);
    }
    else
    {
        ++census_
              .colors[resolved.status == object::ResolutionStatus::kNull ? "no material"
                                                                         : "material cooked out"];
    }
    if (light_map && draw.lighting != Lighting::kUnlit)
    {
        ApplyLightMap(package, *light_map, draw, textures);
    }
    ++census_.lighting[std::string(NameOf(draw.lighting))];
    draw.textures = SetOf(textures);
    return draw;
}

void DrawMaterials::ApplySurface(const material::MaterialSurface &surface, DrawMaterial &draw,
                                 DrawTextures &textures)
{
    textures.color = &TextureOr(surface.color, ColorSpace::kSrgb);
    std::string color_source(material::NameOf(surface.color.outcome));
    if (surface.color.outcome == material::ColorOutcome::kTexture && textures.color == &neutral_)
    {
        color_source = "texture format not sampled";
    }
    ++census_.colors[color_source];
    ++census_.blends[std::string(material::NameOf(surface.blend))];

    draw.lighting = surface.unlit ? Lighting::kUnlit : Lighting::kSun;
    draw.clip = surface.opacity_clip;
    draw.channel = ChannelIndex(surface.opacity.channel);
    // A computed opacity is not evaluated; the section keeps full opacity.
    textures.opacity = &TextureOr(surface.opacity, ColorSpace::kLinear);
    bool textured_opacity = textures.opacity != &neutral_;
    switch (surface.blend)
    {
    case material::BlendMode::kOpaque:
        break;
    case material::BlendMode::kMasked:
        draw.opacity = textured_opacity ? OpacityUse::kAlphaTest : OpacityUse::kNone;
        break;
    case material::BlendMode::kTranslucent:
        draw.blend = Blend::kAlpha;
        draw.opacity = textured_opacity ? OpacityUse::kBlend : OpacityUse::kNone;
        break;
    case material::BlendMode::kAdditive:
        draw.blend = Blend::kAdditive;
        break;
    case material::BlendMode::kModulative:
        draw.blend = Blend::kModulative;
        break;
    }
}

void DrawMaterials::ApplyLightMap(const package::Package &package, const bsp::LightMap2D &light_map,
                                  DrawMaterial &draw, DrawTextures &textures)
{
    DrawTextures lit = textures;
    for (std::size_t i = 0; i < bsp::kLightMapCoefficients; ++i)
    {
        object::Resolution coefficient = resolver_.Resolve(package, light_map.textures[i]);
        if (coefficient.status != object::ResolutionStatus::kFound)
        {
            return;
        }
        const GpuTexture *uploaded = Upload(coefficient.location, ColorSpace::kLinear);
        if (uploaded == nullptr)
        {
            return;
        }
        lit.light_map[i] = uploaded;
    }
    textures = lit;
    draw.lighting = Lighting::kLightMap;
    draw.light_scales = light_map.scales;
}

const GpuTexture *DrawMaterials::Upload(const object::ExportLocation &texture, ColorSpace space)
{
    TextureKey key{texture.package, texture.export_index, space};
    auto found = textures_.find(key);
    if (found == textures_.end())
    {
        auto object =
            object::SerializedObject::Read(*texture.package, texture.export_index, classes_);
        auto decoded = texture::Texture2D::Read(object);
        std::unique_ptr<GpuTexture> uploaded;
        if (GpuTexture::CanSample(decoded.Format()))
        {
            uploaded = std::make_unique<GpuTexture>(device_, decoded, files_, space);
            ++census_.textures;
        }
        found = textures_.emplace(key, std::move(uploaded)).first;
    }
    return found->second.get();
}

const GpuTexture &DrawMaterials::TextureOr(const material::ColorTexture &input, ColorSpace space)
{
    if (input.outcome != material::ColorOutcome::kTexture)
    {
        return neutral_;
    }
    const GpuTexture *uploaded = Upload(input.texture, space);
    return uploaded != nullptr ? *uploaded : neutral_;
}

VkDescriptorSet DrawMaterials::SetOf(const DrawTextures &textures)
{
    auto found = sets_.find(textures);
    if (found == sets_.end())
    {
        found = sets_.emplace(textures, bindings_.Bind(textures)).first;
    }
    return found->second;
}

} // namespace gears::engine::render
