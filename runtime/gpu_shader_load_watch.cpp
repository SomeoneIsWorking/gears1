#include "gpu_shader_load_watch.h"

#include "guest_backtrace.h"
#include "guest_memory.h"
#include "guest_write_watch.h"

#include <atomic>
#include <charconv>
#include <string>
#include <system_error>
#include <utility>

#include <lucent/config.h>
#include <lucent/log.h>

namespace gears
{
namespace
{

struct ShaderLoadWatchState
{
    std::atomic<std::uint32_t> packetGuestAddress{0};
    std::atomic<bool> armAttempted{false};
    std::atomic<bool> copyReported{false};
    std::atomic<bool> writeReported{false};
};

ShaderLoadWatchState g_state;
thread_local bool g_copyObservedInCurrentCall = false;

const std::optional<std::uint64_t> &ConfiguredShaderLoadWatchHash()
{
    static const std::optional<std::uint64_t> hash = []
    {
        const std::string &configured = lucent::config::text("WATCH_SHADER_LOAD_HASH");
        if (configured.empty())
            return std::optional<std::uint64_t>{};
        const std::optional<std::uint64_t> parsed = ParseShaderLoadWatchHash(configured);
        if (!parsed)
            lucent::error("hle",
                          "WATCH_SHADER_LOAD_HASH must be one hexadecimal 64-bit hash, got {}",
                          configured);
        return parsed;
    }();
    return hash;
}

} // namespace

std::optional<std::uint64_t> ParseShaderLoadWatchHash(std::string_view text)
{
    if (text.starts_with("0x") || text.starts_with("0X"))
        text.remove_prefix(2);
    if (text.empty())
        return std::nullopt;

    std::uint64_t hash = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), hash, 16);
    if (error != std::errc{} || end != text.data() + text.size())
        return std::nullopt;
    return hash;
}

bool ShaderLoadWatchCopyCoversTarget(std::uint32_t destination, std::uint32_t bytes,
                                     std::uint32_t target)
{
    if (bytes == 0)
        return false;
    const std::uint32_t physicalDestination = destination & GuestMemory::kAliasMask;
    const std::uint32_t physicalTarget = target & GuestMemory::kAliasMask;
    return physicalTarget >= physicalDestination &&
           std::uint64_t(physicalTarget) - physicalDestination < bytes;
}

bool ShaderLoadPacketWatchEnabled()
{
    return ConfiguredShaderLoadWatchHash().has_value();
}

void ObserveShaderLoadPacketWrite(std::uint64_t shaderHash, std::uint32_t packetGuestAddress,
                                  bool immediate)
{
    const std::optional<std::uint64_t> &configured = ConfiguredShaderLoadWatchHash();
    if (!configured || *configured != shaderHash ||
        g_state.writeReported.load(std::memory_order_acquire))
        return;
    bool expected = false;
    if (g_state.armAttempted.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
    {
        if (!ArmGuestWriteWatch(GuestWriteWatchOwner::kShaderLoadPacket, packetGuestAddress, 1))
            return;
        g_state.packetGuestAddress.store(packetGuestAddress, std::memory_order_release);
        lucent::info("hle", "shader-load packet watch selected {} shader {:#018x} at guest {:#x}",
                     immediate ? "inline" : "memory-backed", shaderHash, packetGuestAddress);
    }
    if (ReportGuestWriteWatch(GuestWriteWatchOwner::kShaderLoadPacket, false))
        g_state.writeReported.store(true, std::memory_order_release);
}

void ObserveShaderLoadPacketCopy(std::uint32_t destination, std::uint32_t bytes,
                                 std::uint32_t caller)
{
    const std::uint32_t target = g_state.packetGuestAddress.load(std::memory_order_acquire);
    if (target == 0 || !ShaderLoadWatchCopyCoversTarget(destination, bytes, target))
        return;
    bool expected = false;
    if (g_state.copyReported.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
    {
        g_copyObservedInCurrentCall = true;
        lucent::info("hle",
                     "shader-load packet copy covers guest {:#x} for {} bytes from caller {:#x}",
                     target, bytes, caller);
    }
}

void ReportShaderLoadPacketProducerReturn(std::uint32_t caller, std::uint32_t stackPointer)
{
    if (!std::exchange(g_copyObservedInCurrentCall, false))
        return;
    lucent::info("hle", "shader-load packet producer returned to caller {:#x}; guest stack: {}",
                 caller, FormatGuestBacktrace(stackPointer, caller));
}

} // namespace gears
