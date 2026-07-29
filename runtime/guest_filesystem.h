#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>

namespace gears
{

// Translates the console's object paths onto a host directory holding the
// game's files, and owns the open-file table.
//
// The title addresses its data through device paths like
// "\Device\Cdrom0\WarGame\..." or the "game:\" alias. Those all resolve to the
// same place here: the directory the user extracted the disc into.
class FileSystem
{
public:
    void SetGameDirectory(const std::filesystem::path& directory);
    bool HasGameDirectory() const { return !gameDirectory_.empty(); }

    // Returns an empty path when the guest path names a device that is not
    // mapped, which is different from a file that is simply missing.
    std::filesystem::path Resolve(const std::string& guestPath) const;

    // Where saves live on the host. A console writes them to a memory unit or
    // the hard drive; a PC game writes them under the user's data directory,
    // and that difference is the port's to make rather than to emulate.
    // GEARS_SAVE_DIR overrides it.
    const std::filesystem::path& SaveDirectory() const;

    // Content packages the title creates (XamContentCreateEx) are mounted under
    // a root name it chooses, and it then opens files as "<root>:\file". A
    // mount is a guest root name pointing at a host directory that this
    // runtime will WRITE to, which is what makes it different from the disc.
    void Mount(const std::string& rootName, const std::filesystem::path& hostDirectory);
    void Unmount(const std::string& rootName);

    // True when the resolved path is under a writable mount rather than the
    // read-only game directory.
    bool IsWritable(const std::string& guestPath) const;

private:
    std::filesystem::path gameDirectory_;
    mutable std::filesystem::path saveDirectory_;
    std::map<std::string, std::filesystem::path, std::less<>> mounts_;
};

FileSystem& Files();

} // namespace gears
