#pragma once

#include "rhi_resource_reference.h"
#include "rhi_target_state.h"
#include "rhi_texture_state.h"

#include <array>
#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

namespace gears
{

struct FrameDrawItem;

enum class RhiSemanticDrawKind : std::uint8_t
{
    TransientVertices,
    TransientVerticesAndIndices,
    BoundVertices,
    BoundIndices,
};

[[nodiscard]] inline constexpr bool RhiDrawUsesDmaIndices(RhiSemanticDrawKind kind)
{
    return kind == RhiSemanticDrawKind::TransientVerticesAndIndices ||
           kind == RhiSemanticDrawKind::BoundIndices;
}

struct RhiSemanticBufferRange
{
    std::uint32_t guestAddress = 0;
    std::uint32_t sizeBytes = 0;
};

struct RhiSemanticBufferView
{
    RhiSemanticBufferRange allocation;
    std::uint32_t elementStrideBytes = 0;
    std::uint32_t endianSwap = 0;
};

struct RhiSemanticVertexStream
{
    std::uint32_t slot = 0;
    std::uint32_t object = 0;
    RhiSemanticBufferView view;
};

struct RhiSemanticRenderTarget
{
    // The object identity and descriptor words observed in the device shadow.
    // Descriptors can be mutated after binding by separate render-state setters;
    // the title adapter supplies the post-call state when it is available.
    bool depthStencil = false;
    std::uint32_t slot = 0;
    std::uint32_t object = 0;
    std::array<std::uint32_t, 6> descriptor{};
    std::uint32_t descriptorDwords = 0;
    bool normalizedStatePresent = false;
    RhiRenderTargetDescriptorState normalizedState;
};

struct RhiSemanticDraw
{
    RhiSemanticDrawKind kind = RhiSemanticDrawKind::BoundVertices;
    std::uint32_t primitiveType = 0;
    std::uint32_t elementCount = 0;
    std::uint32_t baseVertex = 0;
    std::uint32_t startIndex = 0;
    std::uint32_t vertexStrideBytes = 0;
    std::uint32_t indexFormatFlags = 0;
    RhiSemanticBufferRange vertexData;
    RhiSemanticBufferRange indexData;
    bool indexBufferViewPresent = false;
    RhiSemanticBufferView indexBuffer;
};

struct RhiDrawPacketEvidence
{
    bool present = false;
    std::uint32_t packetGuestAddress = 0;
    std::uint32_t opcode = 0;
    std::uint32_t primitiveType = 0;
    std::uint32_t sourceSelect = 0;
    std::uint32_t elementCount = 0;
    bool transientDataPresent = false;
    RhiSemanticBufferRange vertexData;
    RhiSemanticBufferRange indexData;
    bool indexDataPresent = false;
    std::uint32_t indexStrideBytes = 0;
    std::uint32_t indexEndianSwap = 0;
    bool vertexStreamsPresent = false;
    std::vector<RhiSemanticVertexStream> vertexStreams;
    bool renderTargetsPresent = false;
    std::vector<RhiSemanticRenderTarget> renderTargets;
};

enum class RhiDrawEvidenceResult : std::uint8_t
{
    Match,
    Missing,
    Mismatch,
};

enum class RhiSemanticBindingKind : std::uint8_t
{
    Texture,
    TextureState,
    PixelShader,
    VertexShader,
    VertexStream,
    IndexBuffer,
    ColorRenderTarget,
    DepthStencilTarget,
};

inline constexpr std::size_t kRhiSemanticBindingKindCount =
    static_cast<std::size_t>(RhiSemanticBindingKind::DepthStencilTarget) + 1;

struct RhiSemanticBinding
{
    RhiSemanticBindingKind kind = RhiSemanticBindingKind::Texture;
    std::uint32_t slot = 0;
    std::uint32_t object = 0;
    std::array<std::uint32_t, 6> descriptor{};
    std::uint32_t descriptorDwords = 0;
    bool bufferViewPresent = false;
    RhiSemanticBufferView bufferView;
};

struct RhiResourceIdentityEvidence
{
    bool present = false;
    std::uint32_t object = 0;
    std::uint32_t rawFlags = 0;
    std::uint32_t resourceType = 0;
    std::uint32_t backingObject = 0;
    std::uint32_t referenceCount = 0;
};

struct RhiBindingStateEvidence
{
    bool present = false;
    std::uint32_t observedObject = 0;
    std::array<std::uint32_t, 6> descriptor{};
    std::uint32_t descriptorDwords = 0;
    bool bufferViewPresent = false;
    RhiSemanticBufferView bufferView;
    bool targetStatePresent = false;
    RhiRenderTargetDescriptorState targetState;
    bool surfaceStatePresent = false;
    RhiSurfaceState surfaceState;
    // Shader setters may patch the register shadow after SetTexture. The
    // post-call fetch file lets the state tracker apply that ordered mutation
    // instead of preserving stale bind-time descriptors.
    bool textureFetchStatePresent = false;
    RhiTextureFetchState textureFetchState;
    RhiResourceIdentityEvidence identity;
};

enum class RhiBindingEvidenceResult : std::uint8_t
{
    Match,
    Missing,
    Mismatch,
};

struct RhiSemanticDrawState
{
    RhiSemanticDraw draw;
    std::vector<RhiSemanticVertexStream> vertexStreams;
    std::vector<RhiSemanticRenderTarget> renderTargets;
    std::vector<RhiSemanticBinding> textures;
    std::optional<RhiSemanticBinding> pixelShader;
    std::optional<RhiSemanticBinding> vertexShader;
    std::optional<RhiSemanticBinding> indexBuffer;
    bool surfaceStatePresent = false;
    RhiSurfaceState surfaceState;
};

struct RhiSemanticColorWriteState
{
    std::uint64_t requested = 0;
    bool targetPresent = false;
    RhiSemanticRenderTarget target;
    bool surfaceStatePresent = false;
    RhiSurfaceState surfaceState;
};

enum class RhiColorWriteStateEvidenceResult : std::uint8_t
{
    Match,
    Missing,
    Mismatch,
};

struct RhiSemanticPresent
{
    std::uint64_t frameSequence = 0;
    std::uint32_t frontBuffer = 0;
    std::array<std::uint32_t, 6> fetchDescriptor{};
};

struct RhiPresentPacketEvidence
{
    bool present = false;
    bool framingValid = false;
    std::uint32_t frameSequence = 0;
    std::uint32_t frontBuffer = 0;
    std::array<std::uint32_t, 6> fetchDescriptor{};
};

enum class RhiPresentEvidenceResult : std::uint8_t
{
    Match,
    Missing,
    Mismatch,
};

struct RhiSemanticResourceLifetime
{
    RhiResourceLifetimeOperation operation = RhiResourceLifetimeOperation::AddReference;
    std::uint32_t object = 0;
    std::uint32_t rawFlags = 0;
    std::uint32_t resourceType = 0;
    std::uint32_t backingObject = 0;
    std::uint32_t previousReferenceCount = 0;
};

struct RhiResourceLifetimeEvidence
{
    bool present = false;
    std::uint32_t returnedReferenceCount = 0;
};

enum class RhiResourceLifetimeEvidenceResult : std::uint8_t
{
    Match,
    Missing,
    Mismatch,
};

enum class RhiSemanticResourceConstructionKind : std::uint8_t
{
    OwnedBacking,
    WrappedBacking,
};

struct RhiSemanticResourceConstruction
{
    RhiSemanticResourceConstructionKind kind = RhiSemanticResourceConstructionKind::OwnedBacking;
    std::uint32_t requestedBytes = 0;
    std::uint32_t resourceFlags = 0;
    std::uint32_t allocationFlags = 0;
};

struct RhiResourceConstructionEvidence
{
    bool present = false;
    std::uint32_t object = 0;
    // These fields are decoded by the title adapter from the returned object.
    // The object words remain available for byte-level evidence, but native
    // owners must not infer a title's layout from that audit array.
    std::uint32_t objectFlags = 0;
    std::uint32_t initialReferenceCount = 0;
    std::uint32_t backingObject = 0;
    std::array<std::uint32_t, 5> objectWords{};
};

struct RhiSemanticVertexStreamReset
{
    std::uint32_t firstSlot = 0;
    std::uint32_t slotCount = 0;
};

struct RhiVertexStreamResetEvidence
{
    bool present = false;
    std::vector<RhiSemanticVertexStream> activeStreams;
};

enum class RhiVertexStreamResetEvidenceResult : std::uint8_t
{
    Match,
    Missing,
    Mismatch,
};

struct RhiSemanticResolve
{
    bool sourceDepthStencil = false;
    std::uint32_t sourceSlot = 0;
    std::uint32_t sourceObject = 0;
    std::uint32_t destinationObject = 0;
    std::uint32_t destinationAddress = 0;
    std::uint32_t destinationPitch = 0;
    std::uint32_t destinationHeight = 0;
    std::uint32_t destinationFormat = 0;

    // Preserve the complete title-neutral call payload alongside the fields
    // decoded for the retained-packet comparer. A native resolve consumer
    // needs the requested rectangle, destination point, storage descriptor,
    // and byte width; dropping these at the title adapter would force it to
    // rediscover guest layout from PM4 state. operationFlags remains raw until
    // each bit has an independently grounded native meaning.
    std::uint32_t operationFlags = 0;
    std::array<std::int32_t, 4> sourceRectangle{};
    std::array<std::int32_t, 2> destinationPoint{};
    std::array<std::uint32_t, 6> destinationDescriptor{};
    std::uint32_t bytesPerBlock = 0;
};

struct RhiResolvePacketEvidence
{
    bool present = false;
    std::uint32_t observedSourceObject = 0;
    std::uint32_t destinationAddress = 0;
    std::uint32_t destinationPitch = 0;
    std::uint32_t destinationHeight = 0;
    std::uint32_t drawOpcode = 0;
    std::uint32_t primitiveType = 0;
    std::uint32_t sourceSelect = 0;
    std::uint32_t elementCount = 0;
};

enum class RhiResolveEvidenceResult : std::uint8_t
{
    Match,
    Missing,
    Mismatch,
};

struct RhiObservedDraw
{
    RhiSemanticDrawState state;
    RhiDrawPacketEvidence packet;
    RhiDrawEvidenceResult evidence = RhiDrawEvidenceResult::Missing;
};

struct RhiObservedBinding
{
    RhiSemanticBinding binding;
    RhiBindingStateEvidence state;
    RhiBindingEvidenceResult evidence = RhiBindingEvidenceResult::Missing;
};

struct RhiObservedPresent
{
    RhiSemanticPresent present;
    RhiPresentPacketEvidence packet;
    RhiPresentEvidenceResult evidence = RhiPresentEvidenceResult::Missing;
};

struct RhiObservedResolve
{
    RhiSemanticResolve resolve;
    RhiResolvePacketEvidence packet;
    RhiResolveEvidenceResult evidence = RhiResolveEvidenceResult::Missing;
};

struct RhiObservedResourceLifetime
{
    RhiSemanticResourceLifetime lifetime;
    RhiResourceLifetimeEvidence retained;
    RhiResourceLifetimeEvidenceResult evidence = RhiResourceLifetimeEvidenceResult::Missing;
};

struct RhiObservedResourceConstruction
{
    RhiSemanticResourceConstruction construction;
    RhiResourceConstructionEvidence retained;
};

struct RhiObservedVertexStreamReset
{
    RhiSemanticVertexStreamReset reset;
    RhiVertexStreamResetEvidence state;
    RhiVertexStreamResetEvidenceResult evidence = RhiVertexStreamResetEvidenceResult::Missing;
};

struct RhiObservedColorWriteState
{
    RhiSemanticColorWriteState state;
    RhiColorWriteStateEvidenceResult evidence = RhiColorWriteStateEvidenceResult::Missing;
};

using RhiSemanticEventPayload =
    std::variant<RhiObservedDraw, RhiObservedBinding, RhiObservedResourceLifetime,
                 RhiObservedResourceConstruction, RhiObservedVertexStreamReset,
                 RhiObservedColorWriteState, RhiObservedResolve, RhiObservedPresent>;

struct RhiSemanticEvent
{
    std::uint64_t sequence = 0;
    RhiSemanticEventPayload payload;
};

struct RhiSemanticFrame
{
    std::uint64_t frameSequence = 0;
    std::vector<RhiSemanticEvent> events;
    std::uint64_t draws = 0;
    std::uint64_t bindings = 0;
    std::uint64_t resourceLifetimeCalls = 0;
    std::uint64_t resourceRetirements = 0;
    std::uint64_t resourceConstructions = 0;
    std::uint64_t resolves = 0;
    std::uint64_t presents = 0;
    std::uint64_t matched = 0;
    std::uint64_t missing = 0;
    std::uint64_t mismatched = 0;
    std::uint64_t bindingsMatched = 0;
    std::uint64_t bindingsMissing = 0;
    std::uint64_t bindingsMismatched = 0;
    std::uint64_t resourceLifetimeMatched = 0;
    std::uint64_t resourceLifetimeMissing = 0;
    std::uint64_t resourceLifetimeMismatched = 0;
    std::uint64_t vertexStreamResets = 0;
    std::uint64_t vertexStreamResetsMatched = 0;
    std::uint64_t vertexStreamResetsMissing = 0;
    std::uint64_t vertexStreamResetsMismatched = 0;
    std::uint64_t colorWriteStates = 0;
    std::uint64_t colorWriteStatesMatched = 0;
    std::uint64_t colorWriteStatesMissing = 0;
    std::uint64_t colorWriteStatesMismatched = 0;
    std::uint64_t resolvesMatched = 0;
    std::uint64_t resolvesMissing = 0;
    std::uint64_t resolvesMismatched = 0;
    std::uint64_t presentsMatched = 0;
    std::uint64_t presentsMissing = 0;
    std::uint64_t presentsMismatched = 0;
};

[[nodiscard]] bool RhiSemanticObservationEnabled();
[[nodiscard]] RhiDrawEvidenceResult CompareRhiDrawPacket(const RhiSemanticDraw &draw,
                                                         const RhiDrawPacketEvidence &packet);
[[nodiscard]] RhiDrawEvidenceResult CompareRhiDrawVertexState(const RhiSemanticDrawState &state,
                                                              const RhiDrawPacketEvidence &packet);
[[nodiscard]] RhiDrawEvidenceResult
CompareRhiDrawRenderTargetState(const RhiSemanticDrawState &state,
                                const RhiDrawPacketEvidence &packet);
[[nodiscard]] RhiBindingEvidenceResult CompareRhiBindingState(const RhiSemanticBinding &binding,
                                                              const RhiBindingStateEvidence &state);
[[nodiscard]] RhiPresentEvidenceResult
CompareRhiPresentPacket(const RhiSemanticPresent &present, const RhiPresentPacketEvidence &packet);
[[nodiscard]] RhiResourceLifetimeEvidenceResult
CompareRhiResourceLifetime(const RhiSemanticResourceLifetime &lifetime,
                           const RhiResourceLifetimeEvidence &retained);
[[nodiscard]] RhiVertexStreamResetEvidenceResult
CompareRhiVertexStreamReset(const RhiSemanticVertexStreamReset &reset,
                            const RhiVertexStreamResetEvidence &state);
[[nodiscard]] RhiResolveEvidenceResult
CompareRhiResolvePacket(const RhiSemanticResolve &resolve, const RhiResolvePacketEvidence &packet);
void ObserveRhiSemanticDraw(const RhiSemanticDraw &draw, const RhiDrawPacketEvidence &packet);
void ObserveRhiSemanticBinding(const RhiSemanticBinding &binding,
                               const RhiBindingStateEvidence &state);
void ObserveRhiSemanticPresent(const RhiSemanticPresent &present,
                               const RhiPresentPacketEvidence &packet);
void ObserveRhiSemanticResourceLifetime(const RhiSemanticResourceLifetime &lifetime,
                                        const RhiResourceLifetimeEvidence &retained);
void ObserveRhiSemanticResourceConstruction(const RhiSemanticResourceConstruction &construction,
                                            const RhiResourceConstructionEvidence &retained);
void ObserveRhiSemanticVertexStreamReset(const RhiSemanticVertexStreamReset &reset,
                                         const RhiVertexStreamResetEvidence &state);
void ObserveRhiSemanticColorWriteState(const RhiSemanticColorWriteState &state);
void ObserveRhiSemanticResolve(const RhiSemanticResolve &resolve,
                               const RhiResolvePacketEvidence &packet);
[[nodiscard]] RhiSemanticFrame SealRhiSemanticFrame(std::uint64_t frameSequence);
[[nodiscard]] RhiSemanticFrame ReportRhiSemanticFrame(std::uint64_t frameSequence);

} // namespace gears
