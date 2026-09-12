#pragma once

#include <cstdint>

#include <x360port/runtime.hpp>

namespace gears::titles::gears1
{

class XamVideoServices final
{
  public:
    explicit XamVideoServices(std::uint32_t av_pack) noexcept : av_pack_(av_pack) {}

    XamVideoServices(const XamVideoServices &) = delete;
    XamVideoServices &operator=(const XamVideoServices &) = delete;
    XamVideoServices(XamVideoServices &&) = delete;
    XamVideoServices &operator=(XamVideoServices &&) = delete;

    // Binds only the exact XGetAVPack export used by this title revision.
    // Unknown imports remain the caller's responsibility and must not acquire
    // a guessed service implementation.
    void Bind(const x360port::ImportRequirement &requirement,
              x360port::ImportBinding &binding) noexcept;

    [[nodiscard]] std::uint32_t call_count() const noexcept { return call_count_; }

  private:
    static void GetAVPack(x360port::GuestImportContext &context, void *service) noexcept;

    std::uint32_t av_pack_;
    std::uint32_t call_count_ = 0;
};

} // namespace gears::titles::gears1
