// Loads every cooked package in a directory through the native engine's
// package layer and reports what it decoded. A package that fails to decode
// fails the census by name; an empty or missing directory is a refusal, not a
// clean result.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <lucent/log.h>

#include "package/lzo1x.h"
#include "package/package.h"

namespace
{

namespace fs = std::filesystem;
using gears::engine::package::Package;

std::vector<std::uint8_t> ReadFile(const fs::path &path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
    {
        throw std::runtime_error("cannot open " + path.filename().string());
    }
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

// Export serial data must tile the package after the header: sorted by
// offset, each range starts where the previous ended and the last ends at the
// package end. A layout misread shifts these ranges and breaks the tiling.
std::string CheckExportTiling(const Package &package)
{
    std::vector<std::pair<std::size_t, std::size_t>> ranges;
    for (const auto &object : package.Tables().exports)
    {
        if (object.serial_size != 0U)
        {
            ranges.emplace_back(object.serial_offset, object.serial_size);
        }
    }
    std::ranges::sort(ranges);
    std::size_t cursor = package.Summary().total_header_size;
    for (const auto &[offset, size] : ranges)
    {
        if (offset != cursor)
        {
            return "export data at " + std::to_string(offset) + " follows " +
                   std::to_string(cursor);
        }
        cursor += size;
    }
    if (cursor != package.Bytes().size())
    {
        return "export data ends at " + std::to_string(cursor) + " of " +
               std::to_string(package.Bytes().size());
    }
    return {};
}

struct Census
{
    std::size_t packages = 0;
    std::size_t failed = 0;
    std::size_t names = 0;
    std::size_t imports = 0;
    std::size_t exports = 0;
    std::size_t uncompressed_bytes = 0;
    std::map<std::string, std::size_t> export_classes;
};

void LoadOne(const fs::path &path, Census &census)
{
    ++census.packages;
    try
    {
        Package package = Package::Load(ReadFile(path));
        std::string tiling = CheckExportTiling(package);
        if (!tiling.empty())
        {
            ++census.failed;
            lucent::error("package-census", "{}: {}", path.filename().string(), tiling);
            return;
        }
        census.names += package.Tables().names.size();
        census.imports += package.Tables().imports.size();
        census.exports += package.Tables().exports.size();
        census.uncompressed_bytes += package.Bytes().size();
        for (std::size_t i = 0; i < package.Tables().exports.size(); ++i)
        {
            ++census.export_classes[package.ClassName(static_cast<std::int32_t>(i + 1U))];
        }
    }
    // The census boundary: a format refusal is this package's result, and the
    // next file is independent of it.
    catch (const gears::engine::package::PackageFormatError &error)
    {
        ++census.failed;
        lucent::error("package-census", "{}: {}", path.filename().string(), error.what());
    }
    catch (const gears::engine::package::DecompressionError &error)
    {
        ++census.failed;
        lucent::error("package-census", "{}: {}", path.filename().string(), error.what());
    }
}

int Run(const fs::path &directory)
{
    if (!fs::is_directory(directory))
    {
        lucent::error("package-census", "REFUSING: {} is not a directory, so nothing was loaded",
                      directory.string());
        return 2;
    }
    std::vector<fs::path> files;
    for (const fs::directory_entry &entry : fs::directory_iterator(directory))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".xxx")
        {
            files.push_back(entry.path());
        }
    }
    std::ranges::sort(files);
    if (files.empty())
    {
        lucent::error("package-census", "REFUSING: {} holds no .xxx packages", directory.string());
        return 2;
    }
    Census census;
    for (const fs::path &path : files)
    {
        LoadOne(path, census);
    }
    std::vector<std::pair<std::size_t, std::string>> classes;
    classes.reserve(census.export_classes.size());
    for (const auto &[name, count] : census.export_classes)
    {
        classes.emplace_back(count, name);
    }
    std::ranges::sort(classes, std::greater<>());
    for (std::size_t i = 0; i < std::min<std::size_t>(classes.size(), 25U); ++i)
    {
        lucent::info("package-census", "  {:>8} {}", classes[i].first, classes[i].second);
    }
    lucent::info("package-census",
                 "{} of {} package(s) decoded; {} failed; {} names, {} imports, {} exports, "
                 "{} export classes, {} MiB uncompressed",
                 census.packages - census.failed, census.packages, census.failed, census.names,
                 census.imports, census.exports, census.export_classes.size(),
                 census.uncompressed_bytes >> 20U);
    return census.failed == 0U ? 0 : 1;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        lucent::error("package-census", "usage: gears_package_census <CookedXenon directory>");
        return 2;
    }
    return Run(argv[1]);
}
