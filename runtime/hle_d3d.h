#pragma once

namespace gears
{
// Prints the per-function call census gathered by the D3D probes in
// hle_d3d.cpp. Channel-gated on "hle"; costs nothing when the channel is off.
void HleDumpCensus(const char* why);

// Reports how the D3D worker's replay queue behaved: whether each replay took
// a new command list or repeated the previous one.
void HleWorkerCensus();
}
