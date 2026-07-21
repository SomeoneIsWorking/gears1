#include "guest_thread.h"

#include <byteswap.h>
#include <lucent/log.h>

#include "guest_heap.h"

namespace gears
{

namespace
{
// Offsets within the KPCR that guest code is known to read. Only the fields the
// game actually touches are populated; the rest stays zeroed so an unexpected
// read shows up as a zero rather than as plausible-looking noise.
constexpr uint32_t kPcrTlsPtr = 0x000;
constexpr uint32_t kPcrStackBase = 0x008;
constexpr uint32_t kPcrStackLimit = 0x00C;
constexpr uint32_t kPcrCurrentThread = 0x100;

constexpr uint32_t kPcrSize = 0x1000;
constexpr uint32_t kThreadSize = 0x1000;
constexpr uint32_t kTlsSize = 0x1000;

void Store32(GuestMemory& memory, uint32_t address, uint32_t value)
{
    *memory.Translate<uint32_t>(address) = ByteSwap(value);
}
} // namespace

bool CreateGuestThreadBlock(GuestMemory& memory, uint32_t stackSize, GuestThreadBlock& out)
{
    uint32_t pcrSize = kPcrSize;
    uint32_t threadSize = kThreadSize;
    uint32_t tlsSize = kTlsSize;

    const uint32_t pcr = TitleHeap().Allocate(0, pcrSize, kMemCommit);
    const uint32_t thread = TitleHeap().Allocate(0, threadSize, kMemCommit);
    const uint32_t tls = TitleHeap().Allocate(0, tlsSize, kMemCommit);
    uint32_t stack = TitleHeap().Allocate(0, stackSize, kMemCommit);

    if (pcr == 0 || thread == 0 || tls == 0 || stack == 0)
    {
        lucent::error("thread", "could not allocate the guest thread block");
        return false;
    }

    out.pcrAddress = pcr;
    out.threadAddress = thread;
    out.stackLimit = stack;
    out.stackBase = stack + stackSize;

    Store32(memory, pcr + kPcrTlsPtr, tls);
    Store32(memory, pcr + kPcrStackBase, out.stackBase);
    Store32(memory, pcr + kPcrStackLimit, out.stackLimit);
    Store32(memory, pcr + kPcrCurrentThread, thread);

    lucent::info("thread", "thread block: pcr={:#x} thread={:#x} tls={:#x} stack={:#x}..{:#x}",
        pcr, thread, tls, out.stackLimit, out.stackBase);
    return true;
}

} // namespace gears
