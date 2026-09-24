#include "package_summary.h"

#include <format>

#include "package_constants.h"

namespace gears::engine::package
{
namespace
{

TableLocation ReadTableLocation(ByteReader &reader)
{
    TableLocation location;
    location.count = reader.ReadCount(0);
    location.offset = reader.ReadU32();
    return location;
}

CompressionMethod ReadCompressionMethod(ByteReader &reader)
{
    std::uint32_t method = reader.ReadU32();
    switch (method)
    {
    case static_cast<std::uint32_t>(CompressionMethod::None):
        return CompressionMethod::None;
    case static_cast<std::uint32_t>(CompressionMethod::Lzo):
        return CompressionMethod::Lzo;
    default:
        reader.Fail(std::format("unsupported compression method {:#x}", method));
    }
}

} // namespace

PackageSummary PackageSummary::Read(ByteReader &reader)
{
    reader.Seek(0);
    if (reader.ReadU32() != kPackageTag)
    {
        reader.Fail("not a big-endian cooked package");
    }
    PackageSummary summary;
    std::uint32_t version = reader.ReadU32();
    summary.file_version = static_cast<std::uint16_t>(version & 0xFFFFU);
    summary.licensee_version = static_cast<std::uint16_t>(version >> 16U);
    if (summary.file_version != kGears1PackageFileVersion)
    {
        reader.Fail(std::format("package file version {} is not Gears 1's {}", summary.file_version,
                                kGears1PackageFileVersion));
    }
    summary.total_header_size = reader.ReadU32();
    summary.folder_name = reader.ReadString();
    summary.package_flags = reader.ReadU32();
    summary.names = ReadTableLocation(reader);
    summary.exports = ReadTableLocation(reader);
    summary.imports = ReadTableLocation(reader);
    std::span<const std::uint8_t> guid = reader.ReadBytes(summary.guid.size());
    std::copy(guid.begin(), guid.end(), summary.guid.begin());
    std::size_t generation_count = reader.ReadCount(12);
    summary.generations.reserve(generation_count);
    for (std::size_t i = 0; i < generation_count; ++i)
    {
        GenerationInfo generation;
        generation.export_count = reader.ReadU32();
        generation.name_count = reader.ReadU32();
        generation.net_object_count = reader.ReadU32();
        summary.generations.push_back(generation);
    }
    summary.engine_version = reader.ReadU32();
    summary.cooker_version = reader.ReadU32();
    summary.compression_field_offset = reader.Offset();
    summary.compression = ReadCompressionMethod(reader);
    std::size_t chunk_count = reader.ReadCount(16);
    summary.chunks.reserve(chunk_count);
    for (std::size_t i = 0; i < chunk_count; ++i)
    {
        CompressedChunk chunk;
        chunk.uncompressed_offset = reader.ReadU32();
        chunk.uncompressed_size = reader.ReadU32();
        chunk.compressed_offset = reader.ReadU32();
        chunk.compressed_size = reader.ReadU32();
        summary.chunks.push_back(chunk);
    }
    if (summary.chunks.empty() != (summary.compression == CompressionMethod::None))
    {
        reader.Fail("compression method and chunk table disagree");
    }
    summary.end_offset = reader.Offset();
    return summary;
}

} // namespace gears::engine::package
