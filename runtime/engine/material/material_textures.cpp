#include "material_textures.h"

#include <format>

#include "object/property_values.h"
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
// The LightingModel value of a material shaded by its emissive colour alone.
constexpr std::uint8_t kLightingModelUnlit = 2;
// Instance parents and expression inputs deeper than this are a cycle or a
// misread, not an authored material.
constexpr std::size_t kMaxDepth = 64;

std::string PathOf(const object::ExportLocation &location)
{
    return location.package->FullPath(
        static_cast<package::PackageIndex>(location.export_index + 1U));
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

ColorTexture MaterialTextures::BaseColor(const object::ExportLocation &material)
{
    Key key{material.package, material.export_index};
    auto cached = cache_.find(key);
    if (cached != cache_.end())
    {
        return cached->second;
    }
    ColorTexture texture = Walk(material);
    cache_.emplace(key, texture);
    return texture;
}

ColorTexture MaterialTextures::FromTexture(const object::Resolution &texture)
{
    switch (texture.status)
    {
    case object::ResolutionStatus::kFound:
        return {ColorOutcome::kTexture, texture.location};
    case object::ResolutionStatus::kCookedOut:
        return {ColorOutcome::kCookedOut, {}};
    case object::ResolutionStatus::kNull:
        break;
    }
    return {ColorOutcome::kNoTextureInGraph, {}};
}

ColorTexture MaterialTextures::Walk(const object::ExportLocation &material)
{
    Parameters parameters;
    object::ExportLocation current = material;
    for (std::size_t depth = 0; depth < kMaxDepth; ++depth)
    {
        const package::Package &package = *current.package;
        auto index = static_cast<package::PackageIndex>(current.export_index + 1U);
        std::string class_path = object::ClassHierarchy::ClassPath(package, index);
        auto object = object::SerializedObject::Read(package, current.export_index, classes_);
        object::PropertyValues properties(object.Properties());
        if (classes_.IsA(class_path, kMaterialInstanceClass))
        {
            // The nearest instance's assignment wins over its parents'.
            for (const auto &element : properties.StructArray("TextureParameterValues"))
            {
                object::PropertyValues value(element);
                std::optional<std::string> name = value.Name("ParameterName");
                if (name)
                {
                    parameters.try_emplace(
                        *name, resolver_.Resolve(package, value.Object("ParameterValue")));
                }
            }
            object::Resolution parent = resolver_.Resolve(package, properties.Object("Parent"));
            if (parent.status == object::ResolutionStatus::kCookedOut)
            {
                return {ColorOutcome::kCookedOut, {}};
            }
            if (parent.status == object::ResolutionStatus::kNull)
            {
                return {ColorOutcome::kNoParent, {}};
            }
            current = parent.location;
            continue;
        }
        if (classes_.IsA(class_path, kMaterialClass))
        {
            std::string_view input = properties.Byte("LightingModel") == kLightingModelUnlit
                                         ? "EmissiveColor"
                                         : "DiffuseColor";
            std::optional<object::TaggedProperties> color =
                properties.StructFields(input, kColorInputStruct);
            if (!color)
            {
                return {ColorOutcome::kNoColorInput, {}};
            }
            return FromExpression(package, object::PropertyValues(*color).Object("Expression"),
                                  parameters, 0);
        }
        throw package::PackageFormatError(
            std::format("{} is a {}, not a material", PathOf(current), class_path));
    }
    throw package::PackageFormatError(
        std::format("material {} has more than {} instance parents", PathOf(material), kMaxDepth));
}

ColorTexture MaterialTextures::FromExpression(const package::Package &package,
                                              package::PackageIndex expression,
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
        return FromTexture(node);
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
            return FromTexture(assigned->second);
        }
    }
    if (classes_.IsA(class_path, kTextureSampleClass))
    {
        return FromTexture(resolver_.Resolve(owner, properties.Object("Texture")));
    }
    ColorTexture result{ColorOutcome::kNoTextureInGraph, {}};
    for (const object::PropertyTag &tag : object.Properties())
    {
        if (owner.NameText(tag.type) != "StructProperty" ||
            owner.NameText(tag.struct_name) != kExpressionInputStruct)
        {
            continue;
        }
        object::TaggedProperties input = object::TaggedProperties::Parse(tag.value, owner);
        ColorTexture found = FromExpression(
            owner, object::PropertyValues(input).Object("Expression"), parameters, depth + 1U);
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
