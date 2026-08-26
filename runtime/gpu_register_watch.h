#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gears
{

// Diagnostic ownership for GEARS_GPU_REG_WATCH. Packet execution supplies the
// current source and draw ordinal; this module owns configuration, hit counts,
// value formatting, and periodic publication.
const std::vector<uint32_t> &GpuRegisterWatchRegisters();
bool GpuRegisterWatchEnabled();
void ObserveGpuRegisterWrite(uint32_t reg, uint32_t value, uint32_t oldBits, uint32_t drawOrdinal);
void ReportGpuRegisterWatch();

class GpuRegisterWriteScope
{
  public:
    explicit GpuRegisterWriteScope(std::string source);
    ~GpuRegisterWriteScope();

    GpuRegisterWriteScope(const GpuRegisterWriteScope &) = delete;
    GpuRegisterWriteScope &operator=(const GpuRegisterWriteScope &) = delete;

  private:
    std::string previous_;
    bool active_ = false;
};

} // namespace gears
