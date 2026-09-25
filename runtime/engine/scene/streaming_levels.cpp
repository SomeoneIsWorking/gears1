#include "streaming_levels.h"

#include <cstddef>
#include <format>
#include <optional>
#include <string_view>

#include "object/property_values.h"
#include "object/serialized_object.h"

namespace gears::engine::scene
{
namespace
{

constexpr std::string_view kWorldInfoClass = "Engine.WorldInfo";

std::size_t WorldInfoOf(const package::Package &level, object::ClassHierarchy &classes)
{
    const auto &exports = level.Tables().exports;
    std::optional<std::size_t> found;
    for (std::size_t i = 0; i < exports.size(); ++i)
    {
        auto index = static_cast<package::PackageIndex>(i + 1U);
        if ((exports[i].object_flags & object::kObjectFlagClassDefaultObject) != 0U ||
            object::IsInsideClassDefaults(level, i) ||
            !classes.IsA(object::ClassHierarchy::ClassPath(level, index), kWorldInfoClass))
        {
            continue;
        }
        if (found)
        {
            throw package::PackageFormatError(
                std::format("{} has more than one WorldInfo", level.Name()));
        }
        found = i;
    }
    if (!found)
    {
        throw package::PackageFormatError(std::format("{} has no WorldInfo", level.Name()));
    }
    return *found;
}

} // namespace

std::vector<std::string> StreamingLevelPackages(const package::Package &level,
                                                object::ClassHierarchy &classes)
{
    auto world_info = object::SerializedObject::Read(level, WorldInfoOf(level, classes), classes);
    std::vector<package::PackageIndex> entries =
        object::PropertyValues(world_info.Properties()).ObjectArray("StreamingLevels");
    std::vector<std::string> packages;
    packages.reserve(entries.size());
    for (package::PackageIndex entry : entries)
    {
        if (entry <= 0 || static_cast<std::size_t>(entry) > level.Tables().exports.size())
        {
            throw package::PackageFormatError(std::format(
                "{} streams level {} that is not one of its exports", level.Name(), entry));
        }
        auto streaming =
            object::SerializedObject::Read(level, static_cast<std::size_t>(entry) - 1U, classes);
        std::optional<std::string> name =
            object::PropertyValues(streaming.Properties()).Name("PackageName");
        if (!name)
        {
            throw package::PackageFormatError(
                std::format("{} streams level {} with no package name", level.Name(), entry));
        }
        packages.push_back(std::move(*name));
    }
    return packages;
}

} // namespace gears::engine::scene
