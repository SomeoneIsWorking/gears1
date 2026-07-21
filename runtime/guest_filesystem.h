#pragma once

#include <cstdint>
#include <filesystem>
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

private:
    std::filesystem::path gameDirectory_;
};

FileSystem& Files();

} // namespace gears
