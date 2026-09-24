#pragma once

#include <cstddef>
#include <cstdint>

namespace gears::engine::package
{

// First word of every cooked package and of every compressed record.
inline constexpr std::uint32_t kPackageTag = 0x9E2A83C1U;

// The package file version Gears of War (2006) cooks with. The summary and
// table layouts in this module were measured on that version and refuse any
// other rather than guessing at a neighbour's layout.
inline constexpr std::uint16_t kGears1PackageFileVersion = 374U;

// Block size of a compressed record whose block-size field repeats the tag.
inline constexpr std::size_t kDefaultCompressionBlockSize = 0x20000U;

// Encoded size of a name reference: name-table index plus instance number.
inline constexpr std::size_t kNameReferenceSize = 8U;

} // namespace gears::engine::package
