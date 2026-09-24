#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "byte_reader.h"

namespace gears::engine::package
{

struct TableLocation
{
    std::size_t count = 0;
    std::size_t offset = 0;
};

struct GenerationInfo
{
    std::uint32_t export_count = 0;
    std::uint32_t name_count = 0;
    std::uint32_t net_object_count = 0;
};

enum class CompressionMethod : std::uint32_t
{
    None = 0,
    Lzo = 2,
};

// One range of the uncompressed package held as a compressed record.
struct CompressedChunk
{
    std::size_t uncompressed_offset = 0;
    std::size_t uncompressed_size = 0;
    std::size_t compressed_offset = 0;
    std::size_t compressed_size = 0;
};

// The package header that precedes the name table, as Gears 1 cooks it.
struct PackageSummary
{
    std::uint16_t file_version = 0;
    std::uint16_t licensee_version = 0;
    std::size_t total_header_size = 0;
    std::string folder_name;
    std::uint32_t package_flags = 0;
    TableLocation names;
    TableLocation exports;
    TableLocation imports;
    std::array<std::uint8_t, 16> guid{};
    std::vector<GenerationInfo> generations;
    std::uint32_t engine_version = 0;
    std::uint32_t cooker_version = 0;
    CompressionMethod compression = CompressionMethod::None;
    std::vector<CompressedChunk> chunks;
    // Offset of the compression method field. The uncompressed package a
    // chunk table describes ends its summary eight bytes later, with no
    // method and no chunks.
    std::size_t compression_field_offset = 0;
    // Offset of the first byte after the summary.
    std::size_t end_offset = 0;

    // Reads a big-endian summary from the reader's start; refuses a version
    // other than Gears 1's and a compression method this module cannot decode.
    static PackageSummary Read(ByteReader &reader);
};

} // namespace gears::engine::package
