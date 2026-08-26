#include "guest_dirty_pages.h"

// Linux soft-dirty page tracking. See the header for the contract; this file
// holds only the kernel interface. The fallback everywhere is "dirty", which
// makes every caller behave exactly as it did before this module existed.

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include <lucent/log.h>

namespace gears
{
namespace
{

constexpr uint64_t kPageSize = 4096;

// The registered live guest-memory layout. Empty until RegisterGuestAliasLayout
// runs, which is the normal state of a replay tool.
const void *g_liveBase = nullptr;
std::vector<uint64_t> g_liveWindows;
uint64_t g_liveAliasMask = 0;

} // namespace

void RegisterGuestAliasLayout(const void *base, std::vector<uint64_t> windows, uint64_t aliasMask)
{
    g_liveBase = base;
    g_liveWindows = std::move(windows);
    g_liveAliasMask = aliasMask;
}

bool StalenessWindowsFor(const void *base, std::vector<uint64_t> &windows, uint64_t &aliasMask)
{
    windows.clear();
    aliasMask = 0;
    if (base == nullptr)
        return false;
    if (base == g_liveBase && !g_liveWindows.empty())
    {
        windows = g_liveWindows;
        aliasMask = g_liveAliasMask;
    }
    else
    {
        // Not the live reservation: one flat window, no aliasing.
        windows.push_back(0);
    }
    return true;
}

int GuestDirtyPages::OpenPagemap()
{
    return ::open("/proc/self/pagemap", O_RDONLY);
}

bool GuestDirtyPages::ReadEntries(uint64_t firstPage, size_t count,
                                  std::vector<uint64_t> &out) const
{
    out.resize(count);
    const size_t want = count * sizeof(uint64_t);
    ssize_t got;
    do
    {
        got = ::pread(fd_, out.data(), want, off_t(firstPage) * off_t(sizeof(uint64_t)));
    } while (got < 0 && errno == EINTR);
    if (got < 0 || size_t(got) != want)
    {
        ++preadShort;
        return false;
    }
    return true;
}

bool GuestDirtyPages::Probe()
{
    // Two pages, deliberately far enough apart to be two PTEs and close enough
    // to have been faulted by one allocation. The sequence drives BOTH answer
    // classes: after a clear, a touched page must read clean (the clear works)
    // and an untouched page must read clean (absence of writes is visible);
    // after touching one page again, that page must read dirty while its
    // neighbour must STILL read clean (a store to one page does not smear).
    // Any other outcome refuses support rather than guessing.
    //
    // Raw mmap, not a container: a heap allocation shares arena state with the
    // rest of the process, which has no bearing on what is being measured here
    // and would only add noise to a go/no-go decision.
    uint8_t *probe = static_cast<uint8_t *>(
        ::mmap(nullptr, 2 * kPageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (probe == MAP_FAILED)
        return false;
    const uintptr_t first = reinterpret_cast<uintptr_t>(probe);
    probe[0] = 1;

    BeginObservationPeriod();
    bool ok = clearFailures == 0;

    std::vector<uint64_t> entries;
    ok = ok && ReadEntries(first / kPageSize, 2, entries);
    ok = ok && !PagemapEntrySoftDirty(entries[0]) && !PagemapEntrySoftDirty(entries[1]);

    probe[0] = 2;
    ok = ok && ReadEntries(first / kPageSize, 2, entries);
    ok = ok && PagemapEntrySoftDirty(entries[0]) && !PagemapEntrySoftDirty(entries[1]);

    ::munmap(probe, 2 * kPageSize);
    if (!ok)
        return false;

    // Leave the observation period consistent with a fresh clear.
    BeginObservationPeriod();
    return clearFailures == 0;
}

bool GuestDirtyPages::Open(const uint8_t *base, std::vector<uint64_t> windows, uint64_t aliasMask)
{
    if (base == nullptr || windows.empty())
        return false;

    // Re-opening against a different buffer (a replay tool handing over its
    // capture, say) must not leak the previous descriptors.
    if (fd_ >= 0)
        ::close(fd_);
    if (clearFd_ >= 0)
        ::close(clearFd_);
    fd_ = -1;
    clearFd_ = -1;
    probeOk_ = false;

    fd_ = OpenPagemap();
    clearFd_ = ::open("/proc/self/clear_refs", O_WRONLY | O_APPEND);
    if (fd_ < 0 || clearFd_ < 0)
    {
        lucent::warn("mem",
                     "soft-dirty tracking unavailable: pagemap {},"
                     " clear_refs {}",
                     fd_ >= 0 ? "ok" : "failed", clearFd_ >= 0 ? "ok" : "failed");
        return false;
    }

    base_ = base;
    windows_ = std::move(windows);
    aliasMask_ = aliasMask;
    probeOk_ = Probe();
    if (!probeOk_)
    {
        lucent::warn("mem", "soft-dirty tracking PROBE FAILED on this kernel: it could"
                            " not demonstrate both a clean page staying clean and a"
                            " written page turning dirty. Skipping texture hashes would"
                            " trust an unproven instrument, so it stays OFF and every"
                            " texture keeps re-hashing");
        base_ = nullptr;
        windows_.clear();
    }
    else
    {
        lucent::info("mem",
                     "soft-dirty tracking armed: touched-page-clean and"
                     " touched-page-dirty both demonstrated against"
                     " /proc/self/pagemap across {} window(s)",
                     windows_.size());
    }
    return probeOk_;
}

void GuestDirtyPages::BeginObservationPeriod()
{
    ++generation_;
    if (clearFd_ < 0)
        return;
    const char four[] = "4\n";
    const ssize_t wrote = ::write(clearFd_, four, 2);
    if (wrote != 2)
    {
        ++clearFailures;
        if (clearFailures == 1 || (clearFailures & (clearFailures - 1)) == 0)
            lucent::warn("mem", "soft-dirty clear failed ({})", clearFailures);
    }
}

bool GuestDirtyPages::RangeCleanSinceLastClear(uint64_t offsetFromBase, size_t bytes) const
{
    if (!Supported() || base_ == nullptr || bytes == 0)
        return false;

    ++spansQueried;
    const uint64_t phys = aliasMask_ != 0 ? (offsetFromBase & aliasMask_) : offsetFromBase;

    std::vector<uint64_t> entries;
    for (const uint64_t w : windows_)
    {
        // Pagemap is indexed by ABSOLUTE virtual page number: the window
        // offset alone would silently describe the bottom of the address
        // space, which maps nothing and therefore always reads clean.
        const uint64_t va = reinterpret_cast<uint64_t>(base_) + w + phys;
        const uint64_t firstPage = va / kPageSize;
        // Widen to enclosing pages: a span ending mid-page must consult that
        // whole page, because the bit belongs to it.
        const uint64_t lastPage = (va + bytes - 1) / kPageSize;
        const size_t count = size_t(lastPage - firstPage + 1);
        pagesRead += count;
        if (!ReadEntries(firstPage, count, entries))
            return false;
        for (const uint64_t e : entries)
            if (PagemapEntrySoftDirty(e))
            {
                ++spansDirty;
                return false;
            }
    }
    return true;
}

} // namespace gears
