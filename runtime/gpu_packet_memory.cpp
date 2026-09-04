#include "gpu_packet_memory.h"

#include "byte_order.h"

#include <lucent/log.h>

#include "guest_memory.h"
#include "gpu_ticket_wait.h"
#include "render_thread.h"

namespace gears
{

namespace
{

uint32_t ReadGuest32(uint32_t address)
{
    return ByteSwap(*Memory().Translate<uint32_t>(address));
}

void StoreGuest32(uint32_t address, uint32_t value)
{
    if (address != 0)
        *Memory().Translate<uint32_t>(address) = ByteSwap(value);
}

} // namespace

uint32_t LoadGpuPacketWord(uint32_t addressWord)
{
    const uint32_t address = addressWord & ~3u;
    const uint32_t endian = addressWord & 3u;
    if (endian == 0)
        return *Memory().Translate<uint32_t>(address);
    return ReadGuest32(address);
}

void StoreGpuPacketWord(uint32_t addressWord, uint32_t value)
{
    const uint32_t address = addressWord & ~3u;
    const uint32_t endian = addressWord & 3u;
    if (endian == 0)
        *Memory().Translate<uint32_t>(address) = value;
    else if (endian == 2)
        StoreGuest32(address, value);
    else
    {
        lucent::warn("gpu", "unhandled endian mode {} storing to {:#x}", endian, address);
        StoreGuest32(address, value);
    }
    NotifyGpuPacketMemoryChanged(address);
}

void DeferGpuRetirementWrite(uint32_t addressWord, uint32_t value, uint32_t packetBase,
                             uint32_t packetIndex)
{
    DeferUntilAcceptedRenderRetires(
        [addressWord, value, packetBase, packetIndex]
        {
            StoreGpuPacketWord(addressWord, value);
            lucent::debug("gpu", "EVENT_WRITE_SHD {:#x} <- {:#x} (from {:#x}[{:#x}])",
                          addressWord & ~3u, value, packetBase, packetIndex);
        });
}

} // namespace gears
