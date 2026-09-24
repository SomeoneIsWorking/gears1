#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "byte_reader.h"
#include "package_summary.h"

namespace gears::engine::package
{

// A name as a package stores it: a name-table index and an instance number
// whose non-zero value N renders as the suffix "_<N-1>".
struct NameReference
{
    std::uint32_t index = 0;
    std::uint32_t number = 0;
};

struct NameEntry
{
    std::string text;
    std::uint64_t flags = 0;
};

// Positive values select export value-1, negative values import -value-1,
// and zero is no object.
using PackageIndex = std::int32_t;

struct ObjectImport
{
    NameReference class_package;
    NameReference class_name;
    PackageIndex outer = 0;
    NameReference object_name;
};

struct ObjectExport
{
    PackageIndex class_index = 0;
    PackageIndex super_index = 0;
    PackageIndex outer = 0;
    NameReference object_name;
    PackageIndex archetype = 0;
    std::uint64_t object_flags = 0;
    std::size_t serial_size = 0;
    std::size_t serial_offset = 0;
    std::vector<std::pair<NameReference, PackageIndex>> components;
    std::uint32_t export_flags = 0;
    std::vector<std::uint32_t> net_object_counts;
    std::array<std::uint8_t, 16> package_guid{};
};

struct ObjectTables
{
    std::vector<NameEntry> names;
    std::vector<ObjectImport> imports;
    std::vector<ObjectExport> exports;

    // Reads the three tables the summary locates in the uncompressed package
    // and validates every name and object reference inside them.
    static ObjectTables Read(ByteReader &reader, const PackageSummary &summary);
};

} // namespace gears::engine::package
