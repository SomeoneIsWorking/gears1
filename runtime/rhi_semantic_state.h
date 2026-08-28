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
    void ApplyVertexStreamReset(const RhiSemanticVertexStreamReset &reset);
    [[nodiscard]] RhiSemanticDrawState SnapshotDraw(const RhiSemanticDraw &draw) const;
    void Reset();

  private:
    std::map<std::uint32_t, RhiSemanticVertexStream> vertexStreams_;
    std::map<std::uint32_t, RhiSemanticRenderTarget> colorTargets_;
    std::optional<RhiSemanticRenderTarget> depthStencilTarget_;
};

} // namespace gears
