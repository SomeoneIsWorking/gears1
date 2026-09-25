#include "bulk_data.h"

#include <format>

#include "package/compressed_record.h"

namespace gears::engine::object
{

BulkData BulkData::Read(package::ByteReader &reader, std::size_t data_base)
{
    std::uint32_t flags = reader.ReadU32();
    if ((flags & ~(kBulkDataCompressedLzo | kBulkDataStoredInSeparateFile | kBulkDataUnused)) != 0U)
    {
        reader.Fail(std::format("bulk data flags {:#x} include storage this reader does not "
                                "decode",
                                flags));
    }
    std::size_t element_count = reader.ReadCount(0);
    if ((flags & kBulkDataUnused) != 0U)
    {
        // An unused entry stores -1 for both its size and its offset.
        std::int32_t stored = reader.ReadI32();
        std::int32_t offset = reader.ReadI32();
        if (element_count != 0U || stored != -1 || offset != -1)
        {
            reader.Fail(std::format("unused bulk data declares {} element(s), size {}, offset {}",
                                    element_count, stored, offset));
        }
        return {flags, 0U, 0U, 0U, {}};
    }
    std::size_t stored_size = reader.ReadCount(0);
    std::size_t file_offset = reader.ReadU32();
    if ((flags & kBulkDataCompressedLzo) == 0U && stored_size != element_count)
    {
        reader.Fail(std::format("uncompressed bulk data stores {} byte(s) for {} element(s)",
                                stored_size, element_count));
    }
    if ((flags & kBulkDataStoredInSeparateFile) != 0U)
    {
        return {flags, element_count, stored_size, file_offset, {}};
    }
    if (file_offset != data_base + reader.Offset())
    {
        reader.Fail(std::format("bulk data claims offset {:#x} but lies at {:#x}", file_offset,
                                data_base + reader.Offset()));
    }
    return {flags, element_count, stored_size, file_offset, reader.ReadBytes(stored_size)};
}

std::vector<std::uint8_t> BulkData::Decode(package::ContentFiles &files,
                                           std::string_view outer_package) const
{
    std::span<const std::uint8_t> stored =
        IsSeparate() ? files.Range(outer_package, file_offset_, stored_size_) : inline_bytes_;
    if ((flags_ & kBulkDataCompressedLzo) == 0U)
    {
        return {stored.begin(), stored.end()};
    }
    std::vector<std::uint8_t> bytes(element_count_);
    std::size_t consumed = package::DecodeCompressedRecord(stored, bytes);
    if (consumed != stored.size())
    {
        throw package::PackageFormatError(std::format(
            "compressed bulk data uses {} of its {} stored byte(s)", consumed, stored.size()));
    }
    return bytes;
}

} // namespace gears::engine::object
