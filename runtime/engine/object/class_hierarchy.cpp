#include "class_hierarchy.h"

#include <format>

#include "package/byte_reader.h"

namespace gears::engine::object
{

std::string ClassHierarchy::ClassPath(const package::Package &package, package::PackageIndex index)
{
    if (index < 0)
    {
        const package::ObjectImport &import =
            package.Tables().imports.at(static_cast<std::size_t>(-(index + 1)));
        return package.NameText(import.class_package) + "." + package.NameText(import.class_name);
    }
    package::PackageIndex class_index =
        package.Tables().exports.at(static_cast<std::size_t>(index) - 1U).class_index;
    return class_index == 0 ? std::string("Core.Class") : package.FullPath(class_index);
}

bool ClassHierarchy::IsA(const std::string &class_path, std::string_view ancestor)
{
    std::string current = class_path;
    for (std::size_t depth = 0; depth < 64U; ++depth)
    {
        if (current == ancestor)
        {
            return true;
        }
        const std::optional<std::string> &parent = Superclass(current);
        if (!parent)
        {
            return false;
        }
        current = *parent;
    }
    throw package::PackageFormatError(
        std::format("class {} has a cyclic superclass chain", class_path));
}

const std::optional<std::string> &ClassHierarchy::Superclass(const std::string &class_path)
{
    auto cached = parents_.find(class_path);
    if (cached != parents_.end())
    {
        return cached->second;
    }
    std::size_t dot = class_path.find('.');
    if (dot == std::string::npos)
    {
        throw package::PackageFormatError(
            std::format("class path '{}' names no package", class_path));
    }
    const package::Package &owner = store_.Load(std::string_view(class_path).substr(0, dot));
    std::string_view class_name = std::string_view(class_path).substr(dot + 1U);
    const auto &exports = owner.Tables().exports;
    for (std::size_t i = 0; i < exports.size(); ++i)
    {
        if (exports[i].class_index == 0 && exports[i].outer == 0 &&
            owner.NameText(exports[i].object_name) == class_name)
        {
            std::optional<std::string> parent;
            if (exports[i].super_index != 0)
            {
                parent = owner.FullPath(exports[i].super_index);
            }
            return parents_.emplace(class_path, std::move(parent)).first->second;
        }
    }
    intrinsic_.insert(class_path);
    return parents_.emplace(class_path, std::nullopt).first->second;
}

} // namespace gears::engine::object
