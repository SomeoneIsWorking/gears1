#include "content_files.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <fstream>

#include "byte_reader.h"

namespace gears::engine::package
{
namespace
{

std::string Lower(std::string_view text)
{
    std::string lowered(text);
    std::ranges::transform(lowered, lowered.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lowered;
}

} // namespace

std::vector<std::uint8_t> ReadPackageFile(const std::filesystem::path &path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
    {
        throw PackageFormatError(
            std::format("cannot open package file {}", path.filename().string()));
    }
    std::vector<std::uint8_t> bytes(std::filesystem::file_size(path));
    stream.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (stream.gcount() != static_cast<std::streamsize>(bytes.size()))
    {
        throw PackageFormatError(std::format("package file {} ended after {} of {} byte(s)",
                                             path.filename().string(), stream.gcount(),
                                             bytes.size()));
    }
    return bytes;
}

ContentFiles::ContentFiles(std::filesystem::path directory) : directory_(std::move(directory))
{
    if (!std::filesystem::is_directory(directory_))
    {
        throw PackageFormatError(
            std::format("content directory {} does not exist", directory_.string()));
    }
    for (const std::filesystem::directory_entry &entry :
         std::filesystem::directory_iterator(directory_))
    {
        if (entry.is_regular_file() && Lower(entry.path().extension().string()) == ".xxx")
        {
            paths_.emplace(Lower(entry.path().stem().string()), entry.path());
        }
    }
    if (paths_.empty())
    {
        throw PackageFormatError(
            std::format("content directory {} holds no packages", directory_.string()));
    }
}

std::span<const std::uint8_t> ContentFiles::File(std::string_view package)
{
    std::string key = Lower(package);
    auto loaded = loaded_.find(key);
    if (loaded != loaded_.end())
    {
        return loaded->second;
    }
    auto path = paths_.find(key);
    if (path == paths_.end())
    {
        throw PackageFormatError(std::format("no package file for '{}'", package));
    }
    return loaded_.emplace(key, ReadPackageFile(path->second)).first->second;
}

std::span<const std::uint8_t> ContentFiles::Range(std::string_view package, std::size_t offset,
                                                  std::size_t size)
{
    std::span<const std::uint8_t> file = File(package);
    if (offset > file.size() || size > file.size() - offset)
    {
        throw PackageFormatError(std::format("range {:#x}+{:#x} lies outside package '{}' of {} "
                                             "byte(s)",
                                             offset, size, package, file.size()));
    }
    return file.subspan(offset, size);
}

} // namespace gears::engine::package
