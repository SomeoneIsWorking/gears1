#include "gpu_ticket_wait.h"
#include "gpu_ticket_wait_state.h"
#include "guest_state_memory.h"
#include "import_stub.h"

#include <atomic>
#include <chrono>
#include <cstdint>

#include <lucent/config.h>
#include <lucent/log.h>

namespace
{

using gears::titles::gears1::GuestStateMemory;

constexpr std::uint32_t kGpuTicketWaitOperation = 3;

struct WaitEpisode
{
    std::uint32_t state = 0;
    std::uint32_t initialTick = 0;
    std::uint32_t initialTimeBase = 0;
    bool native = false;
};

std::atomic<std::uint64_t> g_episodeOrdinal{0};
thread_local WaitEpisode t_episode;

[[nodiscard]] bool RecompiledGpuTicketWaitRequested()
{
    static const bool requested = lucent::config::flag("RECOMP_GPU_TICKET_WAIT");
    return requested;
}

[[nodiscard]] bool GpuTicketWaitAbEnabled()
{
    static const bool enabled = lucent::config::flag("GPU_TICKET_WAIT_AB");
    return enabled;
}

[[nodiscard]] bool SelectNativeEpisode(std::uint32_t state,
                                       const gears::titles::gears1::GpuTicketWaitState &snapshot)
{
    if (t_episode.state != state || t_episode.initialTick != snapshot.initialTick ||
        t_episode.initialTimeBase != snapshot.initialTimeBase)
    {
        const std::uint64_t ordinal = g_episodeOrdinal.fetch_add(1, std::memory_order_relaxed) + 1;
        t_episode = {
            .state = state,
            .initialTick = snapshot.initialTick,
            .initialTimeBase = snapshot.initialTimeBase,
            .native = !GpuTicketWaitAbEnabled() || (ordinal & 1u) != 0,
        };
    }
    return t_episode.native;
}

[[nodiscard]] bool WaitForTicketPublication(PPCContext &ctx, std::uint8_t *base)
{
    const std::uint32_t state = ctx.r3.u32;
    const GuestStateMemory memory(base);
    const auto snapshot = gears::titles::gears1::ReadGpuTicketWaitState(memory, state, ctx.r13.u32);
    if (!snapshot.has_value() || snapshot->operation != kGpuTicketWaitOperation ||
        !SelectNativeEpisode(state, *snapshot))
    {
        return false;
    }

    // The retained helper exits before touching the ticket when this bit says
    // the caller owns the wait. Do not turn that non-blocking contract into a
    // host wait merely because the native arm can observe the ticket address.
    if (snapshot->owningThreadExempt)
        return true;

    const gears::GpuPacketMemoryObservation observation =
        gears::ObserveGpuPacketMemory(snapshot->progressAddress);
    const std::uint32_t servedTicket = memory.Read32(snapshot->progressAddress);
    if (servedTicket != snapshot->lastProgress)
        return true;

    const std::chrono::milliseconds remaining =
        snapshot->refreshDeadline
            ? std::chrono::milliseconds{gears::kGpuTicketHangTimeoutMilliseconds}
            : gears::RemainingGpuTicketWait(snapshot->currentTick, snapshot->lastProgressTick);
    if (remaining == std::chrono::milliseconds::zero())
        return true;

    (void)gears::WaitForGpuPacketMemoryChange(observation,
                                              std::chrono::steady_clock::now() + remaining);
    return true;
}

} // namespace

extern "C" PPC_FUNC(__imp__sub_8222F460);
PPC_FUNC(sub_8222F460)
{
    if (RecompiledGpuTicketWaitRequested())
    {
        __imp__sub_8222F460(ctx, base);
        return;
    }

    // The retained helper remains the authority for progress accounting,
    // owning-thread exemptions, the 5 s hang escalation, and its return value.
    // The native seam only blocks until its producer can have changed state,
    // replacing millions of translated delay/poll iterations with one host wait.
    (void)WaitForTicketPublication(ctx, base);
    __imp__sub_8222F460(ctx, base);
}
