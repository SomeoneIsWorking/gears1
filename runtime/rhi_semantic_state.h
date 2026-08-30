#pragma once

#include "rhi_semantic_stream.h"

#include <cstdint>
#include <map>
#include <optional>

namespace gears
{

class RhiSemanticStateTracker
{
  public:
    void ApplyBinding(const RhiSemanticBinding &binding);
    void ApplyBinding(const RhiSemanticBinding &binding, const RhiBindingStateEvidence &state);
    void ApplyVertexStreamReset(const RhiSemanticVertexStreamReset &reset);
    [[nodiscard]] RhiColorWriteStateEvidenceResult
    ApplyColorWriteState(const RhiSemanticColorWriteState &state);
    [[nodiscard]] RhiSemanticDrawState SnapshotDraw(const RhiSemanticDraw &draw) const;
    void Reset();

  private:
    std::map<std::uint32_t, RhiSemanticVertexStream> vertexStreams_;
    std::map<std::uint32_t, RhiSemanticBinding> textures_;
    std::optional<RhiSemanticBinding> pixelShader_;
    std::optional<RhiSemanticBinding> vertexShader_;
    std::optional<RhiSemanticBinding> indexBuffer_;
    std::map<std::uint32_t, RhiSemanticRenderTarget> colorTargets_;
    std::optional<RhiSemanticRenderTarget> depthStencilTarget_;
    std::optional<RhiSurfaceState> surfaceState_;
};

} // namespace gears
