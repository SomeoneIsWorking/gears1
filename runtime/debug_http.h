#pragma once

#include <cstdint>

namespace gears
{

// The debug server is deliberately loopback-only and starts once. Port 32123
// is the default; GEARS_DEBUG_HTTP_PORT=0 disables it, and another value moves
// it when several runtime instances are being investigated at once.
void StartDebugHttpServer();
void StopDebugHttpServer();

} // namespace gears
