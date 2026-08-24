#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <string_view>

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
    void SetGameDirectory(const std::filesystem::path &directory);
    [[nodiscard]] bool SetSaveNamespace(std::string_view saveNamespace);
    bool HasGameDirectory() const { return !gameDirectory_.empty(); }

    // Returns an empty path when the guest path names a device that is not
    // mapped, which is different from a file that is simply missing.
    std::filesystem::path Resolve(const std::string &guestPath) const;

    // Where saves live on the host. A console writes them to a memory unit or
    // the hard drive; a PC game writes them under the user's data directory,
    // and that difference is the port's to make rather than to emulate.
    // GEARS_SAVE_DIR overrides it.
    const std::filesystem::path &SaveDirectory() const;

    // Content packages the title creates (XamContentCreateEx) are mounted under
    // a root name it chooses, and it then opens files as "<root>:\file". A
    // mount is a guest root name pointing at a host directory that this
    // runtime will WRITE to, which is what makes it different from the disc.
    void Mount(const std::string &rootName, const std::filesystem::path &hostDirectory);
    void Unmount(const std::string &rootName);

    // True when the resolved path is under a writable mount rather than the
    // read-only game directory.
    bool IsWritable(const std::string &guestPath) const;

  private:
    // MOUNTS ARE TOUCHED FROM SEVERAL GUEST THREADS AT ONCE. Mount runs from
    // XamContentCreateEx and Unmount ERASES from XamContentClose, both guest
    // imports, while Resolve and IsWritable read the same map from NtCreateFile
    // and NtOpenFile on other threads. An erase concurrent with a lookup
    // invalidates the node the reader is walking -- the same class of defect as
    // the raw pointer FindFile used to hand out, and reachable on the save path
    // this port now exercises.
    //
    // The lazily-derived saveDirectory_ is guarded by the same lock, since it is
    // computed on first use from any thread that asks.
    mutable std::mutex mutex_;
    std::filesystem::path gameDirectory_;
    mutable std::filesystem::path saveDirectory_;
    std::string saveNamespace_ = "gears1";
    std::map<std::string, std::filesystem::path, std::less<>> mounts_;
};

FileSystem &Files();

} // namespace gears
