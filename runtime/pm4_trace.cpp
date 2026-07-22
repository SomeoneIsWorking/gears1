// A read-only walk of the GPU command stream.
#include <algorithm>
//
// This executes nothing. Its purpose is to answer one question with evidence
// rather than assumption: which packet writes the word D3D waits on.
//
// That word (observed at 0xA030A000) is never handed to the runtime by any Vd*
// registration -- the addresses those give us are a different word each. So it
// can only be named by a packet the title wrote into the stream, and the only
// way to know which is to read the stream.
//
// Packet formats, Xenos PM4:
//   TYPE0 (31:30 = 0)  register write; 29:16 = count-1, 14:0 = base register
//   TYPE1 (31:30 = 1)  two register writes
//   TYPE2 (31:30 = 2)  no-op, header only
//   TYPE3 (31:30 = 3)  command; 29:16 = count-1, 15:8 = opcode
#include <cstdint>
#include <vector>

#include <lucent/log.h>

#include <byteswap.h>

#include "guest_memory.h"

namespace gears
{

namespace
{
constexpr uint32_t kIndirectBuffer = 0x3F;

// Physical addresses reach the same memory through several windows, so a
// candidate is compared on the offset that survives the aliasing.
uint32_t PhysicalOffset(uint32_t address)
{
    return address & 0x1FFFFFFF;
}

const char* OpcodeName(uint32_t opcode)
{
    switch (opcode)
    {
    case 0x3F: return "INDIRECT_BUFFER";
    case 0x3D: return "MEM_WRITE";
    case 0x46: return "EVENT_WRITE";
    case 0x22: return "COND_EXEC";
    case 0x2D: return "SET_CONSTANT";
    case 0x10: return "NOP";
    case 0x48: return "ME_INIT";
    case 0x3C: return "WAIT_REG_MEM";
    case 0x54: return "INTERRUPT";
    case 0x58: return "EVENT_WRITE_SHD";
    default: return "?";
    }
}

uint32_t Read(uint32_t address)
{
    return ByteSwap(*Memory().Translate<uint32_t>(address));
}

void Walk(uint32_t base, uint32_t words, uint32_t watchAddress, int depth,
    std::vector<uint32_t>& visited)
{
    if (depth > 4)
        return; // indirect buffers nest, but not deeply; this guards a cycle

    for (uint32_t i = 0; i < words;)
    {
        const uint32_t header = Read(base + i * 4);
        const uint32_t type = header >> 30;
        ++i;

        if (header == 0 || type == 2)
            continue; // padding or no-op

        const uint32_t count = ((header >> 16) & 0x3FFF) + 1;

        if (type == 0 || type == 1)
        {
            // Register writes are inspected rather than skipped. A fence
            // address is normally programmed into a scratch-address register,
            // so it reaches the GPU as a TYPE0 payload and never appears in a
            // TYPE3 data word -- which is why an earlier walk that skipped
            // these found nothing.
            const uint32_t baseRegister = header & 0x7FFF;
            for (uint32_t w = 0; w < count && i + w < words; w++)
            {
                const uint32_t value = Read(base + (i + w) * 4);
                const uint32_t reg = baseRegister + w;
                // The scratch registers (0x578..0x57F) and the writeback
                // control pair (SCRATCH_UMSK 0x1DC / SCRATCH_ADDR 0x1DD) are
                // the retirement protocol: values written to a masked scratch
                // register are copied by the GPU to SCRATCH_ADDR + 4n. Report
                // them unconditionally -- they carry submission ids and
                // callback pointers, never the fence address itself, which is
                // why a watch on the address alone cannot see them.
                if (reg >= 0x578 && reg <= 0x57F)
                {
                    lucent::info("pm4", "SCRATCH_REG{} <- {:#010x} (TYPE0 at {:#x})",
                        reg - 0x578, value, base + (i - 1) * 4);
                }
                else if (reg == 0x1DC || reg == 0x1DD)
                {
                    lucent::info("pm4", "{} <- {:#010x} (TYPE0 at {:#x})",
                        reg == 0x1DC ? "SCRATCH_UMSK" : "SCRATCH_ADDR", value,
                        base + (i - 1) * 4);
                }
                if (PhysicalOffset(value) == PhysicalOffset(watchAddress))
                {
                    lucent::info("pm4", "TYPE{} register write at {:#x} puts the watched"
                        " address in register {:#x}", type, base + (i - 1) * 4,
                        baseRegister + w);
                }
            }
            i += count;
            continue;
        }

        const uint32_t opcode = (header >> 8) & 0x7F;

        if (opcode == 0x40)
        {
            lucent::info("pm4", "INTERRUPT packet at {:#x}, data {:#010x}",
                base + (i - 1) * 4, count >= 1 ? Read(base + i * 4) : 0u);
        }

        // Any packet carrying the watched address is the one being looked for,
        // whichever opcode it turns out to be -- reported rather than assumed
        // from a table of what usually writes fences.
        for (uint32_t w = 0; w < count && i + w < words; w++)
        {
            const uint32_t value = Read(base + (i + w) * 4);
            if (PhysicalOffset(value) == PhysicalOffset(watchAddress))
            {
                lucent::info("pm4", "packet {:#x} ({}) at {:#x} carries the watched address"
                    " in word {}", opcode, OpcodeName(opcode), base + (i - 1) * 4, w);
                for (uint32_t d = 0; d < count && i + d < words; d++)
                    lucent::info("pm4", "    data[{}] = {:#010x}", d, Read(base + (i + d) * 4));
                break;
            }
        }

        if (opcode == kIndirectBuffer && count >= 2)
        {
            const uint32_t address = Read(base + i * 4);
            const uint32_t size = Read(base + (i + 1) * 4);
            if (address != 0 && size != 0 && size < 0x40000)
            {
                if (std::find(visited.begin(), visited.end(), address) == visited.end())
                {
                    visited.push_back(address);
                    Walk(address, size, watchAddress, depth + 1, visited);
                }
            }
        }

        i += count;
    }
}
} // namespace

// Reports every packet in the ring, and in the buffers it points at, that
// mentions `watchAddress`.
void TraceCommandStream(uint32_t ringBase, uint32_t ringWords, uint32_t watchAddress)
{
    lucent::info("pm4", "walking ring {:#x} ({} words) for writes to {:#x}",
        ringBase, ringWords, watchAddress);
    std::vector<uint32_t> visited;
    Walk(ringBase, ringWords, watchAddress, 0, visited);
    lucent::info("pm4", "walk complete, {} indirect buffers followed", visited.size());
}

} // namespace gears
