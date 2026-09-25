#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <string_view>

#include "package/byte_reader.h"
#include "package/content_files.h"

namespace gears::engine::object
{

// Bulk-data flag: the payload lies in the raw file of the referencing
// object's outermost package, not inline.
inline constexpr std::uint32_t kBulkDataStoredInSeparateFile = 0x01U;
// Bulk-data flag: the entry has no payload anywhere; its counts are zero.
inline constexpr std::uint32_t kBulkDataUnused = 0x20U;
// Bulk-data flag: the payload is one LZO compressed record.
inline constexpr std::uint32_t kBulkDataCompressedLzo = 0x10U;

// A block of raw bytes stored beside an object's properties, such as a
// texture mip. An inline payload follows the header, and the header repeats
// its absolute position in the uncompressed package, which the reader checks.
// A separate-file payload lies at the header's offset in the raw file of the
// package named by the referencing object's outermost outer.
class BulkData
{
  public:
    // Reads one bulk-data header and its inline payload. `data_base` is the
    // absolute package offset of the reader's first byte. Refuses flags whose
    // storage this module does not decode.
    static BulkData Read(package::ByteReader &reader, std::size_t data_base);

    [[nodiscard]] std::uint32_t Flags() const noexcept { return flags_; }
    [[nodiscard]] std::size_t ElementCount() const noexcept { return element_count_; }
    [[nodiscard]] std::size_t StoredSize() const noexcept { return stored_size_; }
    [[nodiscard]] bool IsSeparate() const noexcept
    {
        return (flags_ & kBulkDataStoredInSeparateFile) != 0U;
    }

    // The payload as the object uses it: decompressed when stored compressed.
    // Every element this module reads is a single byte, so the payload is
    // `ElementCount()` bytes. A separate-file payload is read from `files`
    // under `outer_package`.
    [[nodiscard]] std::vector<std::uint8_t> Decode(package::ContentFiles &files,
                                                   std::string_view outer_package) const;

  private:
    BulkData(std::uint32_t flags, std::size_t element_count, std::size_t stored_size,
             std::size_t file_offset, std::span<const std::uint8_t> inline_bytes)
        : flags_(flags), element_count_(element_count), stored_size_(stored_size),
          file_offset_(file_offset), inline_bytes_(inline_bytes)
    {
    }

    std::uint32_t flags_;
    std::size_t element_count_;
    std::size_t stored_size_;
    std::size_t file_offset_;
    std::span<const std::uint8_t> inline_bytes_;
};

} // namespace gears::engine::object
