#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace gears::engine::package
{

// The whole of one raw package file, read with a single sized read. Refuses
// a file that cannot be opened or is shorter than its reported size.
[[nodiscard]] std::vector<std::uint8_t> ReadPackageFile(const std::filesystem::path &path);

// The cooked content directory of one title: raw package files by package
// name, read on first use and kept for the owner's lifetime. Bulk data stored
// outside the package that references it is read through here.
class ContentFiles
{
  public:
    // Refuses a path that is not a directory.
    explicit ContentFiles(std::filesystem::path directory);

    [[nodiscard]] const std::filesystem::path &Directory() const noexcept { return directory_; }

    // The raw bytes of `<package>.xxx`, matched without regard to case.
    // Refuses a package with no file.
    [[nodiscard]] std::span<const std::uint8_t> File(std::string_view package);

    // `size` bytes at `offset` of a package's raw file; refuses a range
    // outside it.
    [[nodiscard]] std::span<const std::uint8_t> Range(std::string_view package, std::size_t offset,
                                                      std::size_t size);

  private:
    std::filesystem::path directory_;
    // Lower-cased package name to its file path, from one directory scan.
    std::map<std::string, std::filesystem::path> paths_;
    std::map<std::string, std::vector<std::uint8_t>> loaded_;
};

} // namespace gears::engine::package
