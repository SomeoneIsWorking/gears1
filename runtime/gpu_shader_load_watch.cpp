#include "gpu_shader_load_watch.h"

#include "guest_backtrace.h"
#include "guest_memory.h"
#include "guest_write_watch.h"
#include "fnv1a.h"
#include "rhi_packet_evidence.h"

#include <atomic>
#include <charconv>
#include <cstring>
#include <span>
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
    std::atomic<std::uint32_t> guestSwapSequence{0};
    std::atomic<std::uint32_t> lastSelectedFlushSwap{0};
    std::atomic<bool> selectedFlushObserved{false};
    std::atomic<bool> transitionMarkerArmed{false};
    std::atomic<bool> transitionReported{false};
};

ShaderLoadWatchState g_state;
thread_local bool g_copyObservedInCurrentCall = false;
thread_local std::uint64_t g_copySequence = 0;

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

const std::optional<std::uint32_t> &ConfiguredShaderLoadWatchAfterSwap()
{
    static const std::optional<std::uint32_t> sequence = []
    {
        const std::string &configured = lucent::config::text("WATCH_SHADER_LOAD_AFTER_SWAP");
        if (configured.empty())
            return std::optional<std::uint32_t>{};
        const std::optional<std::uint32_t> parsed = ParseShaderLoadWatchAfterSwap(configured);
        if (!parsed)
            lucent::error(
                "hle",
                "WATCH_SHADER_LOAD_AFTER_SWAP must be one unsigned decimal swap sequence, got {}",
                configured);
        return parsed;
    }();
    return sequence;
}

[[nodiscard]] bool ConfiguredShaderLoadTransitionWatch()
{
    static const bool enabled = lucent::config::flag("WATCH_SHADER_LOAD_ZERO_MARKER");
    return enabled;
}

[[nodiscard]] std::uint32_t ReadGuestBe32(std::uint8_t *memory, std::uint32_t address)
{
    std::uint32_t value = 0;
    std::memcpy(&value, memory + address, sizeof(value));
    return __builtin_bswap32(value);
}

[[nodiscard]] bool IsSelectedTransitionLoad(const RhiShaderLoadPacketEvidence &load,
                                            std::uint8_t *memory, std::uint64_t shaderHash)
{
    if (load.predicated)
        return false;
    if (load.immediate)
        return Fnv1a64(load.immediateMicrocode) == shaderHash;
    constexpr std::uint64_t kPhysicalMemoryBytes = std::uint64_t{GuestMemory::kAliasMask} + 1;
    if (load.guestAddress >= kPhysicalMemoryBytes || load.sizeBytes == 0 ||
        load.sizeBytes > kPhysicalMemoryBytes - load.guestAddress)
    {
        return false;
    }
    return Fnv1a64(std::span<const std::uint8_t>(memory + load.guestAddress, load.sizeBytes)) ==
           shaderHash;
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

std::optional<std::uint32_t> ParseShaderLoadWatchAfterSwap(std::string_view text)
{
    if (text.empty())
        return std::nullopt;

    std::uint32_t sequence = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), sequence);
    if (error != std::errc{} || end != text.data() + text.size())
        return std::nullopt;
    return sequence;
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

bool ShaderLoadPacketTransitionWatchEnabled()
{
    return ConfiguredShaderLoadTransitionWatch() && ConfiguredShaderLoadWatchHash().has_value() &&
           ConfiguredShaderLoadWatchAfterSwap().has_value();
}

void ObserveShaderLoadPacketWrite(std::uint64_t shaderHash, std::uint32_t packetGuestAddress,
                                  bool immediate, std::uint32_t completedSwapSequence)
{
    const std::optional<std::uint64_t> &configured = ConfiguredShaderLoadWatchHash();
    const std::optional<std::uint32_t> &afterSwap = ConfiguredShaderLoadWatchAfterSwap();
    if (!configured || *configured != shaderHash ||
        (afterSwap && *afterSwap != completedSwapSequence))
        return;

    if (ShaderLoadPacketTransitionWatchEnabled() &&
        g_state.transitionMarkerArmed.exchange(false, std::memory_order_acq_rel))
    {
        if (!afterSwap)
            return;
        bool expected = false;
        if (g_state.transitionReported.compare_exchange_strong(expected, true,
                                                               std::memory_order_acq_rel))
        {
            lucent::error(
                "hle",
                "shader-load zero-marker transition after guest swap {} reached its selected PM4"
                " load without a matching complete generic-copy packet or retained shader flush",
                *afterSwap);
        }
    }

    if (g_state.writeReported.load(std::memory_order_acquire))
        return;
    bool expected = false;
    if (g_state.armAttempted.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
    {
        if (!ArmGuestWriteWatch(GuestWriteWatchOwner::kShaderLoadPacket, packetGuestAddress, 1))
            return;
        g_state.packetGuestAddress.store(packetGuestAddress, std::memory_order_release);
        lucent::info(
            "hle",
            "shader-load packet watch selected {} shader {:#018x} at guest {:#x} after swap {}",
            immediate ? "inline" : "memory-backed", shaderHash, packetGuestAddress,
            completedSwapSequence);
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
        ++g_copySequence;
        lucent::info("hle",
                     "shader-load packet copy covers guest {:#x} for {} bytes from caller {:#x}",
                     target, bytes, caller);
    }
}

void NoteShaderLoadWatchGuestSwap(std::uint32_t guestSwapSequence)
{
    g_state.guestSwapSequence.store(guestSwapSequence, std::memory_order_release);
}

void ArmShaderLoadPacketTransitionFromZeroMarker()
{
    if (!ShaderLoadPacketTransitionWatchEnabled() ||
        g_state.transitionReported.load(std::memory_order_acquire))
    {
        return;
    }

    const std::optional<std::uint32_t> &afterSwap = ConfiguredShaderLoadWatchAfterSwap();
    if (!afterSwap)
        return;
    if (g_state.guestSwapSequence.load(std::memory_order_acquire) != *afterSwap)
        return;

    bool expected = false;
    if (g_state.transitionMarkerArmed.compare_exchange_strong(expected, true,
                                                              std::memory_order_acq_rel))
    {
        const std::uint32_t lastSelectedFlushSwap =
            g_state.lastSelectedFlushSwap.load(std::memory_order_acquire);
        if (!g_state.selectedFlushObserved.load(std::memory_order_acquire))
        {
            lucent::info("hle",
                         "shader-load zero-marker transition saw no selected retained shader flush"
                         " before guest swap {}",
                         *afterSwap);
        }
        else if (*afterSwap != 0 && lastSelectedFlushSwap == *afterSwap - 1)
        {
            lucent::info(
                "hle",
                "shader-load zero-marker transition follows a selected retained shader flush"
                " in guest interval {}",
                lastSelectedFlushSwap);
        }
        else
        {
            lucent::info(
                "hle",
                "shader-load zero-marker transition's latest selected retained shader flush"
                " was in guest interval {}, not the preceding interval",
                lastSelectedFlushSwap);
        }
        lucent::info("hle", "shader-load zero-marker transition armed after guest swap {}",
                     *afterSwap);
    }
}

void ObserveShaderLoadPacketTransitionFlush(std::uint64_t shaderHash)
{
    const std::optional<std::uint64_t> &configured = ConfiguredShaderLoadWatchHash();
    if (!configured || *configured != shaderHash)
    {
        return;
    }

    g_state.lastSelectedFlushSwap.store(g_state.guestSwapSequence.load(std::memory_order_acquire),
                                        std::memory_order_release);
    g_state.selectedFlushObserved.store(true, std::memory_order_release);
    if (!ShaderLoadPacketTransitionWatchEnabled() ||
        !g_state.transitionMarkerArmed.exchange(false, std::memory_order_acq_rel))
    {
        return;
    }

    bool expected = false;
    if (g_state.transitionReported.compare_exchange_strong(expected, true,
                                                           std::memory_order_acq_rel))
    {
        lucent::info("hle",
                     "shader-load zero-marker transition emitted selected shader {:#018x}"
                     " through the retained shader flush",
                     shaderHash);
    }
}

void ObserveShaderLoadPacketTransitionCopy(std::uint8_t *guestMemory, std::uint32_t destination,
                                           std::uint32_t bytes, std::uint32_t caller)
{
    if (!g_state.transitionMarkerArmed.load(std::memory_order_acquire) || destination < 4 ||
        bytes < 4 || std::uint64_t(destination) + bytes > PPC_MEMORY_SIZE)
    {
        return;
    }

    const auto evidence = InspectRhiShaderLoadRange(
        destination - 4, destination + bytes - 4,
        [guestMemory](std::uint32_t address) { return ReadGuestBe32(guestMemory, address); });
    if (!evidence.complete)
        return;

    const std::optional<std::uint64_t> &configured = ConfiguredShaderLoadWatchHash();
    if (!configured)
        return;
    const std::uint64_t shaderHash = *configured;
    for (const RhiShaderLoadPacketEvidence &load : evidence.loads)
    {
        if (!IsSelectedTransitionLoad(load, guestMemory, shaderHash))
            continue;
        bool expected = false;
        if (g_state.transitionReported.compare_exchange_strong(expected, true,
                                                               std::memory_order_acq_rel))
        {
            g_state.transitionMarkerArmed.store(false, std::memory_order_release);
            lucent::info("hle",
                         "shader-load zero-marker transition copied selected {} shader {:#018x}"
                         " at guest {:#x} from caller {:#x}",
                         load.immediate ? "inline" : "memory-backed", shaderHash,
                         load.headerAddress, caller);
        }
        return;
    }
}

std::uint64_t ShaderLoadPacketCopySequence()
{
    return g_copySequence;
}

void ReportShaderLoadPacketProducerReturn(std::uint32_t caller, std::uint32_t stackPointer)
{
    if (!std::exchange(g_copyObservedInCurrentCall, false))
        return;
    lucent::info("hle", "shader-load packet producer returned to caller {:#x}; guest stack: {}",
                 caller, FormatGuestBacktrace(stackPointer, caller));
}

void ReportShaderLoadPacketProducerParent(std::uint64_t copySequence, std::uint32_t caller,
                                          std::array<std::uint32_t, 6> arguments,
                                          std::array<std::uint32_t, 2> shaderObjectsBefore,
                                          std::array<std::uint32_t, 2> shaderObjectsAfter)
{
    if (copySequence == g_copySequence)
        return;
    lucent::info("hle",
                 "shader-load packet reached enclosing retained routine from caller {:#x}; "
                 "integer arguments {:#x} {:#x} {:#x} {:#x} {:#x} {:#x}; "
                 "device shaders pixel {:#x}->{:#x}, vertex {:#x}->{:#x}",
                 caller, arguments[0], arguments[1], arguments[2], arguments[3], arguments[4],
                 arguments[5], shaderObjectsBefore[0], shaderObjectsAfter[0],
                 shaderObjectsBefore[1], shaderObjectsAfter[1]);
}

} // namespace gears
