#pragma once

#include <cstdint>

namespace gears
{

// PM4 packet addresses encode the Xenos endian mode in their low two bits.
uint32_t LoadGpuPacketWord(uint32_t addressWord);
void StoreGpuPacketWord(uint32_t addressWord, uint32_t value);

// EVENT_WRITE_SHD publishes its value only after the accepted render generation
// preceding the packet has retired. The command processor itself never waits.
void DeferGpuRetirementWrite(uint32_t addressWord, uint32_t value, uint32_t packetBase,
                             uint32_t packetIndex);

} // namespace gears
