#include "x360port/runtime.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string_view>

namespace
{

using x360port::GuestAddress;

constexpr GuestAddress kGearsResourceLeafAddress = 0x8222E868U;
constexpr GuestAddress kSyntheticImageBase = 0x82220000U;
constexpr GuestAddress kSyntheticDbgPrintAddress = kGearsResourceLeafAddress + 0x40U;
constexpr GuestAddress kSyntheticDbgPrintRecord = kGearsResourceLeafAddress + 0x50U;
constexpr std::uint32_t kDbgPrintOrdinal = 3U;
constexpr std::size_t kSyntheticImageSize = 0x10000U;

template <std::size_t ImageSize, std::size_t Size>
void CopyBytes(std::array<std::byte, ImageSize> &image, std::size_t offset,
               const std::array<std::uint8_t, Size> &bytes)
{
    for (std::size_t index = 0; index < bytes.size(); ++index)
    {
        image[offset + index] = static_cast<std::byte>(bytes[index]);
    }
}

class GearsBoundaryModule final : public x360port::GuestModule
{
  public:
    GearsBoundaryModule()
    {
        constexpr std::array<std::uint8_t, 24> CallDbgPrint{
            0x7D, 0x88, 0x02, 0xA6, // mflr r12
            0x38, 0x60, 0x00, 0x07, // li r3, 7
            0x48, 0x00, 0x00, 0x39, // bl +0x38
            0x38, 0x63, 0x00, 0x01, // addi r3, r3, 1
            0x7D, 0x88, 0x03, 0xA6, // mtlr r12
            0x4E, 0x80, 0x00, 0x20, // blr
        };
        CopyBytes(image_, kGearsResourceLeafAddress - kSyntheticImageBase, CallDbgPrint);
        imports_[0] = {
            x360port::ImportKind::Function, "xboxkrnl.exe",          kDbgPrintOrdinal, "DbgPrint",
            kSyntheticDbgPrintAddress,      kSyntheticDbgPrintRecord};
        descriptor_.image = {x360port::HashBytes(image_), kSyntheticImageBase,
                             static_cast<std::uint32_t>(image_.size()), kGearsResourceLeafAddress};
        descriptor_.code = {kGearsResourceLeafAddress, 0x50U};
        descriptor_.import_count = imports_.size();
        descriptor_.import_manifest_sha256 = x360port::HashImportManifest(imports_);
    }

    [[nodiscard]] const x360port::ModuleDescriptor &Descriptor() const noexcept override
    {
        return descriptor_;
    }

    [[nodiscard]] std::span<const std::byte> ImageBytes() const noexcept override { return image_; }

    [[nodiscard]] std::span<const x360port::ImportRequirement>
    ImportManifest() const noexcept override
    {
        return imports_;
    }

  private:
    std::array<std::byte, kSyntheticImageSize> image_{};
    std::array<x360port::ImportRequirement, 1> imports_{};
    x360port::ModuleDescriptor descriptor_{};
};

struct NativeObservations
{
    std::uint32_t dbg_print_calls = 0;
};

void DbgPrint(void *, void *, void *context) noexcept
{
    ++static_cast<NativeObservations *>(context)->dbg_print_calls;
}

[[noreturn]] void Fail(std::string_view message)
{
    std::cerr << "Gears x360port discriminator failed: " << message << '\n';
    std::exit(1);
}

void Require(bool condition, std::string_view message)
{
    if (!condition)
    {
        Fail(message);
    }
}

} // namespace

int main()
{
    x360port::RuntimeCreateResult created = x360port::RuntimeContext::Create();
    Require(static_cast<bool>(created), created.failure.detail);

    GearsBoundaryModule module;
    NativeObservations observations;
    const std::array bindings{
        x360port::ImportBinding{.library = "xboxkrnl.exe",
                                .ordinal = kDbgPrintOrdinal,
                                .kind = x360port::ImportKind::Function,
                                .function_handler = DbgPrint,
                                .function_context = &observations},
    };
    x360port::RuntimeFailure loaded = created.context->LoadModule(module, bindings);
    Require(!loaded, loaded.detail);

    const x360port::ExecutionResult executed = created.context->Execute(kGearsResourceLeafAddress);
    Require(static_cast<bool>(executed), executed.failure.detail);
    Require(executed.value == 8U, "translated PPC did not return across the native import");
    Require(observations.dbg_print_calls == 1U,
            "typed DbgPrint import did not cross the native callback exactly once");
    Require(created.context->Statistics().translated_functions == 1U,
            "cold execution did not translate the synthetic Gears-addressed leaf");
    Require(created.context->Statistics().emitted_host_bytes > 0U,
            "Xenia reported no emitted host code");
    Require(created.context->Statistics().execution_calls == 1U,
            "the translated guest call was not counted");

    std::cout << "Gears/x360port boundary: synthetic PPC at 0x8222E868 translated by Xenia, "
                 "called typed DbgPrint, and returned to native code\n";
    return 0;
}
