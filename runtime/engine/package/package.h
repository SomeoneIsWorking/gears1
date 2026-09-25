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
    // consistent package. `name` is the package's own name (its file stem),
    // the implicit outermost outer of every export.
    static Package Load(std::string name, std::span<const std::uint8_t> file);

    [[nodiscard]] const std::string &Name() const noexcept { return name_; }
    [[nodiscard]] const PackageSummary &Summary() const noexcept { return summary_; }
    [[nodiscard]] const ObjectTables &Tables() const noexcept { return tables_; }
    [[nodiscard]] std::span<const std::uint8_t> Bytes() const noexcept { return bytes_; }

    [[nodiscard]] bool HasName(const NameReference &name) const noexcept
    {
        return name.index < tables_.names.size();
    }
    // Refuses a reference outside the name table.
    [[nodiscard]] std::string NameText(const NameReference &name) const;
    // The dotted outer chain of an object, e.g. "Package.Group.Object".
    [[nodiscard]] std::string ObjectPath(PackageIndex index) const;
    // The name of the topmost object in an object's outer chain. In a cooked
    // level this is the original package an embedded object came from.
    [[nodiscard]] std::string OutermostName(PackageIndex index) const;
    // An object's path including its owning package, e.g. "Engine.Actor".
    [[nodiscard]] std::string FullPath(PackageIndex index) const;
    // The class name of an import or export; exports whose class index is zero
    // are classes themselves and report "Class".
    [[nodiscard]] std::string ClassName(PackageIndex index) const;
    [[nodiscard]] std::span<const std::uint8_t> ExportData(std::size_t export_index) const;

  private:
    Package(std::string name, std::vector<std::uint8_t> bytes, PackageSummary summary,
            ObjectTables tables)
        : name_(std::move(name)), bytes_(std::move(bytes)), summary_(std::move(summary)),
          tables_(std::move(tables))
    {
    }

    [[nodiscard]] const NameReference &ObjectName(PackageIndex index) const;
    [[nodiscard]] PackageIndex Outer(PackageIndex index) const;

    std::string name_;
    std::vector<std::uint8_t> bytes_;
    PackageSummary summary_;
    ObjectTables tables_;
};

} // namespace gears::engine::package
