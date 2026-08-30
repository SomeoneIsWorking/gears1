#pragma once

// The title-neutral draw input consumed by a native renderer.
//
// BuildNativeDrawInput is the only function here that accepts the retained
// guest register snapshot. It is a compatibility-front-end producer: it
// decodes that snapshot once into ordinary draw, target, raster, and viewport
// state. A native execution owner receives NativeDrawInput, never PM4 packets,
// Xenos register storage, or translated shader objects. The retained Vulkan
// renderer currently consumes the same value as its oracle.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include "gpu_draw_depth_bias.h"
#include "gpu_draw_formats.h"
#include "gpu_draw_sample_layout.h"
#include "gpu_draw_xlate.h"

namespace gears::draw
{

struct NativeDrawInputOptions
{
    bool msaaModel = true;
    bool hasDepthClamp = false;
    bool applyDepthBias = true;
    bool fixedViewport = false;
    uint32_t sampleGridWidth = 0;
    uint32_t sampleGridHeight = 0;
    uint32_t targetWidth = 0;
    uint32_t targetHeight = 0;
    uint32_t maxViewportWidth = 0;
    uint32_t maxViewportHeight = 0;
};

// Host-independent viewport/scissor values. Vulkan conversion remains in the
// retained renderer; the native backend can map this to its own API without
// inheriting Vulkan or Xenos ownership.
struct NativeViewport
{
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    float minDepth = 0.0f;
    float maxDepth = 1.0f;
    uint32_t scissorX = 0;
    uint32_t scissorY = 0;
    uint32_t scissorWidth = 0;
    uint32_t scissorHeight = 0;
};

struct NativeDrawInput
{
    uint32_t primitiveType = 0;
    uint32_t indexCount = 0;
    bool indexed = false;
    bool indexIs32 = false;
    uint32_t indexEndian = 0;
    uint32_t indexGuestBase = 0;
    uint64_t vertexShaderHash = 0;
    uint64_t pixelShaderHash = 0;

    uint32_t surfaceBase = 0;
    uint32_t colorFormat = 0;
    int32_t colorExpBias = 0;
    uint32_t surfaceInfo = 0;
    uint32_t depthBase = 0;
    bool depthIsFloat24 = false;
    DrawSampleLayout sampleLayout;

    OutputMergerState outputMerger;
    DepthBias depthBias;
    GuestViewport guestViewport;
    NativeViewport viewport;

    // These values are retained only for the existing diagnostic stream. They
    // are decoded here so the renderer does not reread the register file after
    // the native input boundary.
    uint32_t clipControl = 0;
    uint32_t vteControl = 0;
    uint32_t windowOffset = 0;
    float viewportXScale = 0.0f;
    float viewportXOffset = 0.0f;
    float viewportYScale = 0.0f;
    float viewportYOffset = 0.0f;
    float viewportZScale = 0.0f;
    float viewportZOffset = 0.0f;
    bool viewportClamped = false;
};

enum class NativeDrawMaterializationOutcome : uint8_t
{
    Refused,
    Resolve,
    Materialized,
};

struct NativeDrawMaterialization
{
    size_t sourceOrdinal = 0;
    uint32_t packetGuestAddress = 0;
    NativeDrawMaterializationOutcome outcome = NativeDrawMaterializationOutcome::Refused;
    NativeDrawInput input;
};

enum class NativeFrameMaterializationStatus : uint8_t
{
    Complete,
    RendererUnavailable,
    Dropped,
};

struct NativeFrameMaterialization
{
    NativeFrameMaterializationStatus status = NativeFrameMaterializationStatus::Complete;
    std::vector<NativeDrawMaterialization> draws;
};

using NativeFrameMaterializationCallback =
    std::function<void(uint64_t, NativeFrameMaterialization)>;

// One terminal publication per renderer attempt. Every source draw gets one
// stable ordinal and defaults to Refused, so a skipped draw cannot compact the
// evidence vector and shift every later semantic comparison.
class NativeFrameMaterializationRecorder
{
  public:
    NativeFrameMaterializationRecorder(int64_t frameSequence, size_t drawCount,
                                       NativeFrameMaterializationCallback callback)
        : frameSequence_(frameSequence >= 0 ? static_cast<uint64_t>(frameSequence) : 0),
          callback_(std::move(callback))
    {
        if (!callback_)
            return;
        result_.draws.resize(drawCount);
        for (size_t ordinal = 0; ordinal < drawCount; ++ordinal)
            result_.draws[ordinal].sourceOrdinal = ordinal;
    }

    ~NativeFrameMaterializationRecorder() { Publish(); }

    NativeFrameMaterializationRecorder(const NativeFrameMaterializationRecorder &) = delete;
    NativeFrameMaterializationRecorder &
    operator=(const NativeFrameMaterializationRecorder &) = delete;

    [[nodiscard]] size_t BeginDraw(uint32_t packetGuestAddress)
    {
        const size_t sourceOrdinal = nextOrdinal_++;
        SetPacketIdentity(sourceOrdinal, packetGuestAddress);
        return sourceOrdinal;
    }

    void MarkResolve(size_t sourceOrdinal)
    {
        if (sourceOrdinal < result_.draws.size())
            result_.draws[sourceOrdinal].outcome = NativeDrawMaterializationOutcome::Resolve;
    }

    void SetPacketIdentity(size_t sourceOrdinal, uint32_t packetGuestAddress)
    {
        if (sourceOrdinal < result_.draws.size())
            result_.draws[sourceOrdinal].packetGuestAddress = packetGuestAddress;
    }

    void MarkMaterialized(size_t sourceOrdinal, const NativeDrawInput &input)
    {
        if (sourceOrdinal >= result_.draws.size())
            return;
        result_.draws[sourceOrdinal].outcome = NativeDrawMaterializationOutcome::Materialized;
        result_.draws[sourceOrdinal].input = input;
    }

    void Publish()
    {
        if (!callback_)
            return;
        NativeFrameMaterializationCallback callback = std::move(callback_);
        callback(frameSequence_, std::move(result_));
    }

  private:
    uint64_t frameSequence_ = 0;
    size_t nextOrdinal_ = 0;
    NativeFrameMaterializationCallback callback_;
    NativeFrameMaterialization result_;
};

// Returns false when the retained snapshot is absent or viewport derivation
// cannot produce a complete input. No partial input is handed to either
// renderer owner.
bool BuildNativeDrawInput(const uint32_t *registerFile, uint32_t primitiveType, uint32_t indexCount,
                          bool indexed, bool indexIs32, uint32_t indexEndian,
                          uint32_t indexGuestBase, uint64_t vertexShaderHash,
                          uint64_t pixelShaderHash, const NativeDrawInputOptions &options,
                          NativeDrawInput &out);

} // namespace gears::draw
