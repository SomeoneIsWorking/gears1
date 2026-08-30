#pragma once

namespace gears
{

struct HleD3dDiagnosticsCallbacks
{
    void (*dumpCensus)(const char *why) = nullptr;
    void (*workerCensus)() = nullptr;
};

class HleD3dDiagnosticsRouter final
{
  public:
    [[nodiscard]] bool Install(HleD3dDiagnosticsCallbacks callbacks) noexcept;
    [[nodiscard]] bool IsInstalled() const noexcept;

    void DumpCensus(const char *why) const;
    void WorkerCensus() const;

  private:
    HleD3dDiagnosticsCallbacks callbacks_{};
};

// Product composition installs one complete exact-title diagnostics owner
// before guest threads start. Missing callbacks are an intentional no-op;
// partial and duplicate installations refuse.
[[nodiscard]] bool InstallHleD3dDiagnostics(HleD3dDiagnosticsCallbacks callbacks) noexcept;

// Title-neutral frame-boundary dispatch used by the shared command processor.
void HleDumpCensus(const char *why);
void HleWorkerCensus();

} // namespace gears
