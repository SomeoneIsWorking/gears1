#include "material_surfaces.h"

#include <array>
#include <format>

#include "object/serialized_object.h"
#include "package/byte_reader.h"

namespace gears::engine::material
{
namespace
{

constexpr std::string_view kMaterialClass = "Engine.Material";
constexpr std::string_view kMaterialInstanceClass = "Engine.MaterialInstanceConstant";
constexpr std::string_view kTextureSampleClass = "Engine.MaterialExpressionTextureSample";
constexpr std::string_view kTextureParameterClass =
    "Engine.MaterialExpressionTextureSampleParameter";
constexpr std::string_view kExpressionInputStruct = "ExpressionInput";
constexpr std::string_view kColorInputStruct = "ColorMaterialInput";
constexpr std::string_view kScalarInputStruct = "ScalarMaterialInput";
// The LightingModel value of a material shaded by its emissive colour alone.
constexpr std::uint8_t kLightingModelUnlit = 2;
// Instance parents and expression inputs deeper than this are a cycle or a
// misread, not an authored material.
constexpr std::size_t kMaxDepth = 64;
// Each single-channel mask flag of an expression input and its channel.
constexpr std::array<std::pair<std::string_view, Channel>, 4> kChannelMasks{{
    {"MaskR", Channel::kRed},
    {"MaskG", Channel::kGreen},
    {"MaskB", Channel::kBlue},
    {"MaskA", Channel::kAlpha},
}};

std::string PathOf(const object::ExportLocation &location)
{
    return location.package->FullPath(
        static_cast<package::PackageIndex>(location.export_index + 1U));
}

// The channel an expression input reads: one channel when it masks all but
// one, otherwise the colour.
Channel ChannelOf(const object::PropertyValues &input)
{
    if (input.Int("Mask") == 0)
    {
        return Channel::kColor;
    }
    Channel channel = Channel::kColor;
    std::size_t selected = 0;
    for (const auto &[flag, masked] : kChannelMasks)
    {
        if (input.Int(flag) != 0)
        {
            channel = masked;
            ++selected;
        }
    }
    return selected == 1U ? channel : Channel::kColor;
}

BlendMode BlendOf(const object::PropertyValues &properties, const std::string &path)
{
    std::uint8_t stored = properties.Byte("BlendMode");
    if (stored > static_cast<std::uint8_t>(BlendMode::kModulative))
    {
        throw package::PackageFormatError(
            std::format("material {} stores unknown blend mode {}", path, stored));
    }
    return static_cast<BlendMode>(stored);
}

} // namespace

std::string_view NameOf(ColorOutcome outcome) noexcept
{
    switch (outcome)
    {
    case ColorOutcome::kTexture:
        return "texture";
    case ColorOutcome::kNoColorInput:
        return "no colour input";
    case ColorOutcome::kNoTextureInGraph:
        return "no texture in the colour graph";
    case ColorOutcome::kCookedOut:
        return "cooked out";
    case ColorOutcome::kNoParent:
        return "instance with no parent";
    }
    return "unknown";
}

std::string_view NameOf(BlendMode blend) noexcept
{
    switch (blend)
    {
    case BlendMode::kOpaque:
        return "opaque";
    case BlendMode::kMasked:
        return "masked";
    case BlendMode::kTranslucent:
        return "translucent";
    case BlendMode::kAdditive:
        return "additive";
    case BlendMode::kModulative:
        return "modulative";
    }
    return "unknown";
}

const MaterialSurface &MaterialSurfaces::Surface(const object::ExportLocation &material)
{
    Key key{material.package, material.export_index};
    auto cached = cache_.find(key);
    if (cached == cache_.end())
    {
        cached = cache_.emplace(key, Resolve(material)).first;
    }
    return cached->second;
}

ColorTexture MaterialSurfaces::FromTexture(const object::Resolution &texture, Channel channel)
{
    switch (texture.status)
    {
    case object::ResolutionStatus::kFound:
        return {ColorOutcome::kTexture, texture.location, channel};
    case object::ResolutionStatus::kCookedOut:
        return {ColorOutcome::kCookedOut, {}, channel};
    case object::ResolutionStatus::kNull:
        break;
    }
    return {ColorOutcome::kNoTextureInGraph, {}, channel};
}

MaterialSurface MaterialSurfaces::Resolve(const object::ExportLocation &material)
{
    Parameters parameters;
    object::ExportLocation current = material;
    for (std::size_t depth = 0; depth < kMaxDepth; ++depth)
    {
        const package::Package &package = *current.package;
        auto index = static_cast<package::PackageIndex>(current.export_index + 1U);
        std::string class_path = object::ClassHierarchy::ClassPath(package, index);
        if (classes_.IsA(class_path, kMaterialClass))
        {
            return FromBase(current, parameters);
        }
        if (!classes_.IsA(class_path, kMaterialInstanceClass))
        {
            throw package::PackageFormatError(
                std::format("{} is a {}, not a material", PathOf(current), class_path));
        }
        auto object = object::SerializedObject::Read(package, current.export_index, classes_);
        object::PropertyValues properties(object.Properties());
        // The nearest instance's assignment wins over its parents'.
        for (const auto &element : properties.StructArray("TextureParameterValues"))
        {
            object::PropertyValues value(element);
            std::optional<std::string> name = value.Name("ParameterName");
            if (name)
            {
                parameters.try_emplace(*name,
                                       resolver_.Resolve(package, value.Object("ParameterValue")));
            }
        }
        object::Resolution parent = resolver_.Resolve(package, properties.Object("Parent"));
        if (parent.status != object::ResolutionStatus::kFound)
        {
            MaterialSurface unresolved;
            unresolved.color.outcome = parent.status == object::ResolutionStatus::kCookedOut
                                           ? ColorOutcome::kCookedOut
                                           : ColorOutcome::kNoParent;
            return unresolved;
        }
        current = parent.location;
    }
    throw package::PackageFormatError(
        std::format("material {} has more than {} instance parents", PathOf(material), kMaxDepth));
}

MaterialSurface MaterialSurfaces::FromBase(const object::ExportLocation &material,
                                           const Parameters &parameters)
{
    auto object =
        object::SerializedObject::Read(*material.package, material.export_index, classes_);
    object::PropertyValues properties(object.Properties());
    MaterialSurface surface;
    surface.blend = BlendOf(properties, PathOf(material));
    surface.two_sided = properties.Bool("TwoSided");
    surface.opacity_clip = properties.Float("OpacityMaskClipValue", kDefaultOpacityClip);
    surface.unlit = properties.Byte("LightingModel") == kLightingModelUnlit;
    std::string_view color_input = surface.unlit ? "EmissiveColor" : "DiffuseColor";
    surface.color = FromInput(material, properties, color_input, kColorInputStruct, parameters);
    if (surface.blend == BlendMode::kMasked)
    {
        surface.opacity =
            FromInput(material, properties, "OpacityMask", kScalarInputStruct, parameters);
    }
    else if (surface.blend == BlendMode::kTranslucent)
    {
        surface.opacity =
            FromInput(material, properties, "Opacity", kScalarInputStruct, parameters);
    }
    return surface;
}

ColorTexture MaterialSurfaces::FromInput(const object::ExportLocation &material,
                                         const object::PropertyValues &properties,
                                         std::string_view input, std::string_view struct_name,
                                         const Parameters &parameters)
{
    std::optional<object::TaggedProperties> fields = properties.StructFields(input, struct_name);
    if (!fields)
    {
        return {ColorOutcome::kNoColorInput, {}, Channel::kColor};
    }
    object::PropertyValues values(*fields);
    return FromExpression(*material.package, values.Object("Expression"), ChannelOf(values),
                          parameters, 0);
}

ColorTexture MaterialSurfaces::FromExpression(const package::Package &package,
                                              package::PackageIndex expression, Channel channel,
                                              const Parameters &parameters, std::size_t depth)
{
    if (depth >= kMaxDepth)
    {
        throw package::PackageFormatError(std::format(
            "material expressions of {} nest deeper than {}", package.Name(), kMaxDepth));
    }
    object::Resolution node = resolver_.Resolve(package, expression);
    if (node.status != object::ResolutionStatus::kFound)
    {
        return FromTexture(node, channel);
    }
    const package::Package &owner = *node.location.package;
    auto index = static_cast<package::PackageIndex>(node.location.export_index + 1U);
    std::string class_path = object::ClassHierarchy::ClassPath(owner, index);
    auto object = object::SerializedObject::Read(owner, node.location.export_index, classes_);
    object::PropertyValues properties(object.Properties());
    if (classes_.IsA(class_path, kTextureParameterClass))
    {
        std::optional<std::string> name = properties.Name("ParameterName");
        auto assigned = name ? parameters.find(*name) : parameters.end();
        if (assigned != parameters.end() &&
            assigned->second.status != object::ResolutionStatus::kNull)
        {
            return FromTexture(assigned->second, channel);
        }
    }
    if (classes_.IsA(class_path, kTextureSampleClass))
    {
        return FromTexture(resolver_.Resolve(owner, properties.Object("Texture")), channel);
    }
    ColorTexture result{ColorOutcome::kNoTextureInGraph, {}, channel};
    for (const object::PropertyTag &tag : object.Properties())
    {
        if (owner.NameText(tag.type) != "StructProperty" ||
            owner.NameText(tag.struct_name) != kExpressionInputStruct)
        {
            continue;
        }
        object::TaggedProperties input = object::TaggedProperties::Parse(tag.value, owner);
        object::PropertyValues values(input);
        ColorTexture found = FromExpression(owner, values.Object("Expression"), ChannelOf(values),
                                            parameters, depth + 1U);
        if (found.outcome == ColorOutcome::kTexture)
        {
            return found;
        }
        if (found.outcome == ColorOutcome::kCookedOut)
        {
            result = found;
        }
    }
    return result;
}

} // namespace gears::engine::material
