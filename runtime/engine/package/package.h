#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "object_tables.h"
#include "package_summary.h"

namespace gears::engine::package
{

// One cooked Gears 1 package, fully decompressed and indexed. Owns the
// uncompressed bytes so an export's serialized data stays addressable for the
// object loaders built on top of it.
class Package
{
  public:
    // Decodes a package file as it lies on the disc: whole-file compressed,
    // chunk compressed, or plain. Refuses anything that does not decode to a
    // consistent package.
    static Package Load(std::span<const std::uint8_t> file);

    [[nodiscard]] const PackageSummary &Summary() const noexcept { return summary_; }
    [[nodiscard]] const ObjectTables &Tables() const noexcept { return tables_; }
    [[nodiscard]] std::span<const std::uint8_t> Bytes() const noexcept { return bytes_; }

    [[nodiscard]] std::string NameText(const NameReference &name) const;
    // The dotted outer chain of an object, e.g. "Package.Group.Object".
    [[nodiscard]] std::string ObjectPath(PackageIndex index) const;
    // The class name of an import or export; exports whose class index is zero
    // are classes themselves and report "Class".
    [[nodiscard]] std::string ClassName(PackageIndex index) const;
    [[nodiscard]] std::span<const std::uint8_t> ExportData(std::size_t export_index) const;

  private:
    Package(std::vector<std::uint8_t> bytes, PackageSummary summary, ObjectTables tables)
        : bytes_(std::move(bytes)), summary_(std::move(summary)), tables_(std::move(tables))
    {
    }

    [[nodiscard]] const NameReference &ObjectName(PackageIndex index) const;
    [[nodiscard]] PackageIndex Outer(PackageIndex index) const;

    std::vector<std::uint8_t> bytes_;
    PackageSummary summary_;
    ObjectTables tables_;
};

} // namespace gears::engine::package
