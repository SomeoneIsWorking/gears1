#include "object_resolver.h"

#include <format>

#include "package/byte_reader.h"

namespace gears::engine::object
{

Resolution ObjectResolver::Resolve(const package::Package &package, package::PackageIndex index)
{
    if (index == 0)
    {
        return {};
    }
    if (index > 0)
    {
        return {ResolutionStatus::kFound, {&package, static_cast<std::size_t>(index) - 1U}};
    }
    std::string path = package.FullPath(index);
    std::size_t dot = path.find('.');
    if (dot == std::string::npos)
    {
        throw package::PackageFormatError(
            std::format("import '{}' is a package, not an object", path));
    }
    const package::Package &owner = store_.Load(path.substr(0, dot));
    const ExportPaths &paths = PathsOf(owner);
    auto found = paths.find(path.substr(dot + 1U));
    if (found == paths.end())
    {
        return {ResolutionStatus::kCookedOut, {}};
    }
    return {ResolutionStatus::kFound, {&owner, found->second}};
}

std::optional<ExportLocation> ObjectResolver::Find(std::string_view package_name,
                                                   std::string_view object_path)
{
    const package::Package &owner = store_.Load(package_name);
    const ExportPaths &paths = PathsOf(owner);
    auto found = paths.find(std::string(object_path));
    if (found == paths.end())
    {
        return std::nullopt;
    }
    return ExportLocation{&owner, found->second};
}

const ObjectResolver::ExportPaths &ObjectResolver::PathsOf(const package::Package &package)
{
    auto [entry, inserted] = export_paths_.try_emplace(&package);
    if (inserted)
    {
        for (std::size_t i = 0; i < package.Tables().exports.size(); ++i)
        {
            entry->second.emplace(package.ObjectPath(static_cast<package::PackageIndex>(i + 1U)),
                                  i);
        }
    }
    return entry->second;
}

} // namespace gears::engine::object
