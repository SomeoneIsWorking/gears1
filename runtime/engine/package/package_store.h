#pragma once

#include <map>
#include <memory>
#include <string>
#include <string_view>

#include "content_files.h"
#include "package.h"

namespace gears::engine::package
{

// Decoded packages of one content directory by name, each loaded once and
// kept at a stable address for the store's lifetime.
class PackageStore
{
  public:
    explicit PackageStore(ContentFiles &files) : files_(files) {}

    // Refuses a name with no package file or a file that does not decode.
    const Package &Load(std::string_view name);

    [[nodiscard]] ContentFiles &Files() noexcept { return files_; }

  private:
    ContentFiles &files_;
    std::map<std::string, std::unique_ptr<Package>, std::less<>> packages_;
};

} // namespace gears::engine::package
