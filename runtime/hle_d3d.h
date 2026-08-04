#pragma once

namespace gears
{
// Prints the per-function call census gathered by the D3D probes in
// hle_d3d.cpp. Channel-gated on "hle"; costs nothing when the channel is off.
void HleDumpCensus(const char* why);

// Reports how the D3D worker's replay queue behaved: whether each replay took
// a new command list or repeated the previous one.
void HleWorkerCensus();

// The title's OWN account of which texture object is bound to which sampler
// slot, taken at the D3D SetTexture seam rather than inferred from the PM4
// register file. Printed to be diffed against the renderer's "frame texture
// bases" line: the two describe the same thing from opposite ends, so anywhere
// they disagree is either a register write we do not mirror or a texture the
// title had already replaced.
void ReportTitleTextureSlots();

// Per-frame tick for the bound-shader capture: revalidates the shader pointers
// currently sitting in the device's shader state, dumps any container not seen
// before, and (on a cadence) reports the census. Does nothing unless
// GEARS_SHADER_CAPTURE=1.
void HleShaderCaptureFrame(uint64_t frame);
}
