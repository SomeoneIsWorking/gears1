#include "shader_flush_capture.h"

#include "fnv1a.h"
#include "guest_memory.h"
#include "guest_state_memory.h"
#include "gpu_shader_load_watch.h"
#include "import_stub.h"
#include "rhi_packet_evidence.h"
#include "rhi_semantic_stream.h"
#include "shader_binding_capture.h"
#include "shader_setter_state.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include <lucent/log.h>

namespace gears::titles::gears1
{
namespace
{

thread_local ShaderFlushRangeCapture g_activeCapture;

[[nodiscard]] std::uint32_t ReadGuestBe32(std::uint32_t address)
{
    return GuestStateMemory{gears::Memory().Base()}.Read32(address);
}

[[nodiscard]] bool IsPhysicalRange(std::uint32_t address, std::uint32_t sizeBytes)
{
    constexpr std::uint32_t kPhysicalMemoryBytes = 0x20000000;
    return sizeBytes != 0 && address < kPhysicalMemoryBytes &&
           sizeBytes <= kPhysicalMemoryBytes - address;
}

void PublishShaderModules(ShaderStage stage, std::uint32_t device,
                          std::span<const RhiShaderModuleEvidence> modules)
{
    const auto kind = stage == ShaderStage::Vertex ? RhiSemanticBindingKind::VertexShader
                                                   : RhiSemanticBindingKind::PixelShader;
    const RhiBindingStateEvidence state = CaptureShaderBinding(stage, device, modules, true);
    ObserveRhiSemanticBinding(
        {.kind = kind, .origin = RhiSemanticBindingOrigin::Flush, .object = state.observedObject},
        state);
}

} // namespace

void ObserveShaderFlushCommandBufferTransition(std::uint32_t device, std::uint32_t oldCommandEnd,
                                               std::uint32_t newCommandBefore)
{
    g_activeCapture.ObserveCommandBufferTransition(device, oldCommandEnd, newCommandBefore);
}

void ObserveShaderCommandBufferSubmission(std::uint32_t device, std::uint32_t guestAddress,
                                          std::uint32_t dwordCount)
{
    if (!gears::RhiSemanticObservationEnabled())
        return;
    const std::uint64_t byteCount = std::uint64_t{dwordCount} * sizeof(std::uint32_t);
    if (guestAddress < sizeof(std::uint32_t) || dwordCount == 0 ||
        byteCount > std::numeric_limits<std::uint32_t>::max() ||
        !IsPhysicalRange(guestAddress, static_cast<std::uint32_t>(byteCount)))
    {
        lucent::error("rhi", "shader command-buffer submission refused address {:#x}, {} dword(s)",
                      guestAddress, dwordCount);
        return;
    }

    const std::uint32_t commandEnd =
        guestAddress + static_cast<std::uint32_t>(byteCount) - sizeof(std::uint32_t);
    const auto evidence = gears::InspectRhiShaderLoadRange(guestAddress - sizeof(std::uint32_t),
                                                           commandEnd, [](std::uint32_t address)
                                                           { return ReadGuestBe32(address); });
    std::array<std::vector<RhiShaderModuleEvidence>, 2> modules;
    bool complete = evidence.complete;
    std::array<std::uint32_t, 2> loadCounts{};
    for (const RhiShaderLoadPacketEvidence &load : evidence.loads)
    {
        ++loadCounts[load.stage];
        if (load.predicated ||
            (!load.immediate && !IsPhysicalRange(load.guestAddress, load.sizeBytes)) ||
            (load.immediate && load.immediateMicrocode.size() != load.sizeBytes))
        {
            complete = false;
            continue;
        }
        const auto bytes = load.immediate
                               ? std::span<const std::uint8_t>(load.immediateMicrocode)
                               : std::span<const std::uint8_t>(
                                     gears::Memory().Base() + load.guestAddress, load.sizeBytes);
        modules[load.stage].clear();
        modules[load.stage].push_back({
            .guestAddress = load.guestAddress,
            .sizeBytes = load.sizeBytes,
            .hash = gears::Fnv1a64(bytes),
        });
    }

    if (!complete)
    {
        PublishShaderModules(ShaderStage::Vertex, device, {});
        PublishShaderModules(ShaderStage::Pixel, device, {});
    }
    else
    {
        if (!modules[0].empty())
            PublishShaderModules(ShaderStage::Vertex, device, modules[0]);
        if (!modules[1].empty())
            PublishShaderModules(ShaderStage::Pixel, device, modules[1]);
    }
    lucent::debug("rhi",
                  "shader command-buffer submission at {:#x}, {} dword(s), {} packet(s), {}"
                  " vertex and {} pixel load(s), complete={}",
                  guestAddress, dwordCount, evidence.packetCount, loadCounts[0], loadCounts[1],
                  complete);
}

bool ShaderFlushCaptureActive()
{
    return g_activeCapture.Active();
}

} // namespace gears::titles::gears1

extern "C" PPC_FUNC(__imp__sub_822346A8);
PPC_FUNC(sub_822346A8)
{
    using namespace gears::titles::gears1;

    const std::uint32_t device = ctx.r3.u32;
    if (!gears::RhiSemanticObservationEnabled())
    {
        __imp__sub_822346A8(ctx, base);
        return;
    }

    const std::uint32_t commandBefore = ReadGuestBe32(device + 0x28);
    if (!g_activeCapture.Begin(device, commandBefore))
    {
        lucent::error(
            "rhi", "shader flush capture refused nested or invalid begin for device {:#x}", device);
        __imp__sub_822346A8(ctx, base);
        return;
    }

    __imp__sub_822346A8(ctx, base);
    const ShaderFlushCaptureResult capture =
        g_activeCapture.Finish(device, ReadGuestBe32(device + 0x28));

    std::array<std::vector<gears::RhiShaderModuleEvidence>, 2> modules;
    std::array<std::uint32_t, 2> loadCounts{};
    bool complete = capture.complete;
    std::uint32_t packetCount = 0;
    std::uint32_t dwordCount = 0;
    for (const ShaderCommandRange &range : capture.ranges)
    {
        const auto evidence = gears::InspectRhiShaderLoadRange(
            range.commandBefore, range.commandEnd,
            [](std::uint32_t address) { return ReadGuestBe32(address); });
        complete = complete && evidence.complete;
        packetCount += evidence.packetCount;
        dwordCount += evidence.dwordCount;
        for (const gears::RhiShaderLoadPacketEvidence &load : evidence.loads)
        {
            ++loadCounts[load.stage];
            if (load.predicated ||
                (!load.immediate && !IsPhysicalRange(load.guestAddress, load.sizeBytes)) ||
                (load.immediate && load.immediateMicrocode.size() != load.sizeBytes))
            {
                complete = false;
                continue;
            }
            const auto bytes =
                load.immediate ? std::span<const std::uint8_t>(load.immediateMicrocode)
                               : std::span<const std::uint8_t>(
                                     gears::Memory().Base() + load.guestAddress, load.sizeBytes);
            const std::uint64_t hash = gears::Fnv1a64(bytes);
            gears::ObserveShaderLoadPacketTransitionFlush(hash);
            // Shader loads execute in packet order and no semantic draw can
            // occur inside this retained call. The final unpredicated load is
            // therefore the concrete module active after the flush.
            modules[load.stage].clear();
            modules[load.stage].push_back({
                .guestAddress = load.guestAddress,
                .sizeBytes = load.sizeBytes,
                .hash = hash,
            });
        }
    }

    if (!complete)
    {
        modules[0].clear();
        modules[1].clear();
        PublishShaderModules(ShaderStage::Vertex, device, {});
        PublishShaderModules(ShaderStage::Pixel, device, {});
    }
    else
    {
        if (!modules[0].empty())
            PublishShaderModules(ShaderStage::Vertex, device, modules[0]);
        if (!modules[1].empty())
            PublishShaderModules(ShaderStage::Pixel, device, modules[1]);
    }

    lucent::debug("rhi",
                  "shader flush captured {} range(s), {} packet(s), {} dword(s), {} vertex and "
                  "{} pixel load(s), complete={}",
                  capture.ranges.size(), packetCount, dwordCount, loadCounts[0], loadCounts[1],
                  complete);
}
