#include "guest_filesystem.h"

#include <algorithm>

#include <lucent/log.h>

namespace gears
{

namespace
{

// Device prefixes the title uses to reach its own data. They all resolve to the
// game directory: on the console these are the disc and its aliases, and here
// there is only one place the files can be.
constexpr const char* kGamePrefixes[] = {
    "\\Device\\Cdrom0\\",
    "\\Device\\Harddisk0\\Partition1\\",
    "\\Device\\Harddisk0\\Partition0\\",
    "\\SystemRoot\\",
    "game:\\",
    "d:\\",
    "D:\\",
};

std::string StripPrefix(const std::string& path)
{
    for (const char* prefix : kGamePrefixes)
    {
        const size_t length = std::char_traits<char>::length(prefix);
        if (path.size() >= length && path.compare(0, length, prefix) == 0)
            return path.substr(length);
    }
    return {};
}

} // namespace

void FileSystem::SetGameDirectory(const std::filesystem::path& directory)
{
    gameDirectory_ = directory;
    lucent::info("fs", "game directory: {}", gameDirectory_.string());
}

std::filesystem::path FileSystem::Resolve(const std::string& guestPath) const
{
    if (gameDirectory_.empty())
        return {};

    const std::string relative = StripPrefix(guestPath);
    if (relative.empty())
    {
        lucent::warn("fs", "unmapped device in path: {}", guestPath);
        return {};
    }

    std::string hostRelative = relative;
    std::replace(hostRelative.begin(), hostRelative.end(), '\\', '/');

    std::filesystem::path candidate = gameDirectory_ / hostRelative;
    if (std::filesystem::exists(candidate))
        return candidate;

    // The console's file systems are case-insensitive and titles are casual
    // about case; a Linux host is not. Fall back to a case-insensitive walk
    // rather than reporting a file missing that is really there.
    std::filesystem::path walk = gameDirectory_;
    size_t start = 0;
    while (start < hostRelative.size())
    {
        size_t slash = hostRelative.find('/', start);
        const std::string component = hostRelative.substr(
            start, slash == std::string::npos ? std::string::npos : slash - start);
        start = slash == std::string::npos ? hostRelative.size() : slash + 1;

        if (component.empty())
            continue;

        std::error_code ec;
        bool matched = false;
        for (const auto& entry : std::filesystem::directory_iterator(walk, ec))
        {
            const std::string name = entry.path().filename().string();
            if (name.size() != component.size())
                continue;
            if (std::equal(name.begin(), name.end(), component.begin(),
                    [](char a, char b) { return tolower(a) == tolower(b); }))
            {
                walk = entry.path();
                matched = true;
                break;
            }
        }

        if (!matched)
            return {};
    }

    return walk;
}

FileSystem& Files()
{
    static FileSystem instance;
    return instance;
}

} // namespace gears
