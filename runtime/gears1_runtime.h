#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <x360port/runtime.hpp>
#include <x360port/xam_input.hpp>

#include "gears1_guest_image.h"
#include "titles/gears1/xam_video_services.h"

namespace gears
{

// Composes the authenticated Gears 1 image adapter with the shared Xenia
// runtime. Unknown function imports intentionally remain typed refusals until
// a title-owned service has a grounded contract.
class Gears1Runtime final
{
  public:
    Gears1Runtime(std::uint32_t av_pack, x360port::XamPadReader state_reader,
                  x360port::XamCapabilitiesReader capabilities_reader,
                  void *input_context) noexcept;
    Gears1Runtime(const Gears1Runtime &) = delete;
    Gears1Runtime &operator=(const Gears1Runtime &) = delete;
    Gears1Runtime(Gears1Runtime &&) = delete;
    Gears1Runtime &operator=(Gears1Runtime &&) = delete;
    ~Gears1Runtime() = default;

    [[nodiscard]] x360port::RuntimeFailure Initialize(std::span<const std::byte> normalized_image,
                                                      const XexIdentity &expected,
                                                      std::span<const ImportSpec> imports);
    [[nodiscard]] x360port::RuntimeFailure InitializeCheckedXex(std::span<const std::byte> xex,
                                                                const XexIdentity &expected);
    [[nodiscard]] x360port::ExecutionResult
    ExecuteEntry(std::span<const std::uint64_t> arguments = {},
                 x360port::ExecutionLimits limits = {});

    [[nodiscard]] const x360port::JitStatistics *Statistics() const noexcept;

  private:
    static void RefuseUnsupportedImport(x360port::GuestImportContext &call, void *context) noexcept;
    static x360port::GuestAddress ResolveVariable(void *context) noexcept;

    void Reset();
    [[nodiscard]] x360port::RuntimeFailure InitializeContext();
    [[nodiscard]] x360port::RuntimeFailure ComposeBindings();

    Gears1GuestImage module_;
    std::unique_ptr<x360port::RuntimeContext> context_;
    titles::gears1::XamVideoServices video_services_;
    x360port::XamInputService input_service_;
    x360port::GuestMemoryAllocation variable_storage_{};
    std::size_t variable_count_ = 0;
    std::size_t next_variable_ = 0;
    std::vector<x360port::ImportBinding> bindings_;
};

} // namespace gears
