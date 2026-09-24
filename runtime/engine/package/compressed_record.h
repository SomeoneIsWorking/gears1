#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace gears::engine::package
{

// Decodes one compressed record: a tag whose byte order also fixes the order
// of the record's own header, the block size, the compressed and uncompressed
// totals, one size pair per block, and the LZO1X blocks. `output` must be the
// record's declared uncompressed size. Returns the record's length in bytes.
// Whole-file compression stores the header little-endian; a record inside a
// package's chunk table stores it big-endian.
std::size_t DecodeCompressedRecord(std::span<const std::uint8_t> record,
                                   std::span<std::uint8_t> output);

// The uncompressed size a record declares, read without decoding it.
std::size_t CompressedRecordUncompressedSize(std::span<const std::uint8_t> record);

} // namespace gears::engine::package
