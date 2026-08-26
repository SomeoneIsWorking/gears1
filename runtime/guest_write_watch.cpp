#include "guest_write_watch.h"

#include "fault_report.h"
#include "guest_memory.h"

#include <array>
#include <atomic>
#include <csignal>
#include <dlfcn.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

#include <lucent/config.h>
#include <lucent/log.h>

namespace gears
{
namespace
{

constexpr size_t kWatchSlots = 16;

struct WatchSlot
{
    std::atomic<uintptr_t> instruction{0};
    std::atomic<uint64_t> count{0};
};

struct GuestWriteWatch
{
    std::atomic<bool> armed{false};
    GuestWriteWatchOwner owner = GuestWriteWatchOwner::kQueue;
    std::array<uint8_t *, GuestMemory::kAliasCount> pages{};
    std::array<uint8_t *, GuestMemory::kAliasCount> targets{};
    size_t aliasCount = 0;
    size_t pageSize = 0;
    uint32_t guestTarget = 0;
    uint64_t targetSampleLimit = 0;
    std::atomic<uint64_t> hits{0};
    std::atomic<uint64_t> otherFaults{0};
    WatchSlot slots[kWatchSlots]{};
    uintptr_t moduleBase = 0;
    struct sigaction oldTrap{};
    bool reported = false;
} g_watch;

static_assert(std::atomic<bool>::is_always_lock_free);
static_assert(std::atomic<uintptr_t>::is_always_lock_free);
static_assert(std::atomic<uint64_t>::is_always_lock_free);

thread_local uint8_t *g_singleStepPage = nullptr;

const char *OwnerName(GuestWriteWatchOwner owner)
{
    switch (owner)
    {
    case GuestWriteWatchOwner::kQueue:
        return "queue";
    case GuestWriteWatchOwner::kDrawPacket:
        return "draw-packet";
    }
    return "unknown";
}

void RecordInstruction(uintptr_t instruction)
{
    for (WatchSlot &slot : g_watch.slots)
    {
        uintptr_t observed = slot.instruction.load(std::memory_order_relaxed);
        if (observed == instruction)
        {
            slot.count.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (observed == 0 && slot.instruction.compare_exchange_strong(observed, instruction,
                                                                      std::memory_order_relaxed))
        {
            slot.count.store(1, std::memory_order_relaxed);
            return;
        }
    }
}

void ProtectWatchPages(int protection)
{
    for (size_t i = 0; i < g_watch.aliasCount; ++i)
        mprotect(g_watch.pages[i], g_watch.pageSize, protection);
}

bool OnSegv(uintptr_t fault, void *context) noexcept
{
    size_t alias = g_watch.aliasCount;
    for (size_t i = 0; i < g_watch.aliasCount; ++i)
    {
        const auto page = reinterpret_cast<uintptr_t>(g_watch.pages[i]);
        if (fault >= page && fault - page < g_watch.pageSize)
        {
            alias = i;
            break;
        }
    }
    if (!g_watch.armed.load(std::memory_order_acquire) || alias == g_watch.aliasCount)
        return false;

    auto *machine = static_cast<ucontext_t *>(context);
    if (GuestWriteWatchContains(reinterpret_cast<uintptr_t>(g_watch.targets[alias]),
                                sizeof(uint32_t), fault))
    {
        RecordInstruction(uintptr_t(machine->uc_mcontext.gregs[REG_RIP]));
        g_watch.hits.fetch_add(1, std::memory_order_relaxed);
    }
    else
        g_watch.otherFaults.fetch_add(1, std::memory_order_relaxed);

    g_singleStepPage = g_watch.pages[alias];
    mprotect(g_singleStepPage, g_watch.pageSize, PROT_READ | PROT_WRITE);
    if (g_watch.hits.load(std::memory_order_relaxed) >= g_watch.targetSampleLimit)
    {
        g_watch.armed.store(false, std::memory_order_release);
        ProtectWatchPages(PROT_READ | PROT_WRITE);
        g_singleStepPage = nullptr;
        return true;
    }
    machine->uc_mcontext.gregs[REG_EFL] |= 0x100;
    return true;
}

void DispatchPreviousTrap(int signal, siginfo_t *info, void *context)
{
    if (g_watch.oldTrap.sa_flags & SA_SIGINFO)
    {
        if (g_watch.oldTrap.sa_sigaction != nullptr)
        {
            g_watch.oldTrap.sa_sigaction(signal, info, context);
            return;
        }
    }
    else if (g_watch.oldTrap.sa_handler != SIG_DFL && g_watch.oldTrap.sa_handler != SIG_IGN &&
             g_watch.oldTrap.sa_handler != nullptr)
    {
        g_watch.oldTrap.sa_handler(signal);
        return;
    }

    struct sigaction restore{};
    restore.sa_handler = SIG_DFL;
    sigemptyset(&restore.sa_mask);
    sigaction(signal, &restore, nullptr);
    raise(signal);
}

void OnTrap(int signal, siginfo_t *info, void *context)
{
    if (g_singleStepPage == nullptr)
    {
        DispatchPreviousTrap(signal, info, context);
        return;
    }

    auto *machine = static_cast<ucontext_t *>(context);
    machine->uc_mcontext.gregs[REG_EFL] &= ~0x100;
    if (g_watch.armed.load(std::memory_order_acquire) && g_singleStepPage != nullptr)
        mprotect(g_singleStepPage, g_watch.pageSize, PROT_READ);
    g_singleStepPage = nullptr;
}

void ClearSamples()
{
    for (WatchSlot &slot : g_watch.slots)
    {
        slot.instruction.store(0, std::memory_order_relaxed);
        slot.count.store(0, std::memory_order_relaxed);
    }
    g_watch.hits.store(0, std::memory_order_relaxed);
    g_watch.otherFaults.store(0, std::memory_order_relaxed);
}

} // namespace

bool ArmGuestWriteWatch(GuestWriteWatchOwner owner, uint32_t guestAddress,
                        uint64_t targetSampleLimit)
{
    if (guestAddress == 0 || targetSampleLimit == 0)
        return false;
    if (g_watch.aliasCount != 0)
    {
        if (g_watch.owner != owner)
            lucent::error("hle",
                          "{} write watch refused: {} already owns the process-wide"
                          " guest write watch",
                          OwnerName(owner), OwnerName(g_watch.owner));
        return g_watch.owner == owner && g_watch.guestTarget == guestAddress;
    }

    GuestMemory &memory = Memory();
    const size_t pageSize = size_t(sysconf(_SC_PAGESIZE));
    g_watch.owner = owner;
    g_watch.pageSize = pageSize;
    g_watch.guestTarget = guestAddress;
    g_watch.targetSampleLimit = targetSampleLimit;

    bool isPhysicalAlias = false;
    for (size_t i = 0; i < GuestMemory::kAliasCount; ++i)
    {
        const uint64_t alias = memory.AliasOffset(i);
        if (uint64_t(guestAddress) >= alias &&
            uint64_t(guestAddress) - alias <= GuestMemory::kAliasMask)
        {
            isPhysicalAlias = true;
            break;
        }
    }
    const uint32_t physical = guestAddress & GuestMemory::kAliasMask;
    g_watch.aliasCount = isPhysicalAlias ? GuestMemory::kAliasCount : 1;
    for (size_t i = 0; i < g_watch.aliasCount; ++i)
    {
        const uint32_t address =
            isPhysicalAlias ? uint32_t(memory.AliasOffset(i)) + physical : guestAddress;
        uint8_t *target = memory.Translate<uint8_t>(address);
        g_watch.targets[i] = target;
        g_watch.pages[i] = target - (address % pageSize);
    }

    Dl_info info{};
    if (dladdr(reinterpret_cast<void *>(&ArmGuestWriteWatch), &info))
        g_watch.moduleBase = uintptr_t(info.dli_fbase);

    if (!RegisterSegvObserver(&OnSegv))
    {
        lucent::error("hle",
                      "{} write watch refused: the fault reporter already"
                      " has a different SIGSEGV diagnostic observer",
                      OwnerName(owner));
        g_watch.aliasCount = 0;
        return false;
    }

    struct sigaction trap{};
    trap.sa_sigaction = &OnTrap;
    trap.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&trap.sa_mask);
    sigaction(SIGTRAP, &trap, &g_watch.oldTrap);

    g_watch.armed.store(true, std::memory_order_release);
    ProtectWatchPages(PROT_READ);
    lucent::info("hle",
                 "{} write watch armed on guest {:#x} across {} host alias page(s)"
                 " (module base {:#x})",
                 OwnerName(owner), guestAddress, g_watch.aliasCount, g_watch.moduleBase);
    return true;
}

bool ReportGuestWriteWatch(GuestWriteWatchOwner owner, bool rearm)
{
    if (g_watch.aliasCount == 0 || g_watch.owner != owner || (g_watch.reported && !rearm))
        return false;
    const uint64_t hits = g_watch.hits.load(std::memory_order_relaxed);
    const uint64_t otherFaults = g_watch.otherFaults.load(std::memory_order_relaxed);
    if ((!rearm && hits == 0) || (hits == 0 && otherFaults == 0))
        return false;

    lucent::Line line;
    line.add("{} write watch guest {:#x}: {} target writes, {} other faults on the page",
             OwnerName(owner), g_watch.guestTarget, hits, otherFaults);
    for (const WatchSlot &slot : g_watch.slots)
    {
        const uintptr_t instruction = slot.instruction.load(std::memory_order_relaxed);
        if (instruction == 0)
            break;
        line.add("  rip {:#x} (+{:#x}) x{}", instruction, instruction - g_watch.moduleBase,
                 slot.count.load(std::memory_order_relaxed));
    }
    line.flush(rearm ? lucent::Level::Debug : lucent::Level::Info, "hle");

    if (rearm)
    {
        ClearSamples();
        g_watch.armed.store(true, std::memory_order_release);
        ProtectWatchPages(PROT_READ);
    }
    else if (hits != 0)
    {
        g_watch.reported = true;
        g_watch.armed.store(false, std::memory_order_release);
        ProtectWatchPages(PROT_READ | PROT_WRITE);
    }
    return hits != 0;
}

GuestWriteWatchStats CurrentGuestWriteWatchStats(GuestWriteWatchOwner owner)
{
    if (g_watch.aliasCount == 0 || g_watch.owner != owner)
        return {};
    return {.armed = g_watch.armed.load(std::memory_order_acquire),
            .aliasPages = g_watch.aliasCount,
            .targetWrites = g_watch.hits.load(std::memory_order_relaxed),
            .otherPageWrites = g_watch.otherFaults.load(std::memory_order_relaxed)};
}

bool DrawPacketWatchSelector::Observe(uint32_t swapSequence, int depth, uint32_t afterSwap,
                                      uint32_t ordinal)
{
    if (done || depth <= 0 || swapSequence < afterSwap)
        return false;
    if (!frameSelected || swapSequence != frameSequence)
    {
        frameSelected = true;
        frameSequence = swapSequence;
        ordinalSeen = 0;
    }
    if (ordinalSeen++ != ordinal)
        return false;
    done = true;
    return true;
}

void MaybeArmDrawPacketWriteWatch(uint32_t sourceBase, uint32_t sourceIndex, int depth,
                                  uint32_t swapSequence)
{
    static DrawPacketWatchSelector selector;
    static const bool enabled = lucent::config::flag("WATCH_DRAW_PACKET");
    if (!enabled)
        return;
    const uint32_t afterSwap = uint32_t(lucent::config::number("WATCH_DRAW_PACKET_AFTER_SWAP", 0));
    const uint32_t ordinal = uint32_t(lucent::config::number("WATCH_DRAW_PACKET_ORDINAL", 0));
    if (!selector.Observe(swapSequence, depth, afterSwap, ordinal))
        return;
    ArmGuestWriteWatch(GuestWriteWatchOwner::kDrawPacket,
                       sourceBase + sourceIndex * sizeof(uint32_t), 1);
}

void ReportDrawPacketWriteWatch()
{
    ReportGuestWriteWatch(GuestWriteWatchOwner::kDrawPacket, false);
}

} // namespace gears
