#pragma once

namespace gears
{

struct FrameDrawInputs;

// One authoritative render/publish adapter shared by live and measurement
// frames. The probe state itself remains independently testable.
bool RenderFrameWithGraphicsProbe(const FrameDrawInputs &frame);

} // namespace gears
