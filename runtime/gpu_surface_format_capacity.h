#pragma once

// Persistent render targets must retain every guest format they have needed.
// A Vulkan image's format is fixed at creation, while one Xenos EDRAM base is
// routinely reinterpreted under several formats later in the run.  Keeping the
// accumulated set makes format growth explicit and prevents the first frame to
// touch a base from permanently deciding its capacity.

#include <cstddef>
#include <cstdint>
#include <set>

namespace gears::draw
{

inline bool AccumulateSurfaceFormats(std::set<uint32_t> &capacity,
                                     const std::set<uint32_t> &required)
{
    const size_t before = capacity.size();
    capacity.insert(required.begin(), required.end());
    return capacity.size() != before;
}

} // namespace gears::draw
