#include "compressed_record.h"

#include <algorithm>
#include <format>

#include "byte_reader.h"
#include "lzo1x.h"
#include "package_constants.h"

namespace gears::engine::package
{
namespace
{

// The tag's byte order is the header's byte order. Returns a reader positioned
// after the tag.
ByteReader OpenRecord(std::span<const std::uint8_t> record)
{
    for (const ByteOrder order : {ByteOrder::Big, ByteOrder::Little})
    {
        ByteReader reader(record, order);
        if (reader.ReadU32() == kPackageTag)
        {
            return reader;
        }
    }
    ByteReader(record, ByteOrder::Big)
        .Fail("compressed record does not start with the package tag");
}

struct RecordHeader
{
    std::size_t block_size = 0;
    std::size_t compressed_size = 0;
    std::size_t uncompressed_size = 0;
};

RecordHeader ReadHeader(ByteReader &reader)
{
    RecordHeader header;
    std::uint32_t block_size = reader.ReadU32();
    // A block size equal to the tag marks the format's fixed default.
    header.block_size = block_size == kPackageTag ? kDefaultCompressionBlockSize : block_size;
    if (header.block_size == 0U)
    {
        reader.Fail("zero compression block size");
    }
    header.compressed_size = reader.ReadU32();
    header.uncompressed_size = reader.ReadU32();
    return header;
}

} // namespace

std::size_t CompressedRecordUncompressedSize(std::span<const std::uint8_t> record)
{
    ByteReader reader = OpenRecord(record);
    return ReadHeader(reader).uncompressed_size;
}

std::size_t DecodeCompressedRecord(std::span<const std::uint8_t> record,
                                   std::span<std::uint8_t> output)
{
    ByteReader reader = OpenRecord(record);
    RecordHeader header = ReadHeader(reader);
    if (header.uncompressed_size != output.size())
    {
        reader.Fail(std::format("record declares {} uncompressed byte(s); the caller expects {}",
                                header.uncompressed_size, output.size()));
    }
    std::size_t block_count =
        (header.uncompressed_size + header.block_size - 1U) / header.block_size;
    if (block_count > reader.Remaining() / 8U)
    {
        reader.Fail(std::format("{} block size pairs cannot fit in the record", block_count));
    }
    std::size_t table_offset = reader.Offset();
    std::size_t data_offset = table_offset + block_count * 8U;
    std::size_t output_offset = 0;
    std::size_t compressed_total = 0;
    for (std::size_t block = 0; block < block_count; ++block)
    {
        reader.Seek(table_offset + block * 8U);
        std::size_t compressed = reader.ReadU32();
        std::size_t uncompressed = reader.ReadU32();
        std::size_t expected =
            std::min(header.block_size, header.uncompressed_size - output_offset);
        if (uncompressed != expected)
        {
            reader.Fail(std::format("block {} declares {} byte(s); the block layout requires {}",
                                    block, uncompressed, expected));
        }
        reader.Seek(data_offset);
        DecodeLzo1x(reader.ReadBytes(compressed), output.subspan(output_offset, uncompressed));
        data_offset += compressed;
        output_offset += uncompressed;
        compressed_total += compressed;
    }
    if (compressed_total != header.compressed_size)
    {
        reader.Fail(std::format("blocks hold {} compressed byte(s); the record declares {}",
                                compressed_total, header.compressed_size));
    }
    return data_offset;
}

} // namespace gears::engine::package
