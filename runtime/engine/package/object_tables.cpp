#include "object_tables.h"

#include <algorithm>
#include <format>

#include "package_constants.h"

namespace gears::engine::package
{
namespace
{

constexpr std::size_t kImportSize = 3U * kNameReferenceSize + 4U;
constexpr std::size_t kMinimumExportSize = 68U;

NameReference ReadName(ByteReader &reader)
{
    NameReference name;
    name.index = reader.ReadU32();
    name.number = reader.ReadU32();
    return name;
}

std::vector<NameEntry> ReadNames(ByteReader &reader, const TableLocation &location)
{
    reader.Seek(location.offset);
    std::vector<NameEntry> names;
    names.reserve(std::min(location.count, reader.Remaining() / 12U));
    for (std::size_t i = 0; i < location.count; ++i)
    {
        NameEntry entry;
        entry.text = reader.ReadString();
        entry.flags = reader.ReadU64();
        names.push_back(std::move(entry));
    }
    return names;
}

std::vector<ObjectImport> ReadImports(ByteReader &reader, const TableLocation &location)
{
    reader.Seek(location.offset);
    if (location.count > reader.Remaining() / kImportSize)
    {
        reader.Fail(std::format("{} imports cannot fit in the package", location.count));
    }
    std::vector<ObjectImport> imports(location.count);
    for (ObjectImport &import : imports)
    {
        import.class_package = ReadName(reader);
        import.class_name = ReadName(reader);
        import.outer = reader.ReadI32();
        import.object_name = ReadName(reader);
    }
    return imports;
}

ObjectExport ReadExport(ByteReader &reader)
{
    ObjectExport object;
    object.class_index = reader.ReadI32();
    object.super_index = reader.ReadI32();
    object.outer = reader.ReadI32();
    object.object_name = ReadName(reader);
    object.archetype = reader.ReadI32();
    object.object_flags = reader.ReadU64();
    object.serial_size = reader.ReadU32();
    object.serial_offset = reader.ReadU32();
    std::size_t component_count = reader.ReadCount(kNameReferenceSize + 4U);
    object.components.reserve(component_count);
    for (std::size_t i = 0; i < component_count; ++i)
    {
        NameReference name = ReadName(reader);
        object.components.emplace_back(name, reader.ReadI32());
    }
    object.export_flags = reader.ReadU32();
    std::size_t net_count = reader.ReadCount(4U);
    object.net_object_counts.reserve(net_count);
    for (std::size_t i = 0; i < net_count; ++i)
    {
        object.net_object_counts.push_back(reader.ReadU32());
    }
    std::span<const std::uint8_t> guid = reader.ReadBytes(object.package_guid.size());
    std::copy(guid.begin(), guid.end(), object.package_guid.begin());
    return object;
}

std::vector<ObjectExport> ReadExports(ByteReader &reader, const TableLocation &location)
{
    reader.Seek(location.offset);
    if (location.count > reader.Remaining() / kMinimumExportSize)
    {
        reader.Fail(std::format("{} exports cannot fit in the package", location.count));
    }
    std::vector<ObjectExport> exports;
    exports.reserve(location.count);
    for (std::size_t i = 0; i < location.count; ++i)
    {
        exports.push_back(ReadExport(reader));
    }
    return exports;
}

class ReferenceValidator
{
  public:
    ReferenceValidator(const ByteReader &reader, const ObjectTables &tables)
        : reader_(reader), tables_(tables)
    {
    }

    void Validate() const
    {
        for (std::size_t i = 0; i < tables_.imports.size(); ++i)
        {
            const ObjectImport &import = tables_.imports[i];
            std::string where = std::format("import {}", i);
            Name(import.class_package, where);
            Name(import.class_name, where);
            Name(import.object_name, where);
            Object(import.outer, where);
        }
        for (std::size_t i = 0; i < tables_.exports.size(); ++i)
        {
            const ObjectExport &object = tables_.exports[i];
            std::string where = std::format("export {}", i);
            Name(object.object_name, where);
            Object(object.class_index, where);
            Object(object.super_index, where);
            Object(object.outer, where);
            Object(object.archetype, where);
            for (const auto &[name, component] : object.components)
            {
                Name(name, where);
                Object(component, where);
            }
            if (object.serial_size > reader_.Size() ||
                object.serial_offset > reader_.Size() - object.serial_size)
            {
                reader_.Fail(std::format("{} serial range {:#x}+{:#x} lies outside the package",
                                         where, object.serial_offset, object.serial_size));
            }
        }
    }

  private:
    void Name(const NameReference &name, const std::string &where) const
    {
        if (name.index >= tables_.names.size())
        {
            reader_.Fail(
                std::format("{} names entry {} of {}", where, name.index, tables_.names.size()));
        }
    }

    void Object(PackageIndex index, const std::string &where) const
    {
        bool valid = index == 0 ||
                     (index > 0 && static_cast<std::size_t>(index) <= tables_.exports.size()) ||
                     (index < 0 && static_cast<std::size_t>(-static_cast<std::int64_t>(index)) <=
                                       tables_.imports.size());
        if (!valid)
        {
            reader_.Fail(std::format("{} references object {} outside {} imports and {} exports",
                                     where, index, tables_.imports.size(), tables_.exports.size()));
        }
    }

    const ByteReader &reader_;
    const ObjectTables &tables_;
};

} // namespace

ObjectTables ObjectTables::Read(ByteReader &reader, const PackageSummary &summary)
{
    ObjectTables tables;
    tables.names = ReadNames(reader, summary.names);
    tables.imports = ReadImports(reader, summary.imports);
    tables.exports = ReadExports(reader, summary.exports);
    ReferenceValidator(reader, tables).Validate();
    return tables;
}

} // namespace gears::engine::package
