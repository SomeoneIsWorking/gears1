// File I/O.
//
// Unlike the graphics and audio layers, this is a real implementation: the
// title's reads are served from its actual data files.
#include "import_stub.h"

#include <algorithm>
#include <vector>
#include "directory_info.h"
#include "file_disposition.h"
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include <byteswap.h>
#include <lucent/log.h>

#include "guest_filesystem.h"

namespace
{

constexpr uint32_t kStatusObjectNameNotFound = 0xC0000034;
// FILE_CREATE asked for a file that is already there. Distinct from "not found"
// because a title can act on the difference.
constexpr uint32_t kStatusObjectNameCollision = 0xC0000035;
constexpr uint32_t kStatusEndOfFile = 0xC0000011;
constexpr uint32_t kStatusAccessDenied = 0xC0000022;
constexpr uint32_t kStatusNoSuchFile = 0xC000000F;
constexpr uint32_t kStatusNoMoreFiles = 0x80000006;
constexpr uint32_t kStatusInfoLengthMismatch = 0xC0000004;
// What the console answers when an operation makes no sense for the object: a
// byte transfer on a directory handle, for instance.
constexpr uint32_t kStatusInvalidDeviceRequest = 0xC0000010;

constexpr uint32_t kFileAttributeDirectory = 0x10;
constexpr uint32_t kFileAttributeNormal = 0x80;

struct OpenFile
{
    FILE* handle;
    uint64_t size;
    std::string guestPath;
    bool writable = false;
    // Directories have no FILE*: they are walked with NtQueryDirectoryFile
    // instead of read, so the host path is what is kept.
    bool isDirectory = false;
    std::filesystem::path hostPath;
    size_t directoryCursor = 0;
    std::vector<gears::DirectoryEntry> listing;
    bool listed = false;
};

std::mutex g_filesMutex;
// SHARED, not unique, and FindFile hands out a shared_ptr rather than a raw
// pointer. The lock is released when FindFile returns, so with a raw pointer a
// concurrent NtClose could erase and destroy the OpenFile while another thread
// was still reading or writing through it -- a use-after-free that would land
// somewhere else entirely, which is exactly the kind of second-order crash that
// masked the real one in catalog #46. A shared_ptr keeps the object alive for
// as long as the caller holds it; the close still removes it from the table
// immediately, so no new lookup can find it.
std::unordered_map<uint32_t, std::shared_ptr<OpenFile>> g_openFiles;
uint32_t g_nextFileHandle = 0xF4000000;

// X_OBJECT_ATTRIBUTES: root directory, a pointer to an X_ANSI_STRING name, and
// attribute flags.
std::string ReadObjectPath(uint8_t* base, uint32_t objectAttributes)
{
    if (objectAttributes == 0)
        return {};

    const uint32_t namePtr = ByteSwap(*reinterpret_cast<uint32_t*>(base + objectAttributes + 4));
    if (namePtr == 0)
        return {};

    const uint16_t length = ByteSwap(*reinterpret_cast<uint16_t*>(base + namePtr));
    const uint32_t buffer = ByteSwap(*reinterpret_cast<uint32_t*>(base + namePtr + 4));
    if (buffer == 0)
        return {};

    return std::string(reinterpret_cast<const char*>(base + buffer), length);
}

void StoreU32(uint8_t* base, uint32_t address, uint32_t value)
{
    if (address != 0)
        *reinterpret_cast<uint32_t*>(base + address) = ByteSwap(value);
}

void StoreU64(uint8_t* base, uint32_t address, uint64_t value)
{
    if (address != 0)
        *reinterpret_cast<uint64_t*>(base + address) = ByteSwap(value);
}

std::shared_ptr<OpenFile> FindFile(uint32_t handle)
{
    std::lock_guard<std::mutex> guard(g_filesMutex);
    auto it = g_openFiles.find(handle);
    return it != g_openFiles.end() ? it->second : nullptr;
}

} // namespace

// NTSTATUS NtQueryFullAttributesFile(POBJECT_ATTRIBUTES, PFILE_NETWORK_OPEN_INFORMATION)
void __imp__NtQueryFullAttributesFile(PPCContext& __restrict ctx, uint8_t* base)
{
    const std::string guestPath = ReadObjectPath(base, ctx.r3.u32);
    const std::filesystem::path host = gears::Files().Resolve(guestPath);

    if (host.empty())
    {
        lucent::debug("fs", "query '{}' -> not found", guestPath);
        ctx.r3.u64 = kStatusObjectNameNotFound;
        return;
    }

    std::error_code ec;
    const bool directory = std::filesystem::is_directory(host, ec);
    const uint64_t size = directory ? 0 : std::filesystem::file_size(host, ec);

    const uint32_t info = ctx.r4.u32;
    // Timestamps are left zero: nothing has read them, and inventing plausible
    // ones would be worse than an obviously-unset value if something does.
    StoreU64(base, info + 0x20, size); // allocation size
    StoreU64(base, info + 0x28, size); // end of file
    StoreU32(base, info + 0x30, directory ? kFileAttributeDirectory : kFileAttributeNormal);

    lucent::debug("fs", "query '{}' -> {} ({} bytes)", guestPath, host.string(), size);
    ctx.r3.u64 = gears::kStatusSuccess;
}

// NTSTATUS NtCreateFile(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
//                       PLARGE_INTEGER AllocationSize, ULONG FileAttributes,
//                       ULONG ShareAccess, ULONG CreateDisposition, ULONG CreateOptions)
//
// CreateDisposition arrives in r10 and used to be IGNORED. The rule was "if the
// file is missing and the mount is writable, make it", which is right for a save
// being written and wrong for every read: a caller asking to open an existing
// file got a newly created empty one and STATUS_SUCCESS. An honest failure
// turned into a success carrying no data is the most expensive shape a host shim
// can have -- the title believes its load worked, continues on empty state, and
// the eventual crash is nowhere near the lie.
static void OpenGuestFileWithDisposition(PPCContext& __restrict ctx,
                                         uint8_t* base,
                                         gears::FileDisposition disposition)
{
    const uint32_t handlePtr = ctx.r3.u32;
    const uint32_t objectAttributes = ctx.r5.u32;
    const uint32_t ioStatusBlock = ctx.r6.u32;

    const std::string guestPath = ReadObjectPath(base, objectAttributes);
    const std::filesystem::path host = gears::Files().Resolve(guestPath);

    // A save file is CREATED, not found: on a writable mount a missing file is
    // a file to make, which is the whole difference between the disc and the
    // player's storage.
    const bool writable = gears::Files().IsWritable(guestPath);
    const bool exists = !host.empty() && std::filesystem::exists(host);

    if (host.empty())
    {
        lucent::debug("fs", "open '{}' -> no host path for it", guestPath);
        StoreU32(base, ioStatusBlock, kStatusObjectNameNotFound);
        ctx.r3.u64 = kStatusObjectNameNotFound;
        return;
    }

    const gears::FileOpenAction action =
        gears::DecideFileOpen(disposition, exists, writable);
    if (action == gears::FileOpenAction::NotFound)
    {
        lucent::debug("fs", "open '{}' (disposition {}) -> not found", guestPath,
                      uint32_t(disposition));
        StoreU32(base, ioStatusBlock, kStatusObjectNameNotFound);
        ctx.r3.u64 = kStatusObjectNameNotFound;
        return;
    }
    if (action == gears::FileOpenAction::AlreadyExists)
    {
        lucent::debug("fs", "open '{}' (FILE_CREATE) -> already exists",
                      guestPath);
        StoreU32(base, ioStatusBlock, kStatusObjectNameCollision);
        ctx.r3.u64 = kStatusObjectNameCollision;
        return;
    }

    std::error_code ec;
    if (std::filesystem::is_directory(host, ec))
    {
        // A DIRECTORY HANDLE. The title opens the root of its save mount and
        // walks it, so refusing this told a title checking whether its save
        // location is usable that the location cannot be opened at all.
        auto directory = std::make_shared<OpenFile>();
        directory->handle = nullptr;
        directory->guestPath = guestPath;
        directory->hostPath = host;
        directory->isDirectory = true;
        directory->writable = writable;

        uint32_t directoryHandle;
        {
            std::lock_guard<std::mutex> guard(g_filesMutex);
            directoryHandle = g_nextFileHandle;
            g_nextFileHandle += 4;
            g_openFiles[directoryHandle] = std::move(directory);
        }

        StoreU32(base, handlePtr, directoryHandle);
        StoreU32(base, ioStatusBlock, gears::kStatusSuccess);
        StoreU32(base, ioStatusBlock + 4, 1); // FILE_OPENED
        lucent::debug("fs", "open '{}' -> directory handle {:#x}", guestPath,
                      directoryHandle);
        ctx.r3.u64 = gears::kStatusSuccess;
        return;
    }

    if (writable)
    {
        std::error_code dirEc;
        std::filesystem::create_directories(host.parent_path(), dirEc);
    }
    // "r+b" keeps an existing save intact for a partial rewrite; "w+b" makes a
    // new one. Truncating an existing save on open would lose a player's
    // progress the moment the title looked at it.
    // The mode follows the ACTION, not the mount: truncation is what
    // FILE_OVERWRITE* and FILE_SUPERSEDE asked for and must never happen on a
    // plain open, which would silently discard a save the title meant to read.
    const char* mode = "rb";
    switch (action)
    {
    case gears::FileOpenAction::OpenExisting:
        mode = writable ? "r+b" : "rb";
        break;
    case gears::FileOpenAction::CreateNew:
    case gears::FileOpenAction::Truncate:
        mode = "w+b";
        break;
    default:
        break;      // the refusing actions returned above
    }

    FILE* f = fopen(host.c_str(), mode);
    if (f == nullptr)
    {
        StoreU32(base, ioStatusBlock, kStatusObjectNameNotFound);
        ctx.r3.u64 = kStatusObjectNameNotFound;
        return;
    }

    auto file = std::make_shared<OpenFile>();
    file->handle = f;
    // A truncated file is empty whatever it held a moment ago.
    file->size = (action == gears::FileOpenAction::OpenExisting)
                     ? std::filesystem::file_size(host, ec)
                     : 0;
    file->guestPath = guestPath;
    file->writable = writable;
    if (writable)
        lucent::info("fs", "{} '{}' for writing -> {}", exists ? "opened" : "created",
                     guestPath, host.string());

    uint32_t handle;
    {
        std::lock_guard<std::mutex> guard(g_filesMutex);
        handle = g_nextFileHandle;
        g_nextFileHandle += 4;
        g_openFiles[handle] = std::move(file);
    }

    StoreU32(base, handlePtr, handle);
    StoreU32(base, ioStatusBlock, gears::kStatusSuccess);
    // The Information field distinguishes these, and a title may branch on it.
    StoreU32(base, ioStatusBlock + 4,
             action == gears::FileOpenAction::CreateNew  ? 2u   // FILE_CREATED
             : action == gears::FileOpenAction::Truncate ? 3u   // FILE_OVERWRITTEN
                                                         : 1u); // FILE_OPENED

    // The disposition goes in the line so that "no refusals this run" carries a
    // denominator. Without it, zero refusals is indistinguishable from a rule
    // that never runs.
    lucent::debug("fs", "open '{}' (disposition {}) -> handle {:#x}", guestPath,
                  uint32_t(disposition), handle);
    ctx.r3.u64 = gears::kStatusSuccess;
}

void __imp__NtCreateFile(PPCContext& __restrict ctx, uint8_t* base)
{
    OpenGuestFileWithDisposition(ctx, base,
                                 gears::FileDisposition(ctx.r10.u32));
}

void __imp__NtOpenFile(PPCContext& __restrict ctx, uint8_t* base)
{
    // NtOpenFile has NO CreateDisposition: its parameters are (handle, access,
    // attributes, status, ShareAccess, OpenOptions), so r10 holds nothing to do
    // with one and reading it would be reading rubbish -- which is exactly what
    // delegating straight to NtCreateFile would now do. Opening an object that
    // already exists is what NtOpenFile MEANS, and that is FILE_OPEN.
    OpenGuestFileWithDisposition(ctx, base, gears::kFileOpen);
}

// NTSTATUS NtReadFile(HANDLE, HANDLE Event, PIO_APC_ROUTINE, PVOID ApcContext,
//                     PIO_STATUS_BLOCK, PVOID Buffer, ULONG Length,
//                     PLARGE_INTEGER ByteOffset)
void __imp__NtReadFile(PPCContext& __restrict ctx, uint8_t* base)
{
    const uint32_t handle = ctx.r3.u32;
    const uint32_t ioStatusBlock = ctx.r7.u32;
    const uint32_t buffer = ctx.r8.u32;
    const uint32_t length = ctx.r9.u32;
    const uint32_t byteOffsetPtr = ctx.r10.u32;

    const std::shared_ptr<OpenFile> file = FindFile(handle);
    if (file == nullptr)
    {
        lucent::error("fs", "read from unknown handle {:#x}", handle);
        ctx.r3.u64 = gears::kStatusInvalidHandle;
        return;
    }

    // A DIRECTORY HANDLE HAS NO FILE*. Directory handles were added so the
    // title could open the root of its save mount, and every byte-transfer path
    // here would otherwise dereference a null FILE* -- the console answers this
    // with a status, not a crash.
    if (file->isDirectory)
    {
        ctx.r3.u64 = kStatusInvalidDeviceRequest;
        return;
    }

    if (byteOffsetPtr != 0)
    {
        const uint64_t offset = ByteSwap(*reinterpret_cast<uint64_t*>(base + byteOffsetPtr));
        fseek(file->handle, long(offset), SEEK_SET);
    }

    const long position = ftell(file->handle);
    const size_t read = fread(base + buffer, 1, length, file->handle);

    // Reads were the one file operation with no diagnostic at all, which meant a
    // title deserialising nonsense could not be told from a title being fed
    // nonsense. The first bytes are what a bad length or a bad offset shows up
    // in, so they are worth the line.
    lucent::Line line;
    line.add("read {} bytes at {} from '{}' ->", length, position,
             file->guestPath);
    for (size_t i = 0; i < read && i < 8; ++i)
        line.add(" {:02x}", *(base + buffer + i));
    if (read != length)
        line.add(" (SHORT: {} of {})", read, length);
    line.flush_debug("fs");

    StoreU32(base, ioStatusBlock, read == 0 && length != 0
        ? kStatusEndOfFile : gears::kStatusSuccess);
    StoreU32(base, ioStatusBlock + 4, uint32_t(read));

    ctx.r3.u64 = (read == 0 && length != 0) ? kStatusEndOfFile : gears::kStatusSuccess;
}

// NTSTATUS NtWriteFile(HANDLE, HANDLE Event, PIO_APC_ROUTINE, PVOID ApcContext,
//                      PIO_STATUS_BLOCK, PVOID Buffer, ULONG Length,
//                      PLARGE_INTEGER ByteOffset, PULONG Key)
//
// The counterpart of NtReadFile, and the reason the title could not save: with
// no write path, every save it attempted had nowhere to land. Writes are
// refused on a handle that is not from a writable mount, so nothing can scribble
// on the player's game data through a path that resolves to the disc.
void __imp__NtWriteFile(PPCContext& __restrict ctx, uint8_t* base)
{
    const uint32_t handle = ctx.r3.u32;
    const uint32_t ioStatusBlock = ctx.r7.u32;
    const uint32_t buffer = ctx.r8.u32;
    const uint32_t length = ctx.r9.u32;
    const uint32_t byteOffsetPtr = ctx.r10.u32;

    const std::shared_ptr<OpenFile> file = FindFile(handle);
    if (file == nullptr)
    {
        lucent::error("fs", "write to unknown handle {:#x}", handle);
        ctx.r3.u64 = gears::kStatusInvalidHandle;
        return;
    }

    // A DIRECTORY HANDLE HAS NO FILE*. Directory handles were added so the
    // title could open the root of its save mount, and every byte-transfer path
    // here would otherwise dereference a null FILE* -- the console answers this
    // with a status, not a crash.
    if (file->isDirectory)
    {
        ctx.r3.u64 = kStatusInvalidDeviceRequest;
        return;
    }
    if (!file->writable)
    {
        lucent::warn("fs", "refused a write to '{}', which is not on a writable"
            " mount", file->guestPath);
        StoreU32(base, ioStatusBlock, kStatusAccessDenied);
        ctx.r3.u64 = kStatusAccessDenied;
        return;
    }

    if (byteOffsetPtr != 0)
    {
        const uint64_t offset = ByteSwap(*reinterpret_cast<uint64_t*>(base + byteOffsetPtr));
        fseek(file->handle, long(offset), SEEK_SET);
    }

    const size_t written = fwrite(base + buffer, 1, length, file->handle);
    // Flushed per write rather than at close: a player who kills the game has
    // still saved, and a save that exists only in a FILE* buffer is a save that
    // can vanish for reasons the player will never understand.
    fflush(file->handle);

    const uint64_t end = uint64_t(ftell(file->handle));
    file->size = std::max(file->size, end);

    StoreU32(base, ioStatusBlock, gears::kStatusSuccess);
    StoreU32(base, ioStatusBlock + 4, uint32_t(written));
    lucent::debug("fs", "wrote {} bytes to '{}'", written, file->guestPath);
    ctx.r3.u64 = gears::kStatusSuccess;
}

// NTSTATUS NtQueryInformationFile(HANDLE, PIO_STATUS_BLOCK, PVOID Info,
//                                 ULONG Length, FILE_INFORMATION_CLASS Class)
void __imp__NtQueryInformationFile(PPCContext& __restrict ctx, uint8_t* base)
{
    const uint32_t handle = ctx.r3.u32;
    const uint32_t ioStatusBlock = ctx.r4.u32;
    const uint32_t info = ctx.r5.u32;
    const uint32_t infoClass = ctx.r7.u32;

    const std::shared_ptr<OpenFile> file = FindFile(handle);
    if (file == nullptr)
    {
        ctx.r3.u64 = gears::kStatusInvalidHandle;
        return;
    }

    switch (infoClass)
    {
    case 5: // FileStandardInformation
        StoreU64(base, info + 0x00, file->isDirectory ? 0 : file->size);
        StoreU64(base, info + 0x08, file->isDirectory ? 0 : file->size);
        StoreU32(base, info + 0x10, 1); // number of links
        // +0x14 is delete-pending then the DIRECTORY flag. A title that opened
        // the root of its save mount asks exactly this to find out what it got,
        // and answering "not a directory" for a directory sends it down the
        // file path.
        StoreU32(base, info + 0x14, file->isDirectory ? 0x00000100u : 0u);
        break;

    case 14: // FilePositionInformation
        if (file->isDirectory)
        {
            ctx.r3.u64 = kStatusInvalidDeviceRequest;
            return;
        }
        StoreU64(base, info + 0x00, uint64_t(ftell(file->handle)));
        break;

    case 34: // FileNetworkOpenInformation
        StoreU32(base, info + 0x30, file->isDirectory ? kFileAttributeDirectory
                                                      : kFileAttributeNormal);
        // Same layout NtQueryFullAttributesFile fills, and timestamps are left
        // zero for the same reason: an obviously-unset value beats a plausible
        // invented one. Only the sizes are actually read here -- the title
        // sizes an allocation from this reply, so leaving the class unhandled
        // made it allocate from uninitialised memory.
        StoreU64(base, info + 0x20, file->isDirectory ? 0 : file->size);
        StoreU64(base, info + 0x28, file->isDirectory ? 0 : file->size);
        break;

    default:
        // Answering an unknown class with zeros would look like a valid reply.
        lucent::error("fs", "NtQueryInformationFile: unhandled class {}", infoClass);
        ctx.r3.u64 = gears::kStatusInvalidParameter;
        return;
    }

    StoreU32(base, ioStatusBlock, gears::kStatusSuccess);
    ctx.r3.u64 = gears::kStatusSuccess;
}

void __imp__NtSetInformationFile(PPCContext& __restrict ctx, uint8_t* base)
{
    const uint32_t handle = ctx.r3.u32;
    const uint32_t info = ctx.r5.u32;
    const uint32_t infoClass = ctx.r7.u32;

    const std::shared_ptr<OpenFile> file = FindFile(handle);
    if (file == nullptr)
    {
        ctx.r3.u64 = gears::kStatusInvalidHandle;
        return;
    }

    if (file->isDirectory)
    {
        // Nothing that can be set on this handle applies to a directory, and
        // every path below it assumes a FILE*.
        ctx.r3.u64 = kStatusInvalidDeviceRequest;
        return;
    }

    if (infoClass == 14) // FilePositionInformation
    {
        const uint64_t position = ByteSwap(*reinterpret_cast<uint64_t*>(base + info));
        fseek(file->handle, long(position), SEEK_SET);
        StoreU32(base, ctx.r4.u32, gears::kStatusSuccess);
        ctx.r3.u64 = gears::kStatusSuccess;
        return;
    }

    lucent::error("fs", "NtSetInformationFile: unhandled class {}", infoClass);
    ctx.r3.u64 = gears::kStatusInvalidParameter;
}

bool CloseGuestFile(uint32_t handle)
{
    std::lock_guard<std::mutex> guard(g_filesMutex);
    auto it = g_openFiles.find(handle);
    if (it == g_openFiles.end())
        return false;

    // A DIRECTORY HANDLE HAS NO FILE*. Directory handles carry a host path and
    // a listing instead, so closing one must not reach fclose -- fclose(nullptr)
    // is a crash, and it was one: the title opens the root of its save mount and
    // then closes it.
    if (it->second->handle != nullptr)
        fclose(it->second->handle);
    g_openFiles.erase(it);
    return true;
}

// NTSTATUS NtQueryDirectoryFile(HANDLE, HANDLE Event, PIO_APC_ROUTINE, PVOID ApcContext,
//                               PIO_STATUS_BLOCK, PVOID FileInformation, ULONG Length,
//                               PANSI_STRING FileName, BOOLEAN RestartScan)
//
// Walks a directory handle, ONE ENTRY PER CALL, which is the console's
// behaviour and what a caller looping until STATUS_NO_MORE_FILES expects.
//
// The listing is taken once, on the first query, and then walked from a cursor.
// Re-reading the directory on every call would let it change underneath a walk
// -- and this runtime writes save files into exactly these directories, so that
// is a real possibility rather than a theoretical one.
void __imp__NtQueryDirectoryFile(PPCContext& __restrict ctx, uint8_t* base)
{
    const uint32_t handle = ctx.r3.u32;
    const uint32_t ioStatusBlock = ctx.r7.u32;
    const uint32_t buffer = ctx.r8.u32;
    const uint32_t length = ctx.r9.u32;

    const std::shared_ptr<OpenFile> file = FindFile(handle);
    if (file == nullptr || !file->isDirectory)
    {
        ctx.r3.u64 = gears::kStatusInvalidHandle;
        return;
    }

    // The console refuses a buffer that cannot hold the fixed part plus a name.
    if (length < gears::kDirectoryInfoHeaderSize)
    {
        ctx.r3.u64 = kStatusInfoLengthMismatch;
        return;
    }

    std::lock_guard<std::mutex> guard(g_filesMutex);
    if (!file->listed)
    {
        std::error_code ec;
        for (const auto& entry :
             std::filesystem::directory_iterator(file->hostPath, ec))
        {
            gears::DirectoryEntry item;
            item.name = entry.path().filename().string();
            item.directory = entry.is_directory(ec);
            item.size = item.directory ? 0 : entry.file_size(ec);
            file->listing.push_back(std::move(item));
        }
        // Sorted, so two walks of the same directory agree: the filesystem
        // makes no ordering promise and a title that indexes what it walked
        // would see entries move between runs.
        std::sort(file->listing.begin(), file->listing.end(),
                  [](const gears::DirectoryEntry& a, const gears::DirectoryEntry& b) {
                      return a.name < b.name;
                  });
        file->listed = true;
        lucent::debug("fs", "directory '{}' has {} entries", file->guestPath,
                      file->listing.size());
    }

    if (file->directoryCursor >= file->listing.size())
    {
        StoreU32(base, ioStatusBlock + 4, 0);
        ctx.r3.u64 = kStatusNoMoreFiles;
        return;
    }

    const gears::DirectoryEntry& entry = file->listing[file->directoryCursor];
    const uint32_t written = gears::WriteDirectoryEntry(base, buffer, length,
                                                        entry, true);
    if (written == 0)
    {
        ctx.r3.u64 = kStatusInfoLengthMismatch;
        return;
    }

    ++file->directoryCursor;
    StoreU32(base, ioStatusBlock, gears::kStatusSuccess);
    StoreU32(base, ioStatusBlock + 4, written);
    ctx.r3.u64 = gears::kStatusSuccess;
}
