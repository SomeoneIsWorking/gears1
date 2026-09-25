#include "class_defaults.h"

#include <format>

#include "package/byte_reader.h"
#include "property_values.h"

namespace gears::engine::object
{

const SerializedObject *DefaultChain::Storing(std::string_view name) const
{
    for (const SerializedObject &object : defaults_)
    {
        if (object.Properties().Find(name) != nullptr)
        {
            return &object;
        }
    }
    return nullptr;
}

std::int32_t DefaultChain::Int(std::string_view name, std::int32_t fallback) const
{
    const SerializedObject *object = Storing(name);
    return object == nullptr ? fallback : PropertyValues(object->Properties()).Int(name);
}

float DefaultChain::Float(std::string_view name, float fallback) const
{
    const SerializedObject *object = Storing(name);
    return object == nullptr ? fallback : PropertyValues(object->Properties()).Float(name);
}

bool DefaultChain::Bool(std::string_view name, bool fallback) const
{
    const SerializedObject *object = Storing(name);
    return object == nullptr ? fallback : PropertyValues(object->Properties()).Bool(name);
}

std::optional<StoredReference> DefaultChain::Reference(std::string_view name) const
{
    const SerializedObject *object = Storing(name);
    if (object == nullptr)
    {
        return std::nullopt;
    }
    PackageIndex value = PropertyValues(object->Properties()).Object(name);
    if (value == 0)
    {
        return std::nullopt;
    }
    return StoredReference{&object->Owner(), value};
}

std::optional<std::string> DefaultChain::ObjectPath(std::string_view name) const
{
    std::optional<StoredReference> reference = Reference(name);
    if (!reference)
    {
        return std::nullopt;
    }
    return reference->package->FullPath(reference->index);
}

std::span<const std::uint8_t> DefaultChain::Struct(std::string_view name,
                                                   std::string_view struct_name,
                                                   std::size_t size) const
{
    const SerializedObject *object = Storing(name);
    return object == nullptr
               ? std::span<const std::uint8_t>{}
               : PropertyValues(object->Properties()).Struct(name, struct_name, size);
}

const DefaultChain &ClassDefaults::Of(const std::string &class_path)
{
    auto key = std::make_pair(class_path, std::string());
    auto found = chains_.find(key);
    if (found == chains_.end())
    {
        DefaultChain chain = Read(class_path, {});
        if (chain.Empty())
        {
            throw package::PackageFormatError(
                std::format("class {} and its superclasses have no default object", class_path));
        }
        found = chains_.emplace(std::move(key), std::move(chain)).first;
    }
    return found->second;
}

const DefaultChain &ClassDefaults::Subobject(const std::string &class_path, std::string_view name)
{
    auto key = std::make_pair(class_path, std::string(name));
    auto found = chains_.find(key);
    if (found == chains_.end())
    {
        DefaultChain chain = Read(class_path, "." + std::string(name));
        if (chain.Empty())
        {
            throw package::PackageFormatError(std::format(
                "no default object of {} or its superclasses owns a subobject {}", class_path,
                name));
        }
        found = chains_.emplace(std::move(key), std::move(chain)).first;
    }
    return found->second;
}

DefaultChain ClassDefaults::Read(const std::string &class_path, std::string_view suffix)
{
    std::vector<SerializedObject> defaults;
    std::optional<std::string> current = class_path;
    for (std::size_t depth = 0; current && depth < 64U; ++depth)
    {
        std::size_t dot = current->find('.');
        if (dot == std::string::npos)
        {
            throw package::PackageFormatError(
                std::format("class path '{}' names no package", *current));
        }
        std::string object_path =
            "Default__" + current->substr(dot + 1U) + std::string(suffix);
        std::optional<ExportLocation> location =
            resolver_.Find(std::string_view(*current).substr(0, dot), object_path);
        if (location)
        {
            defaults.push_back(
                SerializedObject::Read(*location->package, location->export_index, classes_));
        }
        current = classes_.Superclass(*current);
    }
    if (current)
    {
        throw package::PackageFormatError(
            std::format("class {} has a cyclic superclass chain", class_path));
    }
    return DefaultChain(std::move(defaults));
}

} // namespace gears::engine::object
