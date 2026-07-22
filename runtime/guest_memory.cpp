#define _GNU_SOURCE 1
#include "guest_memory.h"

#include <sys/mman.h>
#include <unistd.h>

#include <cstring>

#include <lucent/log.h>

namespace gears
{

namespace
{
// PPC_LOOKUP_FUNC indexes a table at base + PPC_IMAGE_BASE + PPC_IMAGE_SIZE,
// scaling (address - PPC_CODE_BASE) by 2. Guest instructions are 4-byte
// aligned, so that scaling yields 8 bytes -- one host pointer -- per
// instruction slot.
constexpr uint64_t kFuncTableBase = PPC_IMAGE_BASE + PPC_IMAGE_SIZE;
constexpr uint64_t kFuncTableSize = uint64_t(PPC_CODE_SIZE) * 2;

// The console has 512 MiB of RAM, and exposes it through several virtual
// windows that differ only in caching and page size. Guest code converts
// between them by masking the top three bits (`clrlwi rD,rS,3`), so a pointer
// handed out as 0xA0320000 comes back as physical 0x00320000 and must refer to
// the same bytes. These are the windows that alias physical RAM.
constexpr uint32_t kPhysicalSize = 0x20000000;
constexpr uint32_t kPhysicalAliases[] = {
    0x00000000, // raw physical
    0xA0000000, // cached
    0xC0000000, // 16 MiB pages
    0xE0000000, // 4 KiB pages
};
} // namespace

bool GuestMemory::Reserve()
{
    // The whole 4 GiB guest window, not just up to the function table: the
    // console's physical-memory ranges live near the top of it, and the graphics
    // path allocates there. It costs address space, not pages -- Commit() faults
    // in only what is used.
    reservedSize_ = PPC_MEMORY_SIZE;
    static_assert(kFuncTableBase + kFuncTableSize <= PPC_MEMORY_SIZE,
        "function table must fit inside the 4 GiB guest window");

    // Reserved without backing pages; Commit() faults in only what is used.
    // MAP_NORESERVE keeps this off the commit charge on overcommit-strict hosts.
    void* p = mmap(nullptr, reservedSize_, PROT_NONE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED)
    {
        lucent::error("mem", "failed to reserve {} bytes of guest address space", reservedSize_);
        return false;
    }

    base_ = static_cast<uint8_t*>(p);

    // PPC_FUNC_PROLOGUE tells Clang to assume this; an unaligned base would
    // silently miscompile every guest function.
    if ((reinterpret_cast<uintptr_t>(base_) & 0x1F) != 0)
    {
        lucent::error("mem", "guest base {} is not 32-byte aligned", static_cast<void*>(base_));
        Release();
        return false;
    }

    lucent::info("mem", "reserved {:.2f} GiB of guest address space at {}",
        double(reservedSize_) / (1024.0 * 1024.0 * 1024.0), static_cast<void*>(base_));

    return MapPhysicalAliases();
}

bool GuestMemory::MapPhysicalAliases()
{
    physicalFd_ = memfd_create("gears-physical", 0);
    if (physicalFd_ < 0)
    {
        lucent::error("mem", "memfd_create failed for physical RAM");
        return false;
    }

    if (ftruncate(physicalFd_, kPhysicalSize) != 0)
    {
        lucent::error("mem", "cannot size physical RAM to {} bytes", kPhysicalSize);
        return false;
    }

    // MAP_SHARED over the same descriptor is what makes the windows alias; the
    // kernel still allocates pages lazily, so mapping all four costs nothing
    // until the guest actually touches them.
    for (uint32_t alias : kPhysicalAliases)
    {
        void* p = mmap(base_ + alias, kPhysicalSize, PROT_READ | PROT_WRITE,
            MAP_SHARED | MAP_FIXED, physicalFd_, 0);
        if (p == MAP_FAILED)
        {
            lucent::error("mem", "cannot map physical RAM at guest {:#x}", alias);
            return false;
        }
    }

    lucent::info("mem", "physical RAM ({} MiB) aliased at {:#x}, {:#x}, {:#x}, {:#x}",
        kPhysicalSize / (1024 * 1024), kPhysicalAliases[0], kPhysicalAliases[1],
        kPhysicalAliases[2], kPhysicalAliases[3]);
    return true;
}

void GuestMemory::Release()
{
    if (base_ != nullptr)
    {
        munmap(base_, reservedSize_);
        base_ = nullptr;
    }

    if (physicalFd_ >= 0)
    {
        close(physicalFd_);
        physicalFd_ = -1;
    }
}

bool GuestMemory::Commit(uint32_t address, uint32_t size)
{
    if (size == 0)
        return true;

    // mprotect needs page-aligned bounds, so widen to the enclosing pages.
    const uintptr_t pageSize = 0x1000;
    uintptr_t begin = uintptr_t(address) & ~(pageSize - 1);
    uintptr_t end = (uintptr_t(address) + size + pageSize - 1) & ~(pageSize - 1);

    if (mprotect(base_ + begin, end - begin, PROT_READ | PROT_WRITE) != 0)
    {
        lucent::error("mem", "failed to commit guest range {:#x}..{:#x}", begin, end);
        return false;
    }

    lucent::debug("mem", "committed {:#x}..{:#x} ({} KiB)", begin, end, (end - begin) / 1024);
    return true;
}

void GuestMemory::Zero(uint32_t address, uint32_t size)
{
    if (size == 0)
        return;
    std::memset(base_ + address, 0, size);
    lucent::debug("mem", "zeroed {:#x}+{:#x}", address, size);
}

namespace
{
GuestMemory* g_memory = nullptr;
}

GuestMemory& Memory()
{
    return *g_memory;
}

void SetMemory(GuestMemory& memory)
{
    g_memory = &memory;
}

size_t InstallFunctionTable(GuestMemory& memory)
{
    if (!memory.Commit(uint32_t(kFuncTableBase), uint32_t(kFuncTableSize)))
        return 0;

    uint8_t* base = memory.Base();
    size_t installed = 0;
    size_t outOfRange = 0;

    for (const PPCFuncMapping* m = PPCFuncMappings; m->host != nullptr; ++m)
    {
        // Imports live outside the .text range the table covers; they are
        // called directly by name from the recompiled code, not through it.
        if (m->guest < PPC_CODE_BASE || m->guest >= PPC_CODE_BASE + PPC_CODE_SIZE)
        {
            ++outOfRange;
            continue;
        }

        PPC_LOOKUP_FUNC(base, m->guest) = m->host;
        ++installed;
    }

    lucent::info("loader", "installed {} functions into the indirect-call table ({} outside .text)",
        installed, outOfRange);
    return installed;
}

} // namespace gears
