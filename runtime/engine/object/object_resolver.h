#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <unordered_map>

#include "package/package.h"
#include "package/package_store.h"

namespace gears::engine::object
{

// An export located in a loaded package.
struct ExportLocation
{
    const package::Package *package = nullptr;
    std::size_t export_index = 0;
};

enum class ResolutionStatus : std::uint8_t
{
    // The reference is None.
    kNull,
    kFound,
    // An import whose package loads but no longer exports the object: the
    // cooker strips editor-only objects and leaves their imports behind.
    kCookedOut,
};

struct Resolution
{
    ResolutionStatus status = ResolutionStatus::kNull;
    // Valid when found.
    ExportLocation location;
};

// Resolves an object reference to the export that defines it: an export of
// the referencing package, or an import found by path in its own package.
class ObjectResolver
{
  public:
    explicit ObjectResolver(package::PackageStore &store) : store_(store) {}

    // Refuses an import that names a package rather than an object, or whose
    // package has no file.
    [[nodiscard]] Resolution Resolve(const package::Package &package, package::PackageIndex index);

  private:
    using ExportPaths = std::unordered_map<std::string, std::size_t>;

    const ExportPaths &PathsOf(const package::Package &package);

    package::PackageStore &store_;
    // Export object paths of packages the store owns (and so outlive this
    // resolver's lookups), built on first lookup.
    std::map<const package::Package *, ExportPaths> export_paths_;
};

} // namespace gears::engine::object
