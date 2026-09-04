#pragma once

#include <cstdint>
#include <cstdlib>

#include <lucent/log.h>

namespace gears
{

// Retained native subsystems still expose guest callback points, but there is
// deliberately no local dispatch implementation. The only valid implementation
// belongs to x360port once it embeds Xenia's Processor and ThreadState.
[[noreturn]] inline void RefuseMissingX360PortExecutor(uint32_t guestAddress)
{
    lucent::critical("executor",
                     "guest call {:#x} refused: shared/x360port exposes no "
                     "Xenia executor",
                     guestAddress);
    std::abort();
}

} // namespace gears
