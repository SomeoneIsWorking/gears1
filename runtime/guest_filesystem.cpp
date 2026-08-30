#include "guest_filesystem.h"

#include <mutex>

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include <lucent/config.h>

#include <lucent/log.h>

namespace gears
{

namespace
{

// Device prefixes the title uses to reach its own data. They all resolve to the
// game directory: on the console these are the disc and its aliases, and here
// there is only one place the files can be.
constexpr const char *kGamePrefixes[] = {
    "\\Device\\Cdrom0\\",
    "\\Device\\Harddisk0\\Partition1\\",
    "\\Device\\Harddisk0\\Partition0\\",
    "\\SystemRoot\\",
    "game:\\",
    "d:\\",
    "D:\\",
};

std::string StripPrefix(const std::string &path)
{
    for (const char *prefix : kGamePrefixes)
    {
        const size_t length = std::char_traits<char>::length(prefix);
        if (path.size() >= length && path.compare(0, length, prefix) == 0)
            return path.substr(length);
    }
    return {};
}

} // namespace

void FileSystem::SetGameDirectory(const std::filesystem::path &directory)
{
    std::lock_guard<std::mutex> guard(mutex_);
    gameDirectory_ = directory;
    lucent::info("fs", "game directory: {}", gameDirectory_.string());
}

bool FileSystem::SetSaveNamespace(std::string_view saveNamespace)
{
    std::lock_guard<std::mutex> guard(mutex_);
    if (!saveDirectory_.empty() || !saveNamespace_.empty() || saveNamespace.empty())
    {
        lucent::error("fs", "cannot activate save namespace '{}'", saveNamespace);
        return false;
    }
    saveNamespace_ = saveNamespace;
    lucent::info("fs", "save namespace: {}", saveNamespace_);
    return true;
}

const std::filesystem::path &FileSystem::SaveDirectory() const
{
    std::lock_guard<std::mutex> guard(mutex_);
    if (!saveDirectory_.empty())
        return saveDirectory_;

    if (saveNamespace_.empty())
    {
        lucent::error("fs", "save directory requested before a title namespace was activated");
        return saveDirectory_;
    }

    if (const std::string &configured = lucent::config::text("SAVE_DIR"); !configured.empty())
    {
        saveDirectory_ = configured;
    }
    else if (const char *xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg)
    {
        // Where a Linux game is expected to keep user data. A console port
        // that scatters saves next to the executable is a port that has not
        // finished arriving.
        saveDirectory_ = std::filesystem::path(xdg) / saveNamespace_;
    }
    else if (const char *home = std::getenv("HOME"); home && *home)
    {
        saveDirectory_ = std::filesystem::path(home) / ".local/share" / saveNamespace_;
    }
    else
    {
        saveDirectory_ = std::filesystem::path("saves") / saveNamespace_;
    }

    std::error_code ec;
    std::filesystem::create_directories(saveDirectory_, ec);
    lucent::info("fs", "save directory: {}", saveDirectory_.string());
    return saveDirectory_;
}

void FileSystem::Mount(const std::string &rootName, const std::filesystem::path &hostDirectory)
{
    std::lock_guard<std::mutex> guard(mutex_);
    std::error_code ec;
    std::filesystem::create_directories(hostDirectory, ec);
    std::string key = rootName;
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    mounts_[key] = hostDirectory;
    lucent::info("fs", "mounted '{}:' -> {} (writable)", rootName, hostDirectory.string());
}

void FileSystem::Unmount(const std::string &rootName)
{
    std::lock_guard<std::mutex> guard(mutex_);
    std::string key = rootName;
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    if (mounts_.erase(key))
        lucent::info("fs", "unmounted '{}:'", rootName);
}

namespace
{
// "SAVE:\slot0" or "\Device\SAVE\slot0" -> ("save", "slot0").
bool SplitMountPath(const std::string &guestPath, std::string &root, std::string &rest)
{
    std::string path = guestPath;
    constexpr const char *kDevice = "\\Device\\";
    if (path.compare(0, std::char_traits<char>::length(kDevice), kDevice) == 0)
        path = path.substr(std::char_traits<char>::length(kDevice));

    const size_t colon = path.find(':');
    const size_t slash = path.find('\\');
    const size_t split = colon != std::string::npos ? colon : slash;
    if (split == std::string::npos || split == 0)
        return false;

    root = path.substr(0, split);
    std::transform(root.begin(), root.end(), root.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    rest = path.substr(split + 1);
    while (!rest.empty() && (rest.front() == '\\' || rest.front() == '/'))
        rest.erase(rest.begin());
    return true;
}
} // namespace

bool FileSystem::IsWritable(const std::string &guestPath) const
{
    std::lock_guard<std::mutex> guard(mutex_);
    std::string root, rest;
    return SplitMountPath(guestPath, root, rest) && mounts_.find(root) != mounts_.end();
}

std::filesystem::path FileSystem::Resolve(const std::string &guestPath) const
{
    std::lock_guard<std::mutex> guard(mutex_);
    // A writable mount wins over the disc: the title asks for the same shape of
    // path either way, and only the mount table says which is which.
    {
        std::string root, rest;
        if (SplitMountPath(guestPath, root, rest))
        {
            if (auto it = mounts_.find(root); it != mounts_.end())
            {
                std::string hostRelative = rest;
                std::replace(hostRelative.begin(), hostRelative.end(), '\\', '/');
                return hostRelative.empty() ? it->second : it->second / hostRelative;
            }
        }
    }

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
        for (const auto &entry : std::filesystem::directory_iterator(walk, ec))
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

FileSystem &Files()
{
    static FileSystem instance;
    return instance;
}

} // namespace gears
