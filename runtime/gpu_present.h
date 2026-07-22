#pragma once

#include <cstdint>

// The host graphics backend: a real window, a Vulkan swapchain, and a present
// that happens exactly when the guest's frame loop says a frame is finished.
//
// It is deliberately NOT part of vd_null_gpu.cpp. That file is the command
// processor and the driver protocol; this one owns host graphics state and
// nothing else. The only coupling is the two calls below.
namespace gears
{

// Brings up SDL video, a Vulkan instance/device/swapchain and the present
// thread. Called once, from the command processor as it starts.
//
// Returns false when there is no usable display or no usable Vulkan device.
// That is a supported outcome, not a failure: every measurement harness in this
// project runs without a display, so the runtime carries on exactly as it did
// before and says so at warn level. GEARS_NO_WINDOW=1 forces it.
bool PresenterStart();

// Presents one frame. Called from the command processor when it executes an
// accepted (non-stale) swap packet -- i.e. once per guest VdSwap, and never on
// a host timer. `sequence` is the VdSwap sequence number the packet carries;
// `frontBuffer` is the guest address of the front buffer the title resolved
// into, kept for when real content exists (nothing reads it yet).
//
// Blocks until the frame has been submitted and presented, so the cost of
// presenting shows up in the guest's own frame rate rather than being hidden
// behind a queue. A no-op when the presenter is not running.
void PresentFrame(uint32_t frontBuffer, uint32_t sequence);

} // namespace gears
