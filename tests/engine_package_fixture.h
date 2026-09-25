#pragma once

// Builders of synthetic cooked packages for the native engine's tests.

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string_view>
#include <vector>

#include "package/package_constants.h"

namespace gears::engine::test
{

using Bytes = std::vector<std::uint8_t>;

// Big-endian package bytes.
class Writer
{
  public:
    void U32(std::uint32_t value)
    {
        for (int shift = 24; shift >= 0; shift -= 8)
        {
            bytes.push_back(static_cast<std::uint8_t>(value >> static_cast<unsigned>(shift)));
        }
    }
    void U32Little(std::uint32_t value)
    {
        for (unsigned shift = 0; shift < 32U; shift += 8U)
        {
            bytes.push_back(static_cast<std::uint8_t>(value >> shift));
        }
    }
    void String(std::string_view text)
    {
        U32(static_cast<std::uint32_t>(text.size() + 1U));
        bytes.insert(bytes.end(), text.begin(), text.end());
        bytes.push_back(0);
    }
    void Name(std::uint32_t index, std::uint32_t number = 0)
    {
        U32(index);
        U32(number);
    }
    void Zeros(std::size_t count) { bytes.insert(bytes.end(), count, 0U); }
    void Patch(std::size_t offset, std::uint32_t value)
    {
        for (std::size_t i = 0; i < 4U; ++i)
        {
            bytes[offset + i] = static_cast<std::uint8_t>(value >> (24U - 8U * i));
        }
    }

    Bytes bytes;
};

inline constexpr std::size_t kSummaryNamesField = 0x19;
inline constexpr std::size_t kSummaryExportsField = 0x21;
inline constexpr std::size_t kSummaryImportsField = 0x29;
inline constexpr std::size_t kSummaryHeaderSizeField = 0x08;
inline constexpr std::array<std::uint8_t, 4> kExportPayload{0xDE, 0xAD, 0xBE, 0xEF};

// The uncompressed summary ends here; a chunked file's table starts here.
inline constexpr std::size_t kChunkTableField = 0x61;

// An uncompressed package: names Core, Package, Thing, then `extra_names`;
// import 0 is the Core package; export "Thing_2" is an instance of that
// import with a four-byte payload.
inline Bytes PlainPackage(std::uint32_t version = package::kGears1PackageFileVersion,
                          std::uint32_t export_name = 2,
                          std::initializer_list<std::string_view> extra_names = {})
{
    Writer w;
    w.U32(package::kPackageTag);
    w.U32(version);
    w.U32(0); // header size, patched
    w.String("None");
    w.U32(0x00080009U);
    w.Zeros(24); // name/export/import locations, patched
    w.Zeros(16); // guid
    w.U32(1);
    w.U32(1);
    w.U32(3);
    w.U32(0);
    w.U32(2451);
    w.U32(32);
    w.U32(0); // no compression
    w.U32(0); // no chunks
    w.Patch(kSummaryNamesField, static_cast<std::uint32_t>(3U + extra_names.size()));
    w.Patch(kSummaryNamesField + 4, static_cast<std::uint32_t>(w.bytes.size()));
    for (std::string_view name : {"Core", "Package", "Thing"})
    {
        w.String(name);
        w.U32(0x00070010U);
        w.U32(0);
    }
    for (std::string_view name : extra_names)
    {
        w.String(name);
        w.U32(0x00070010U);
        w.U32(0);
    }
    w.Patch(kSummaryImportsField, 1);
    w.Patch(kSummaryImportsField + 4, static_cast<std::uint32_t>(w.bytes.size()));
    w.Name(0);
    w.Name(1);
    w.U32(0);
    w.Name(0);
    w.Patch(kSummaryExportsField, 1);
    w.Patch(kSummaryExportsField + 4, static_cast<std::uint32_t>(w.bytes.size()));
    w.U32(0xFFFFFFFFU); // class: import 0
    w.U32(0);
    w.U32(0);
    w.Name(export_name, 3);
    w.U32(0);
    w.U32(0x000F0004U);
    w.U32(0);
    w.U32(static_cast<std::uint32_t>(kExportPayload.size()));
    std::size_t serial_offset_field = w.bytes.size();
    w.U32(0);
    w.U32(0);
    w.U32(0);
    w.U32(0);
    w.Zeros(16);
    auto header_size = static_cast<std::uint32_t>(w.bytes.size());
    w.Patch(kSummaryHeaderSizeField, header_size);
    w.Patch(serial_offset_field, header_size);
    w.bytes.insert(w.bytes.end(), kExportPayload.begin(), kExportPayload.end());
    return w.bytes;
}

} // namespace gears::engine::test
