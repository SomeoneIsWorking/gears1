// File I/O.
//
// Unlike the graphics and audio layers, this is a real implementation: the
// title's reads are served from its actual data files.
#include "import_stub.h"

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
constexpr uint32_t kStatusEndOfFile = 0xC0000011;
constexpr uint32_t kStatusNoSuchFile = 0xC000000F;

constexpr uint32_t kFileAttributeDirectory = 0x10;
constexpr uint32_t kFileAttributeNormal = 0x80;

struct OpenFile
{
    FILE* handle;
    uint64_t size;
    std::string guestPath;
};

std::mutex g_filesMutex;
std::unordered_map<uint32_t, std::unique_ptr<OpenFile>> g_openFiles;
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

OpenFile* FindFile(uint32_t handle)
{
    std::lock_guard<std::mutex> guard(g_filesMutex);
    auto it = g_openFiles.find(handle);
    return it != g_openFiles.end() ? it->second.get() : nullptr;
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
void __imp__NtCreateFile(PPCContext& __restrict ctx, uint8_t* base)
{
    const uint32_t handlePtr = ctx.r3.u32;
    const uint32_t objectAttributes = ctx.r5.u32;
    const uint32_t ioStatusBlock = ctx.r6.u32;

    const std::string guestPath = ReadObjectPath(base, objectAttributes);
    const std::filesystem::path host = gears::Files().Resolve(guestPath);

    if (host.empty() || !std::filesystem::exists(host))
    {
        lucent::debug("fs", "open '{}' -> not found", guestPath);
        StoreU32(base, ioStatusBlock, kStatusObjectNameNotFound);
        ctx.r3.u64 = kStatusObjectNameNotFound;
        return;
    }

    std::error_code ec;
    if (std::filesystem::is_directory(host, ec))
    {
        // Directory handles are only useful with NtQueryDirectoryFile, which is
        // not implemented; refusing is better than handing back a handle that
        // silently fails every later operation.
        lucent::warn("fs", "open '{}' is a directory -- not supported", guestPath);
        StoreU32(base, ioStatusBlock, kStatusNoSuchFile);
        ctx.r3.u64 = kStatusNoSuchFile;
        return;
    }

    FILE* f = fopen(host.c_str(), "rb");
    if (f == nullptr)
    {
        StoreU32(base, ioStatusBlock, kStatusObjectNameNotFound);
        ctx.r3.u64 = kStatusObjectNameNotFound;
        return;
    }

    auto file = std::make_unique<OpenFile>();
    file->handle = f;
    file->size = std::filesystem::file_size(host, ec);
    file->guestPath = guestPath;

    uint32_t handle;
    {
        std::lock_guard<std::mutex> guard(g_filesMutex);
        handle = g_nextFileHandle;
        g_nextFileHandle += 4;
        g_openFiles[handle] = std::move(file);
    }

    StoreU32(base, handlePtr, handle);
    StoreU32(base, ioStatusBlock, gears::kStatusSuccess);
    StoreU32(base, ioStatusBlock + 4, 1); // FILE_OPENED

    lucent::debug("fs", "open '{}' -> handle {:#x}", guestPath, handle);
    ctx.r3.u64 = gears::kStatusSuccess;
}

void __imp__NtOpenFile(PPCContext& __restrict ctx, uint8_t* base)
{
    // NtOpenFile's arguments line up with NtCreateFile's leading ones for the
    // read-only case, which is all the title uses it for.
    __imp__NtCreateFile(ctx, base);
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

    OpenFile* file = FindFile(handle);
    if (file == nullptr)
    {
        lucent::error("fs", "read from unknown handle {:#x}", handle);
        ctx.r3.u64 = gears::kStatusInvalidHandle;
        return;
    }

    if (byteOffsetPtr != 0)
    {
        const uint64_t offset = ByteSwap(*reinterpret_cast<uint64_t*>(base + byteOffsetPtr));
        fseek(file->handle, long(offset), SEEK_SET);
    }

    const size_t read = fread(base + buffer, 1, length, file->handle);

    StoreU32(base, ioStatusBlock, read == 0 && length != 0
        ? kStatusEndOfFile : gears::kStatusSuccess);
    StoreU32(base, ioStatusBlock + 4, uint32_t(read));

    ctx.r3.u64 = (read == 0 && length != 0) ? kStatusEndOfFile : gears::kStatusSuccess;
}

// NTSTATUS NtQueryInformationFile(HANDLE, PIO_STATUS_BLOCK, PVOID Info,
//                                 ULONG Length, FILE_INFORMATION_CLASS Class)
void __imp__NtQueryInformationFile(PPCContext& __restrict ctx, uint8_t* base)
{
    const uint32_t handle = ctx.r3.u32;
    const uint32_t ioStatusBlock = ctx.r4.u32;
    const uint32_t info = ctx.r5.u32;
    const uint32_t infoClass = ctx.r7.u32;

    OpenFile* file = FindFile(handle);
    if (file == nullptr)
    {
        ctx.r3.u64 = gears::kStatusInvalidHandle;
        return;
    }

    switch (infoClass)
    {
    case 5: // FileStandardInformation
        StoreU64(base, info + 0x00, file->size); // allocation size
        StoreU64(base, info + 0x08, file->size); // end of file
        StoreU32(base, info + 0x10, 1);          // number of links
        StoreU32(base, info + 0x14, 0);          // delete pending / directory
        break;

    case 14: // FilePositionInformation
        StoreU64(base, info + 0x00, uint64_t(ftell(file->handle)));
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

    OpenFile* file = FindFile(handle);
    if (file == nullptr)
    {
        ctx.r3.u64 = gears::kStatusInvalidHandle;
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

    fclose(it->second->handle);
    g_openFiles.erase(it);
    return true;
}
