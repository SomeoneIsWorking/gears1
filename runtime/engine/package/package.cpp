#include "package.h"

#include <algorithm>
#include <format>

#include "compressed_record.h"
#include "package_constants.h"

namespace gears::engine::package
{
namespace
{

bool IsWholeFileCompressed(std::span<const std::uint8_t> file)
{
    ByteReader reader(file, ByteOrder::Little);
    return file.size() >= 4U && reader.ReadU32() == kPackageTag;
}

std::vector<std::uint8_t> DecodeWholeFile(std::span<const std::uint8_t> file)
{
    std::vector<std::uint8_t> bytes(CompressedRecordUncompressedSize(file));
    std::size_t record_size = DecodeCompressedRecord(file, bytes);
    // The disc pads a compressed file to its sector multiple with zeros.
    std::span<const std::uint8_t> padding = file.subspan(record_size);
    if (!std::ranges::all_of(padding, [](std::uint8_t byte) { return byte == 0U; }))
    {
        throw PackageFormatError(
            std::format("whole-file record ends at {:#x} but non-zero bytes follow", record_size));
    }
    return bytes;
}

// Rebuilds the uncompressed package from its chunk records. The records tile
// the package after an uncompressed summary that carries no chunk table, so
// that summary is the file's prefix with a zero method and a zero chunk count.
std::vector<std::uint8_t> DecodeChunks(std::span<const std::uint8_t> file,
                                       const PackageSummary &summary)
{
    const CompressedChunk &last = summary.chunks.back();
    std::vector<std::uint8_t> bytes(last.uncompressed_offset + last.uncompressed_size);
    std::size_t expected_offset = summary.compression_field_offset + 8U;
    std::copy_n(file.begin(), summary.compression_field_offset, bytes.begin());
    for (const CompressedChunk &chunk : summary.chunks)
    {
        if (chunk.uncompressed_offset != expected_offset)
        {
            throw PackageFormatError(
                std::format("chunk starts at {:#x}; the previous ended at {:#x}",
                            chunk.uncompressed_offset, expected_offset));
        }
        if (chunk.compressed_offset > file.size() ||
            chunk.compressed_size > file.size() - chunk.compressed_offset)
        {
            throw PackageFormatError(std::format("chunk record {:#x}+{:#x} lies outside the file",
                                                 chunk.compressed_offset, chunk.compressed_size));
        }
        std::size_t consumed = DecodeCompressedRecord(
            file.subspan(chunk.compressed_offset, chunk.compressed_size),
            std::span(bytes).subspan(chunk.uncompressed_offset, chunk.uncompressed_size));
        if (consumed != chunk.compressed_size)
        {
            throw PackageFormatError(std::format("chunk record at {:#x} uses {} of its {} byte(s)",
                                                 chunk.compressed_offset, consumed,
                                                 chunk.compressed_size));
        }
        expected_offset += chunk.uncompressed_size;
    }
    return bytes;
}

// The disc pads packages to a sector multiple with zeros. The package ends at
// its header or its last export's data, whichever is later; anything after
// that must be padding.
void TrimPadding(std::vector<std::uint8_t> &bytes, const PackageSummary &summary,
                 const ObjectTables &tables)
{
    std::size_t end = summary.total_header_size;
    for (const ObjectExport &object : tables.exports)
    {
        end = std::max(end, object.serial_offset + object.serial_size);
    }
    if (end > bytes.size())
    {
        throw PackageFormatError(
            std::format("package data ends at {:#x} beyond its {:#x} byte(s)", end, bytes.size()));
    }
    auto padding = std::span(bytes).subspan(end);
    if (!std::ranges::all_of(padding, [](std::uint8_t byte) { return byte == 0U; }))
    {
        throw PackageFormatError(
            std::format("package data ends at {:#x} but non-zero bytes follow", end));
    }
    bytes.resize(end);
}

} // namespace

Package Package::Load(std::string name, std::span<const std::uint8_t> file)
{
    if (IsWholeFileCompressed(file))
    {
        std::vector<std::uint8_t> inner = DecodeWholeFile(file);
        return Load(std::move(name), inner);
    }
    ByteReader file_reader(file, ByteOrder::Big);
    PackageSummary file_summary = PackageSummary::Read(file_reader);
    std::vector<std::uint8_t> bytes = file_summary.chunks.empty()
                                          ? std::vector<std::uint8_t>(file.begin(), file.end())
                                          : DecodeChunks(file, file_summary);
    ByteReader reader(bytes, ByteOrder::Big);
    PackageSummary summary = PackageSummary::Read(reader);
    if (!summary.chunks.empty())
    {
        throw PackageFormatError("a decompressed package still declares compressed chunks");
    }
    ObjectTables tables = ObjectTables::Read(reader, summary);
    TrimPadding(bytes, summary, tables);
    return Package(std::move(name), std::move(bytes), std::move(summary), std::move(tables));
}

std::string Package::NameText(const NameReference &name) const
{
    if (name.index >= tables_.names.size())
    {
        throw PackageFormatError(
            std::format("name index {} is outside the {} names", name.index, tables_.names.size()));
    }
    const std::string &text = tables_.names[name.index].text;
    return name.number == 0U ? text : std::format("{}_{}", text, name.number - 1U);
}

const NameReference &Package::ObjectName(PackageIndex index) const
{
    return index > 0 ? tables_.exports.at(static_cast<std::size_t>(index) - 1U).object_name
                     : tables_.imports.at(static_cast<std::size_t>(-(index + 1))).object_name;
}

PackageIndex Package::Outer(PackageIndex index) const
{
    return index > 0 ? tables_.exports.at(static_cast<std::size_t>(index) - 1U).outer
                     : tables_.imports.at(static_cast<std::size_t>(-(index + 1))).outer;
}

std::string Package::ObjectPath(PackageIndex index) const
{
    if (index == 0)
    {
        return "None";
    }
    std::vector<PackageIndex> chain;
    for (PackageIndex current = index; current != 0; current = Outer(current))
    {
        if (chain.size() > tables_.imports.size() + tables_.exports.size())
        {
            throw PackageFormatError(std::format("object {} has a cyclic outer chain", index));
        }
        chain.push_back(current);
    }
    std::string path;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it)
    {
        path += path.empty() ? NameText(ObjectName(*it)) : "." + NameText(ObjectName(*it));
    }
    return path;
}

std::string Package::OutermostName(PackageIndex index) const
{
    std::string path = ObjectPath(index);
    return path.substr(0, path.find('.'));
}

std::string Package::FullPath(PackageIndex index) const
{
    PackageIndex outermost = index;
    for (std::size_t steps = 0; Outer(outermost) != 0; ++steps)
    {
        if (steps > tables_.imports.size() + tables_.exports.size())
        {
            throw PackageFormatError(std::format("object {} has a cyclic outer chain", index));
        }
        outermost = Outer(outermost);
    }
    return outermost > 0 ? name_ + "." + ObjectPath(index) : ObjectPath(index);
}

std::string Package::ClassName(PackageIndex index) const
{
    if (index == 0)
    {
        return "None";
    }
    if (index < 0)
    {
        return NameText(tables_.imports.at(static_cast<std::size_t>(-(index + 1))).class_name);
    }
    PackageIndex class_index = tables_.exports.at(static_cast<std::size_t>(index) - 1U).class_index;
    return class_index == 0 ? std::string("Class") : NameText(ObjectName(class_index));
}

std::span<const std::uint8_t> Package::ExportData(std::size_t export_index) const
{
    const ObjectExport &object = tables_.exports.at(export_index);
    return std::span(bytes_).subspan(object.serial_offset, object.serial_size);
}

} // namespace gears::engine::package
