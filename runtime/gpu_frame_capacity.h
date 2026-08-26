#pragma once

#include <cstddef>

namespace gears
{

// Bounded ownership across the asynchronous frame pipeline. Scan-out needs one
// allocation per renderer slot, one per presenter slot, and one independently
// retained latest publication so either side may stall without forcing the
// other to overwrite an image that is still in use.
inline constexpr size_t kRendererFramesInFlight = 2;
inline constexpr size_t kPresenterFramesInFlight = 2;
inline constexpr size_t kPublishedFramesRetained = 1;
inline constexpr size_t kSharedScanoutImageCount =
    kRendererFramesInFlight + kPresenterFramesInFlight + kPublishedFramesRetained;

} // namespace gears
