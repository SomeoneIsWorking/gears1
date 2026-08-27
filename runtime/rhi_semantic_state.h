#pragma once

#include "rhi_semantic_stream.h"

#include <cstdint>
#include <map>

namespace gears
{

class RhiSemanticStateTracker
{
  public:
    void ApplyBinding(const RhiSemanticBinding &binding);
    [[nodiscard]] RhiSemanticDrawState SnapshotDraw(const RhiSemanticDraw &draw) const;
    void Reset();

  private:
    std::map<std::uint32_t, RhiSemanticVertexStream> vertexStreams_;
};

} // namespace gears
