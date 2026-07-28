// The device-store hook's implementation. See ppc_mmio.h for why it exists.
#include "ppc_mmio.h"

#include <byteswap.h>
#include <lucent/log.h>

#include "guest_memory.h"
#include "xma.h"

namespace gears
{

void DeviceStore32(uint32_t address, uint32_t value)
{
    // The store still happens, exactly as PPC_STORE_U32 would do it. Devices
    // here are backed by real memory that the guest reads back with ordinary
    // loads, so intercepting the write must not swallow it -- the hook adds an
    // observer, it does not replace the memory.
    *Memory().Translate<uint32_t>(address) = ByteSwap(value);

    if (OnXmaRegisterStore(address, value))
        return;

    // Everything else in the device window is inert memory, which is correct
    // for the blocks that are only ever read back (the ring write pointer the
    // command processor polls, among them). Logged at debug so a store to a
    // block nobody models is findable rather than invisible.
    lucent::debug("mmio", "device store {:#x} <- {:#x}", address, value);
}

} // namespace gears
