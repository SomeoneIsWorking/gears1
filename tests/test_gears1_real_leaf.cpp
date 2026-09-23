#include "gears1_runtime.h"
#include "input.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <x360port/export_names.hpp>
#include <x360port/runtime.hpp>

#include "titles/gears1/audio_mix.h"
#include "titles/gears1/xam_input_provider.h"

namespace
{

using x360port::GuestAddress;

constexpr GuestAddress kResourceAddRef = 0x8222E868U;
constexpr std::size_t kResourceObjectSize = 0x1CU;

// Guest ABI values the kernel's virtual-memory exports take and answer with,
// restated from the console's documented constants so a wrong value in the
// service fails here instead of agreeing with itself.
constexpr std::uint32_t kMemCommit = 0x00001000U;
constexpr std::uint32_t kMemReserve = 0x00002000U;
constexpr std::uint32_t kMemRelease = 0x00008000U;
constexpr std::uint32_t kPageReadWrite = 0x00000004U;
constexpr std::uint32_t kAllocationGranularity = 64U * 1024U;
constexpr std::uint64_t kStatusSuccess = 0x00000000U;
constexpr std::uint64_t kStatusMemoryNotAllocated = 0xC00000A0U;

constexpr std::array<std::uint8_t, 32> kContainerDigest{
    0xdf, 0x10, 0x41, 0xda, 0x72, 0xd2, 0xb9, 0x47, 0xe3, 0xbb, 0x2f, 0x70, 0x1a, 0x19, 0xfd, 0xe9,
    0xe8, 0x44, 0x88, 0xdc, 0x84, 0x8f, 0xea, 0x2d, 0xec, 0xd6, 0xd4, 0x34, 0x34, 0xef, 0xe2, 0xd1};
constexpr std::array<std::uint8_t, 32> kImageDigest{
    0xf6, 0x1c, 0xc7, 0x8e, 0x40, 0x57, 0xbc, 0x68, 0xa2, 0xc6, 0x53, 0x86, 0xa0, 0x34, 0x1f, 0x6d,
    0x26, 0xa7, 0xad, 0xd3, 0xdf, 0xd9, 0x91, 0x80, 0x07, 0xa4, 0x55, 0x75, 0x0e, 0xc6, 0xed, 0x5c};

struct Observations
{
    std::uint32_t override_calls = 0;
};

x360port::ExecutionResult ScopedOriginal(x360port::GuestCallContext &call, GuestAddress address,
                                         std::span<const std::uint64_t> arguments,
                                         void *context) noexcept
{
    ++static_cast<Observations *>(context)->override_calls;
    return call.CallOriginalBody(address, arguments);
}

[[noreturn]] void Fail(std::string_view message)
{
    std::cerr << "Gears real-leaf discriminator failed: " << message << '\n';
    std::exit(1);
}

void Require(bool condition, std::string_view message)
{
    if (!condition)
    {
        Fail(message);
    }
}

// Locates the manifest entry for a named export. The export table owns the
// ordinal, so the discriminator names the service the real image must retain
// instead of repeating its number.
[[nodiscard]] const x360port::ImportRequirement *
FindImport(std::span<const x360port::ImportRequirement> imports,
           x360port::ExportNames::Library library, std::string_view export_name)
{
    const auto exported = x360port::ExportNames::Find(library, export_name);
    if (!exported.has_value())
    {
        Fail("that library does not declare a service this title requires");
    }
    const std::string_view library_name = library == x360port::ExportNames::Library::Kernel
                                              ? x360port::ExportNames::kernel_library_name
                                              : x360port::ExportNames::xam_library_name;
    const auto found = std::ranges::find_if(
        imports, [&exported, library_name](const x360port::ImportRequirement &import)
        { return import.library == library_name && import.ordinal == exported->ordinal; });
    return found == imports.end() ? nullptr : &*found;
}

[[nodiscard]] const x360port::ImportRequirement *
FindXamImport(std::span<const x360port::ImportRequirement> imports, std::string_view export_name)
{
    return FindImport(imports, x360port::ExportNames::Library::Xam, export_name);
}

[[nodiscard]] const x360port::ImportRequirement *
FindKernelImport(std::span<const x360port::ImportRequirement> imports, std::string_view export_name)
{
    return FindImport(imports, x360port::ExportNames::Library::Kernel, export_name);
}

// One in/out guest word pair, which is how the kernel's virtual-memory exports
// take their base address and region size.
class MemoryArguments final
{
  public:
    MemoryArguments(x360port::RuntimeContext &context, x360port::GuestAddress address) noexcept
        : context_(&context), address_(address)
    {
    }

    [[nodiscard]] x360port::GuestAddress base_pointer() const noexcept { return address_; }
    [[nodiscard]] x360port::GuestAddress size_pointer() const noexcept { return address_ + 4U; }

    void Set(std::uint32_t base, std::uint32_t size) const
    {
        Store(address_, base);
        Store(address_ + 4U, size);
    }

    [[nodiscard]] std::uint32_t base() const { return Load(address_); }
    [[nodiscard]] std::uint32_t size() const { return Load(address_ + 4U); }

  private:
    void Store(x360port::GuestAddress address, std::uint32_t value) const
    {
        const std::array<std::byte, 4> bytes{
            static_cast<std::byte>(value >> 24U), static_cast<std::byte>(value >> 16U),
            static_cast<std::byte>(value >> 8U), static_cast<std::byte>(value)};
        const x360port::RuntimeFailure failure = context_->WriteGuestMemory(address, bytes);
        Require(!failure, failure.detail);
    }

    [[nodiscard]] std::uint32_t Load(x360port::GuestAddress address) const
    {
        std::array<std::byte, 4> bytes{};
        const x360port::RuntimeFailure failure = context_->ReadGuestMemory(address, bytes);
        Require(!failure, failure.detail);
        return (static_cast<std::uint32_t>(bytes[0]) << 24U) |
               (static_cast<std::uint32_t>(bytes[1]) << 16U) |
               (static_cast<std::uint32_t>(bytes[2]) << 8U) | static_cast<std::uint32_t>(bytes[3]);
    }

    x360port::RuntimeContext *context_;
    x360port::GuestAddress address_;
};

std::vector<std::byte> ReadFile(const char *path)
{
    std::ifstream input(path, std::ios::binary);
    const std::vector<char> raw((std::istreambuf_iterator<char>(input)), {});
    Require(input.good() || input.eof(), "could not read the supplied XEX");
    std::vector<std::byte> bytes;
    bytes.reserve(raw.size());
    for (const char value : raw)
    {
        bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(value)));
    }
    Require(!bytes.empty(), "the supplied XEX is empty");
    return bytes;
}

void PutBigEndianFloat(std::span<std::byte> bytes, std::size_t offset, float value)
{
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
    bytes[offset] = static_cast<std::byte>(bits >> 24U);
    bytes[offset + 1U] = static_cast<std::byte>(bits >> 16U);
    bytes[offset + 2U] = static_cast<std::byte>(bits >> 8U);
    bytes[offset + 3U] = static_cast<std::byte>(bits);
}

void FillAudioFixture(std::span<std::byte> input, std::span<std::byte> output,
                      std::span<std::byte> coefficient0, std::span<std::byte> coefficient1)
{
    for (std::size_t offset = 0; offset < input.size(); offset += 4U)
    {
        PutBigEndianFloat(input, offset, 0.125F + static_cast<float>(offset) * 0.0001F);
        PutBigEndianFloat(output, offset, -0.25F + static_cast<float>(offset) * 0.0002F);
    }
    PutBigEndianFloat(coefficient0, 0U, 0.125F);
    PutBigEndianFloat(coefficient0, 4U, -0.25F);
    PutBigEndianFloat(coefficient0, 8U, 0.5F);
    PutBigEndianFloat(coefficient0, 12U, -0.75F);
    PutBigEndianFloat(coefficient1, 0U, -0.0625F);
    PutBigEndianFloat(coefficient1, 4U, 0.1875F);
    PutBigEndianFloat(coefficient1, 8U, -0.3125F);
    PutBigEndianFloat(coefficient1, 12U, 0.4375F);
}

} // namespace

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        std::cerr << "usage: test_gears1_real_leaf <default.xex>\n";
        return 2;
    }

    const std::vector<std::byte> xex = ReadFile(argv[1]);
    const gears::XexIdentity expected{.containerDigest = kContainerDigest,
                                      .imageDigest = kImageDigest,
                                      .imageBase = 0x82000000U,
                                      .imageSize = 13500416U,
                                      .entryPoint = 0x82612BF0U};

    gears::Gears1Runtime runtime(8U, gears::titles::gears1::ReadXamPad,
                                 gears::titles::gears1::ReadXamCapabilities, nullptr);
    const x360port::RuntimeFailure initialized = runtime.InitializeCheckedXex(xex, expected);
    Require(!initialized, initialized.detail);
    const std::span<const x360port::ImportRequirement> imports = runtime.ImportManifest();
    Require(imports.size() == 236U, "the real import manifest was not retained");
    const std::size_t xam_imports = static_cast<std::size_t>(
        std::count_if(imports.begin(), imports.end(), [](const x360port::ImportRequirement &import)
                      { return import.library == "xam.xex"; }));
    const std::size_t xboxkrnl_imports = static_cast<std::size_t>(
        std::count_if(imports.begin(), imports.end(), [](const x360port::ImportRequirement &import)
                      { return import.library == "xboxkrnl.exe"; }));
    Require(xam_imports == 92U && xboxkrnl_imports == 144U,
            "the real import manifest did not resolve its two XEX library-table entries");

    Observations observations;

    const x360port::ImportRequirement *av_pack_import = FindXamImport(imports, "XGetAVPack");
    Require(av_pack_import != nullptr, "the real image did not retain the XGetAVPack import");
    const x360port::ImportRequirement *input_import = FindXamImport(imports, "XamInputGetState");
    Require(input_import != nullptr, "the real image did not retain the XamInputGetState import");
    const x360port::ImportRequirement *capabilities_import =
        FindXamImport(imports, "XamInputGetCapabilities");
    Require(capabilities_import != nullptr,
            "the real image did not retain the XamInputGetCapabilities import");

    x360port::RuntimeContext *context = runtime.Context();
    Require(context != nullptr, "the authenticated runtime did not retain its execution context");

    const x360port::GuestMemoryAllocationResult object_memory = context->AllocateGuestMemory(0x1CU);
    Require(static_cast<bool>(object_memory), object_memory.failure.detail);
    std::array<std::byte, 0x1C> object_bytes{};
    object_bytes[7] = std::byte{4};
    const x360port::RuntimeFailure object_written =
        context->WriteGuestMemory(object_memory.allocation.address, object_bytes);
    Require(!object_written, object_written.detail);
    const GuestAddress object = object_memory.allocation.address;
    const x360port::ExecutionResult av_pack = context->Execute(av_pack_import->address);
    Require(static_cast<bool>(av_pack) && av_pack.value == 8U,
            "the title-owned XGetAVPack service did not execute through its import thunk");

    const auto first_function_import =
        std::find_if(imports.begin(), imports.end(), [](const x360port::ImportRequirement &import)
                     { return import.kind == x360port::ImportKind::Function; });
    Require(first_function_import != imports.end(),
            "the real image did not retain a function import");
    const x360port::ExecutionResult imported_call =
        context->Execute(first_function_import->address);
    Require(imported_call.failure.error == x360port::RuntimeError::ImportServiceRefused,
            "an unimplemented real-image import silently returned into guest code");
    Require(context->Statistics().import_service_refusals == 1U,
            "the real-image import refusal was not accounted for");

    const x360port::GuestMemoryAllocationResult state_memory = context->AllocateGuestMemory(20U);
    Require(static_cast<bool>(state_memory), state_memory.failure.detail);
    constexpr std::array<std::byte, 20> empty_input_record{};
    const x360port::RuntimeFailure input_record_initialized =
        context->WriteGuestMemory(state_memory.allocation.address, empty_input_record);
    Require(!input_record_initialized, input_record_initialized.detail);
    const gears::PadState commanded{.buttons = gears::kPadA | gears::kPadStart,
                                    .leftTrigger = 0x12U,
                                    .rightTrigger = 0x34U,
                                    .thumbLX = -1234,
                                    .thumbLY = 0x2345,
                                    .thumbRX = 0x4567,
                                    .thumbRY = -2345};
    Require(gears::SetRemotePad(commanded), "the retained input owner rejected a remote pad");
    const std::array<std::uint64_t, 3> state_arguments{0U, 0U, state_memory.allocation.address};
    const x360port::ExecutionResult connected =
        context->Execute(input_import->address, state_arguments);
    Require(static_cast<bool>(connected) && connected.value == 0U,
            "the real XamInputGetState thunk did not poll the connected pad");
    std::array<std::byte, 20> captured_state{};
    const x360port::RuntimeFailure captured_state_read =
        context->ReadGuestMemory(state_memory.allocation.address, captured_state);
    constexpr std::array<std::byte, 20> expected_state{
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x01}, std::byte{0x10},
        std::byte{0x10}, std::byte{0x12}, std::byte{0x34}, std::byte{0xFB}, std::byte{0x2E},
        std::byte{0x23}, std::byte{0x45}, std::byte{0x45}, std::byte{0x67}, std::byte{0xF6},
        std::byte{0xD7}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};
    Require(!captured_state_read && captured_state == expected_state,
            "the real input thunk did not publish the retained pad state in guest memory");

    const x360port::ExecutionResult capabilities =
        context->Execute(capabilities_import->address, state_arguments);
    Require(static_cast<bool>(capabilities) && capabilities.value == 0U,
            "the real XamInputGetCapabilities thunk did not report the connected virtual pad");
    captured_state.fill(std::byte{0});
    const x360port::RuntimeFailure capabilities_capture_read =
        context->ReadGuestMemory(state_memory.allocation.address, captured_state);
    constexpr std::array<std::byte, 20> expected_capabilities{
        std::byte{0x01}, std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0xFF},
        std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF},
        std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF},
        std::byte{0xFF}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};
    Require(!capabilities_capture_read && captured_state == expected_capabilities,
            "the real capabilities thunk did not publish the virtual pad's 20-byte guest record");
    const std::array<std::uint64_t, 3> null_capabilities_arguments{0U, 0U, 0U};
    const x360port::ExecutionResult null_capabilities =
        context->Execute(capabilities_import->address, null_capabilities_arguments);
    Require(static_cast<bool>(null_capabilities) &&
                null_capabilities.value == x360port::kXamInputBadArguments,
            "the real capabilities thunk accepted a null guest pointer");

    const std::array<std::uint64_t, 3> query_arguments{0U, 0U, 0U};
    const x360port::ExecutionResult query =
        context->Execute(input_import->address, query_arguments);
    Require(static_cast<bool>(query) && query.value == 0U,
            "the real input thunk refused a connected-pad query");
    const std::array<std::uint64_t, 3> other_user_arguments{1U, 0U,
                                                            state_memory.allocation.address};
    const x360port::ExecutionResult other_user =
        context->Execute(input_import->address, other_user_arguments);
    Require(static_cast<bool>(other_user) &&
                other_user.value == x360port::kXamInputDeviceNotConnected,
            "the Gears one-local-user policy exposed a second controller slot");
    gears::DisconnectRemotePad();
    const x360port::ExecutionResult disconnected =
        context->Execute(input_import->address, state_arguments);
    Require(static_cast<bool>(disconnected) &&
                disconnected.value == x360port::kXamInputDeviceNotConnected,
            "the real input thunk reported a controller after its source disconnected");
    captured_state.fill(std::byte{0});
    const x360port::RuntimeFailure cleared_capture_read =
        context->ReadGuestMemory(state_memory.allocation.address, captured_state);
    Require(!cleared_capture_read && captured_state == empty_input_record,
            "the disconnected input service left stale guest controller state");
    const x360port::ExecutionResult disconnected_capabilities =
        context->Execute(capabilities_import->address, state_arguments);
    Require(static_cast<bool>(disconnected_capabilities) &&
                disconnected_capabilities.value == x360port::kXamInputDeviceNotConnected,
            "the real capabilities thunk reported a disconnected controller");
    captured_state.fill(std::byte{0});
    const x360port::RuntimeFailure disconnected_capabilities_capture_read =
        context->ReadGuestMemory(state_memory.allocation.address, captured_state);
    Require(!disconnected_capabilities_capture_read && captured_state == empty_input_record,
            "the disconnected capabilities thunk left stale guest bytes");
    const std::array<std::uint64_t, 3> invalid_state_arguments{0U, 0U, UINT32_MAX};
    const x360port::ExecutionResult invalid_state =
        context->Execute(input_import->address, invalid_state_arguments);
    Require(invalid_state.failure.error == x360port::RuntimeError::ImportServiceRefused,
            "the real input thunk accepted an unmapped state pointer");
    const x360port::ExecutionResult invalid_capabilities =
        context->Execute(capabilities_import->address, invalid_state_arguments);
    Require(invalid_capabilities.failure.error == x360port::RuntimeError::ImportServiceRefused,
            "the real capabilities thunk accepted an unmapped guest pointer");
    // The image's own virtual-memory imports reach the runtime's kernel
    // services. Nothing about these ordinals is synthetic: they are the entries
    // Epic shipped, resolved by name through the export table.
    const x360port::ImportRequirement *allocate_import =
        FindKernelImport(imports, "NtAllocateVirtualMemory");
    Require(allocate_import != nullptr,
            "the real image did not retain the NtAllocateVirtualMemory import");
    const x360port::ImportRequirement *free_import =
        FindKernelImport(imports, "NtFreeVirtualMemory");
    Require(free_import != nullptr, "the real image did not retain the NtFreeVirtualMemory import");

    const x360port::GuestMemoryAllocationResult memory_arguments = context->AllocateGuestMemory(8U);
    Require(static_cast<bool>(memory_arguments), memory_arguments.failure.detail);
    const MemoryArguments virtual_memory(*context, memory_arguments.allocation.address);
    virtual_memory.Set(0U, 0x1000U);
    const std::array<std::uint64_t, 5> allocate_arguments{
        virtual_memory.base_pointer(), virtual_memory.size_pointer(), kMemCommit | kMemReserve,
        kPageReadWrite, 0U};
    const x360port::ExecutionResult allocated =
        context->Execute(allocate_import->address, allocate_arguments);
    Require(static_cast<bool>(allocated), allocated.failure.detail);
    Require(allocated.value == kStatusSuccess,
            "the real NtAllocateVirtualMemory import did not commit a range");
    const std::uint32_t allocated_address = virtual_memory.base();
    Require(allocated_address != 0U && virtual_memory.size() == kAllocationGranularity,
            "the real allocation import did not report its address and rounded size");

    const std::array<std::uint64_t, 4> release_arguments{
        virtual_memory.base_pointer(), virtual_memory.size_pointer(), kMemRelease, 0U};
    const x360port::ExecutionResult range_released =
        context->Execute(free_import->address, release_arguments);
    Require(static_cast<bool>(range_released) && range_released.value == kStatusSuccess,
            "the real NtFreeVirtualMemory import did not release the range it allocated");

    // A free of address zero is ordinary traffic from this title's own wrapper.
    virtual_memory.Set(0U, 0U);
    const x360port::ExecutionResult free_of_zero =
        context->Execute(free_import->address, release_arguments);
    Require(static_cast<bool>(free_of_zero) && free_of_zero.value == kStatusMemoryNotAllocated,
            "the real free import did not answer a zero base as an unallocated address");

    const std::array<std::uint64_t, 1> arguments{object};
    const x360port::ExecutionResult baseline = context->Execute(kResourceAddRef, arguments);
    Require(static_cast<bool>(baseline), baseline.failure.detail);
    Require(baseline.value == 5U, "real AddRef leaf returned an unexpected baseline value");

    const x360port::RuntimeFailure installed =
        context->InstallOverride(kResourceAddRef, ScopedOriginal, &observations);
    Require(!installed, installed.detail);
    const x360port::ExecutionResult overridden = context->Execute(kResourceAddRef, arguments);
    Require(static_cast<bool>(overridden), overridden.failure.detail);
    Require(overridden.value == 6U && observations.override_calls == 1U,
            "enabled native override did not take one scoped original path");
    Require(context->Statistics().original_calls == 1U, "scoped original call was not counted");

    const std::uint64_t invalidations_before_removal =
        context->Statistics().translation_invalidations;
    const x360port::RuntimeFailure removed = context->RemoveOverride(kResourceAddRef);
    Require(!removed, removed.detail);
    const x360port::ExecutionResult disabled = context->Execute(kResourceAddRef, arguments);
    Require(static_cast<bool>(disabled) && disabled.value == 7U,
            "disabled override did not restore the original guest path");
    Require(context->Statistics().translation_invalidations == invalidations_before_removal,
            "override removal unnecessarily retranslated the original guest function");

    const x360port::GuestMemoryAllocationResult audio_input = context->AllocateGuestMemory(0x500U);
    const x360port::GuestMemoryAllocationResult audio_output = context->AllocateGuestMemory(0x500U);
    const x360port::GuestMemoryAllocationResult audio_coefficient0 =
        context->AllocateGuestMemory(16U);
    const x360port::GuestMemoryAllocationResult audio_coefficient1 =
        context->AllocateGuestMemory(16U);
    Require(static_cast<bool>(audio_input) && static_cast<bool>(audio_output) &&
                static_cast<bool>(audio_coefficient0) && static_cast<bool>(audio_coefficient1),
            "the runtime could not allocate the audio differential fixture");
    std::array<std::byte, 0x500> audio_input_bytes{};
    std::array<std::byte, 0x500> audio_output_bytes{};
    std::array<std::byte, 16> audio_coefficient0_bytes{};
    std::array<std::byte, 16> audio_coefficient1_bytes{};
    FillAudioFixture(audio_input_bytes, audio_output_bytes, audio_coefficient0_bytes,
                     audio_coefficient1_bytes);
    const auto write_audio_fixture = [&]
    {
        Require(!context->WriteGuestMemory(audio_input.allocation.address, audio_input_bytes),
                "could not write the audio input fixture");
        Require(!context->WriteGuestMemory(audio_output.allocation.address, audio_output_bytes),
                "could not write the audio output fixture");
        Require(!context->WriteGuestMemory(audio_coefficient0.allocation.address,
                                           audio_coefficient0_bytes),
                "could not write the first audio coefficient fixture");
        Require(!context->WriteGuestMemory(audio_coefficient1.allocation.address,
                                           audio_coefficient1_bytes),
                "could not write the second audio coefficient fixture");
    };
    write_audio_fixture();
    const std::array<std::uint64_t, 4> audio_arguments{
        audio_output.allocation.address, audio_input.allocation.address,
        audio_coefficient0.allocation.address, audio_coefficient1.allocation.address};
    constexpr x360port::ExecutionLimits audio_limits{.max_guest_blocks = 1000U,
                                                     .max_interpreter_instructions = 100'000U};
    const x360port::ExecutionResult audio_original = context->CallOriginal(
        gears::titles::gears1::kAudioMixAddress, audio_arguments, audio_limits);
    Require(static_cast<bool>(audio_original), audio_original.failure.detail);
    std::array<std::byte, 0x500> original_audio_output{};
    const x360port::RuntimeFailure original_audio_read =
        context->ReadGuestMemory(audio_output.allocation.address, original_audio_output);
    Require(!original_audio_read, original_audio_read.detail);
    write_audio_fixture();
    const std::uint64_t native_override_calls_before_audio =
        context->Statistics().native_override_calls;
    const x360port::ExecutionResult audio_native =
        context->Execute(gears::titles::gears1::kAudioMixAddress, audio_arguments);
    Require(static_cast<bool>(audio_native) && audio_native.value == audio_original.value,
            "the native audio override returned a different guest result");
    Require(context->Statistics().native_override_calls == native_override_calls_before_audio + 1U,
            "the authenticated audio address did not dispatch through the native override");
    std::array<std::byte, 0x500> native_audio_output{};
    const x360port::RuntimeFailure native_audio_read =
        context->ReadGuestMemory(audio_output.allocation.address, native_audio_output);
    if (native_audio_output != original_audio_output)
    {
        std::size_t differing = 0;
        for (std::size_t offset = 0; offset + 4U <= original_audio_output.size(); offset += 4U)
        {
            const auto word = [](std::span<const std::byte> bytes, std::size_t at)
            {
                return (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[at]))
                        << 24U) |
                       (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[at + 1U]))
                        << 16U) |
                       (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[at + 2U]))
                        << 8U) |
                       static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[at + 3U]));
            };
            const std::uint32_t expected_word = word(original_audio_output, offset);
            const std::uint32_t actual_word = word(native_audio_output, offset);
            if (expected_word == actual_word)
            {
                continue;
            }
            if (differing < 8U)
            {
                std::cerr << "audio divergence at +0x" << std::hex << offset
                          << ": original=" << expected_word << " native=" << actual_word << std::dec
                          << "\n";
            }
            ++differing;
        }
        std::cerr << "audio divergence: " << differing << " of "
                  << original_audio_output.size() / 4U << " output words differ\n";
    }
    Require(!native_audio_read && native_audio_output == original_audio_output,
            "the native audio override diverged from the Xenia original output");

    const x360port::RuntimeFailure invalidated =
        context->NotifyExecutableWrite(kResourceAddRef, 4U);
    Require(!invalidated, invalidated.detail);
    const x360port::ExecutionResult after_invalidation =
        context->Execute(kResourceAddRef, arguments);
    Require(static_cast<bool>(after_invalidation) && after_invalidation.value == 8U,
            "real guest execution did not resume after explicit invalidation");
    Require(context->Statistics().translation_invalidations == invalidations_before_removal + 1U,
            "the executable write did not invalidate the real guest translation exactly once");

    const x360port::GuestMemoryAllocationResult recursive_memory =
        context->AllocateGuestMemory(2U * kResourceObjectSize);
    Require(static_cast<bool>(recursive_memory), recursive_memory.failure.detail);
    const GuestAddress outer_resource = recursive_memory.allocation.address;
    const GuestAddress inner_resource = outer_resource + kResourceObjectSize;
    // The real leaf calls AddRef at 0x8222E8C0 for a first reference to this linked resource kind.
    std::array<std::byte, 2U * kResourceObjectSize> recursive_bytes{};
    recursive_bytes[0] = std::byte{0x40};
    recursive_bytes[3] = std::byte{0x04};
    recursive_bytes[24] = static_cast<std::byte>(inner_resource >> 24U);
    recursive_bytes[25] = static_cast<std::byte>(inner_resource >> 16U);
    recursive_bytes[26] = static_cast<std::byte>(inner_resource >> 8U);
    recursive_bytes[27] = static_cast<std::byte>(inner_resource);
    recursive_bytes[kResourceObjectSize + 7U] = std::byte{0x03};
    const x360port::RuntimeFailure recursive_written =
        context->WriteGuestMemory(outer_resource, recursive_bytes);
    Require(!recursive_written, recursive_written.detail);

    const std::uint32_t overrides_before_recursive_call = observations.override_calls;
    const x360port::RuntimeFailure recursive_installed =
        context->InstallOverride(kResourceAddRef, ScopedOriginal, &observations);
    Require(!recursive_installed, recursive_installed.detail);
    const std::array<std::uint64_t, 1> outer_arguments{outer_resource};
    const x360port::ExecutionResult recursive_override =
        context->Execute(kResourceAddRef, outer_arguments);
    Require(static_cast<bool>(recursive_override) && recursive_override.value == 1U &&
                observations.override_calls == overrides_before_recursive_call + 2U,
            "real AddRef's nested guest call did not enter the native override");
    const x360port::RuntimeFailure recursive_removed = context->RemoveOverride(kResourceAddRef);
    Require(!recursive_removed, recursive_removed.detail);

    const x360port::RuntimeFailure recursive_reset =
        context->WriteGuestMemory(outer_resource, recursive_bytes);
    Require(!recursive_reset, recursive_reset.detail);
    const x360port::ExecutionResult recursive_original =
        context->Execute(kResourceAddRef, outer_arguments);
    Require(static_cast<bool>(recursive_original) && recursive_original.value == 1U &&
                observations.override_calls == overrides_before_recursive_call + 2U,
            "removing the override did not restore the nested original guest call");
    const std::array<std::uint64_t, 1> inner_arguments{inner_resource};
    const x360port::ExecutionResult inner_after_original =
        context->Execute(kResourceAddRef, inner_arguments);
    Require(static_cast<bool>(inner_after_original) && inner_after_original.value == 5U,
            "the restored nested guest call did not increment the referenced resource");
    const x360port::RuntimeFailure recursive_released =
        context->ReleaseGuestMemory(recursive_memory.allocation);
    Require(!recursive_released, recursive_released.detail);

    const x360port::RuntimeFailure released = context->ReleaseGuestMemory(object_memory.allocation);
    Require(!released, released.detail);
    const x360port::RuntimeFailure state_released =
        context->ReleaseGuestMemory(state_memory.allocation);
    Require(!state_released, state_released.detail);

    std::cout << "Gears real-image discriminator: checked XEX, resolved 236 imports into "
                 "owned guest storage, polled retained pad state/capabilities through the "
                 "XamInputGetState and XamInputGetCapabilities claims, committed and released "
                 "a range through the retained NtAllocateVirtualMemory and "
                 "NtFreeVirtualMemory imports, executed 0x8222E868, "
                 "scoped original, nested guest-call override/removal, executable invalidation, "
                 "and the native audio mix at 0x825F2D40 matching the original guest body on "
                 "its return value and all 320 output words, passed\n";
    return 0;
}
