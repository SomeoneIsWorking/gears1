// Prints a cooked package's exports, or the leading bytes of the exports of
// one class, through the native engine's package layer. A maintainer tool for
// measuring object layouts; it reads only the given file.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <span>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <lucent/log.h>

#include "object/serialized_object.h"
#include "package/content_files.h"
#include "package/package.h"
#include "package/package_store.h"

namespace
{

using gears::engine::package::Package;

std::string HexLine(std::span<const std::uint8_t> bytes)
{
    std::string line;
    for (std::uint8_t byte : bytes)
    {
        line += std::format("{:02x} ", byte);
    }
    return line;
}

void DumpProperties(const Package &package, std::size_t index,
                    gears::engine::object::ClassHierarchy &classes)
{
    if (gears::engine::object::IsSchemaExport(package, index))
    {
        return;
    }
    auto object = gears::engine::object::SerializedObject::Read(package, index, classes);
    for (const auto &tag : object.Properties())
    {
        std::string type = package.NameText(tag.type);
        if (type == "StructProperty")
        {
            type += " " + package.NameText(tag.struct_name);
        }
        lucent::info("package-inspect", "    {}[{}] {} {} {}", package.NameText(tag.name),
                     tag.array_index, type, tag.value.size(),
                     HexLine(tag.value.first(std::min<std::size_t>(tag.value.size(), 16U))));
    }
    lucent::info("package-inspect", "    native data at {:#x}: {} byte(s)", object.NativeOffset(),
                 object.NativeData().size());
}

void DumpExport(const Package &package, std::size_t index, std::size_t byte_limit,
                std::size_t byte_offset)
{
    auto index_value = static_cast<std::int32_t>(index + 1U);
    std::span<const std::uint8_t> data = package.ExportData(index);
    lucent::info("package-inspect", "export {} {} '{}' flags {:#018x} size {} at {:#x}", index + 1U,
                 package.ClassName(index_value), package.ObjectPath(index_value),
                 package.Tables().exports[index].object_flags, data.size(),
                 package.Tables().exports[index].serial_offset);
    std::size_t shown = std::min(data.size(), byte_offset + byte_limit);
    for (std::size_t offset = std::min(byte_offset, shown); offset < shown; offset += 32U)
    {
        lucent::info("package-inspect", "  {:6x}: {}", offset,
                     HexLine(data.subspan(offset, std::min<std::size_t>(32U, shown - offset))));
    }
}

struct Selection
{
    std::string_view class_filter;
    std::size_t limit = 50U;
    std::size_t byte_limit = 128U;
    // Only exports whose object path contains this text.
    std::string_view path_filter;
    std::size_t byte_offset = 0;
};

int Run(const std::filesystem::path &path, const Selection &selection)
{
    std::string_view class_filter = selection.class_filter;
    gears::engine::package::ContentFiles files(path.parent_path());
    gears::engine::package::PackageStore store(files);
    gears::engine::object::ClassHierarchy classes(store);
    const Package &package = store.Load(path.stem().string());
    lucent::info("package-inspect", "{}: {} names, {} imports, {} exports",
                 path.filename().string(), package.Tables().names.size(),
                 package.Tables().imports.size(), package.Tables().exports.size());
    std::size_t matched = 0;
    for (std::size_t i = 0; i < package.Tables().exports.size() && matched < selection.limit; ++i)
    {
        auto index = static_cast<std::int32_t>(i + 1U);
        if ((class_filter.empty() || package.ClassName(index) == class_filter) &&
            package.ObjectPath(index).find(selection.path_filter) != std::string::npos)
        {
            DumpExport(package, i, class_filter.empty() ? 0U : selection.byte_limit,
                       selection.byte_offset);
            if (!class_filter.empty())
            {
                DumpProperties(package, i, classes);
            }
            ++matched;
        }
    }
    lucent::info("package-inspect", "{} of {} export(s) matched class '{}'", matched,
                 package.Tables().exports.size(), class_filter);
    return matched == 0U ? 1 : 0;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 7)
    {
        lucent::error("package-inspect", "usage: gears_package_inspect <package> [class] [count] "
                                         "[bytes] [path filter] [byte offset]");
        return 2;
    }
    Selection selection;
    selection.class_filter = argc >= 3 ? argv[2] : "";
    selection.limit = argc >= 4 ? std::stoul(argv[3]) : selection.limit;
    selection.byte_limit = argc >= 5 ? std::stoul(argv[4]) : selection.byte_limit;
    selection.path_filter = argc >= 6 ? argv[5] : "";
    selection.byte_offset = argc >= 7 ? std::stoul(argv[6], nullptr, 0) : 0U;
    return Run(argv[1], selection);
}
