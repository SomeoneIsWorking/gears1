#pragma once

#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>

#include "package/package.h"
#include "package/package_store.h"

namespace gears::engine::object
{

// Superclass chains of script classes, read from the class exports of the
// packages that define them and cached by full class path ("Engine.Actor").
class ClassHierarchy
{
  public:
    explicit ClassHierarchy(package::PackageStore &store) : store_(store) {}

    // The full path of the class of `package`'s object `index`.
    [[nodiscard]] static std::string ClassPath(const package::Package &package,
                                               package::PackageIndex index);

    // True when `class_path` is `ancestor` or derives from it. A class its
    // package does not export is intrinsic (declared only in native code); its
    // chain ends there. Refuses a class whose package cannot be loaded.
    [[nodiscard]] bool IsA(const std::string &class_path, std::string_view ancestor);

    // Intrinsic classes met so far, for reports.
    [[nodiscard]] const std::set<std::string, std::less<>> &IntrinsicClasses() const noexcept
    {
        return intrinsic_;
    }

  private:
    // The superclass path of a class, or none for a root class.
    const std::optional<std::string> &Parent(const std::string &class_path);

    package::PackageStore &store_;
    std::map<std::string, std::optional<std::string>, std::less<>> parents_;
    std::set<std::string, std::less<>> intrinsic_;
};

} // namespace gears::engine::object
