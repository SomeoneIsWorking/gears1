#include "gears1_guest_image.h"
#include "input.h"

#include <algorithm>
#include <array>
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

#include <x360port/runtime.hpp>
#include <x360port/xam_input.hpp>

#include "titles/gears1/xam_input_provider.h"
#include "titles/gears1/xam_video_services.h"

namespace
{

using x360port::GuestAddress;

constexpr GuestAddress kResourceAddRef = 0x82233668U;

constexpr std::array<std::uint8_t, 32> kContainerDigest{
    0xdf, 0x10, 0x41, 0xda, 0x72, 0xd2, 0xb9, 0x47, 0xe3, 0xbb, 0x2f, 0x70, 0x1a, 0x19, 0xfd, 0xe9,
    0xe8, 0x44, 0x88, 0xdc, 0x84, 0x8f, 0xea, 0x2d, 0xec, 0xd6, 0xd4, 0x34, 0x34, 0xef, 0xe2, 0xd1};
constexpr std::array<std::uint8_t, 32> kImageDigest{
    0xf6, 0x1c, 0xc7, 0x8e, 0x40, 0x57, 0xbc, 0x68, 0xa2, 0xc6, 0x53, 0x86, 0xa0, 0x34, 0x1f, 0x6d,
    0x26, 0xa7, 0xad, 0xd3, 0xdf, 0xd9, 0x91, 0x80, 0x07, 0xa4, 0x55, 0x75, 0x0e, 0xc6, 0xed, 0x5c};

struct Observations
{
    std::uint32_t function_calls = 0;
    std::uint32_t variable_resolutions = 0;
    std::uint32_t override_calls = 0;
    std::vector<GuestAddress> variable_addresses;
    GuestAddress capture_address = 0;
    bool capture_read = false;
    std::array<std::byte, 20> captured_state{};
};

void RefuseUnsupportedImport(x360port::GuestImportContext &call, void *context) noexcept
{
    auto &observations = *static_cast<Observations *>(context);
    ++observations.function_calls;
    if (observations.capture_address != 0U)
    {
        observations.capture_read =
            call.read_memory(observations.capture_address, observations.captured_state);
    }
    call.refuse(x360port::ImportRefusalReason::UnsupportedService);
}

GuestAddress ResolveVariable(void *context) noexcept
{
    auto &observations = *static_cast<Observations *>(context);
    if (observations.variable_resolutions >= observations.variable_addresses.size())
    {
        return 0;
    }
    const GuestAddress address = observations.variable_addresses[observations.variable_resolutions];
    ++observations.variable_resolutions;
    return address;
}

x360port::ExecutionResult ScopedOriginal(x360port::RuntimeContext &runtime, GuestAddress address,
                                         std::span<const std::uint64_t> arguments,
                                         void *context) noexcept
{
    ++static_cast<Observations *>(context)->override_calls;
    return runtime.CallOriginal(address, arguments);
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

    gears::Gears1GuestImage module;
    std::string error;
    Require(module.InitializeCheckedXex(xex, expected, error), error);
    Require(module.ImportManifest().size() == 236U, "the real import manifest was not retained");
    const std::size_t xam_imports = static_cast<std::size_t>(std::count_if(
        module.ImportManifest().begin(), module.ImportManifest().end(),
        [](const x360port::ImportRequirement &import) { return import.library == "xam.xex"; }));
    const std::size_t xboxkrnl_imports = static_cast<std::size_t>(
        std::count_if(module.ImportManifest().begin(), module.ImportManifest().end(),
                      [](const x360port::ImportRequirement &import)
                      { return import.library == "xboxkrnl.exe"; }));
    Require(xam_imports == 92U && xboxkrnl_imports == 144U,
            "the real import manifest did not resolve its two XEX library-table entries");

    Observations observations;
    gears::titles::gears1::XamVideoServices video_services(8U);
    x360port::XamInputService input_service(gears::titles::gears1::ReadXamPad,
                                            gears::titles::gears1::ReadXamCapabilities, nullptr);
    x360port::RuntimeCreateResult created = x360port::RuntimeContext::Create();
    Require(static_cast<bool>(created), created.failure.detail);

    const std::size_t variable_count = static_cast<std::size_t>(
        std::count_if(module.ImportManifest().begin(), module.ImportManifest().end(),
                      [](const x360port::ImportRequirement &import)
                      { return import.kind == x360port::ImportKind::Variable; }));
    x360port::GuestMemoryAllocationResult variable_memory;
    if (variable_count != 0U)
    {
        variable_memory = created.context->AllocateGuestMemory(
            static_cast<std::uint32_t>(variable_count * sizeof(GuestAddress)));
        Require(static_cast<bool>(variable_memory), variable_memory.failure.detail);
    }
    observations.variable_addresses.reserve(variable_count);
    for (std::size_t index = 0; index < variable_count; ++index)
    {
        observations.variable_addresses.push_back(
            variable_memory.allocation.address +
            static_cast<GuestAddress>(index * sizeof(GuestAddress)));
    }

    std::vector<x360port::ImportBinding> bindings;
    bindings.reserve(module.ImportManifest().size());
    for (const x360port::ImportRequirement &import : module.ImportManifest())
    {
        x360port::ImportBinding binding{.library = import.library,
                                        .ordinal = import.ordinal,
                                        .kind = import.kind,
                                        .function_handler = RefuseUnsupportedImport,
                                        .function_context = &observations};
        if (import.kind == x360port::ImportKind::Variable)
        {
            binding.function_handler = nullptr;
            binding.function_context = nullptr;
            binding.variable_resolver = ResolveVariable;
            binding.variable_resolution_context = &observations;
        }
        video_services.Bind(import, binding);
        input_service.Bind(import, binding);
        bindings.push_back(binding);
    }
    std::vector<x360port::ImportBinding> mismatched_bindings = bindings;
    mismatched_bindings.front().library = "am.xex";
    const x360port::RuntimeFailure mismatch =
        created.context->LoadModule(module, mismatched_bindings);
    Require(static_cast<bool>(mismatch), "a mismatched import-library binding was accepted");

    const auto av_pack_import =
        std::find_if(module.ImportManifest().begin(), module.ImportManifest().end(),
                     [](const x360port::ImportRequirement &import)
                     { return import.library == "xam.xex" && import.ordinal == 971U; });
    Require(av_pack_import != module.ImportManifest().end(),
            "the real image did not retain the XGetAVPack import");
    const auto input_import =
        std::find_if(module.ImportManifest().begin(), module.ImportManifest().end(),
                     [](const x360port::ImportRequirement &import)
                     {
                         return import.library == "xam.xex" &&
                                import.ordinal == x360port::kXamInputGetStateOrdinal;
                     });
    Require(input_import != module.ImportManifest().end(),
            "the real image did not retain the XamInputGetState import");
    const auto capabilities_import =
        std::find_if(module.ImportManifest().begin(), module.ImportManifest().end(),
                     [](const x360port::ImportRequirement &import)
                     {
                         return import.library == "xam.xex" &&
                                import.ordinal == x360port::kXamInputGetCapabilitiesOrdinal;
                     });
    Require(capabilities_import != module.ImportManifest().end(),
            "the real image did not retain the XamInputGetCapabilities import");

    const x360port::GuestMemoryAllocationResult object_memory =
        created.context->AllocateGuestMemory(0x1CU);
    Require(static_cast<bool>(object_memory), object_memory.failure.detail);
    std::array<std::byte, 0x1C> object_bytes{};
    object_bytes[7] = std::byte{4};
    const x360port::RuntimeFailure object_written =
        created.context->WriteGuestMemory(object_memory.allocation.address, object_bytes);
    Require(!object_written, object_written.detail);
    const GuestAddress object = object_memory.allocation.address;
    const x360port::RuntimeFailure loaded = created.context->LoadModule(module, bindings);
    Require(!loaded, loaded.detail);
    Require(observations.variable_resolutions == variable_count,
            "the real image did not resolve every variable import into owned guest memory");
    const x360port::ExecutionResult av_pack = created.context->Execute(av_pack_import->address);
    Require(static_cast<bool>(av_pack) && av_pack.value == 8U && video_services.call_count() == 1U,
            "the title-owned XGetAVPack service did not execute through its import thunk");

    const auto first_function_import =
        std::find_if(module.ImportManifest().begin(), module.ImportManifest().end(),
                     [](const x360port::ImportRequirement &import)
                     { return import.kind == x360port::ImportKind::Function; });
    Require(first_function_import != module.ImportManifest().end(),
            "the real image did not retain a function import");
    const x360port::ExecutionResult imported_call =
        created.context->Execute(first_function_import->address);
    Require(imported_call.failure.error == x360port::RuntimeError::ImportServiceRefused,
            "an unimplemented real-image import silently returned into guest code");
    Require(observations.function_calls == 1U,
            "the real image function-import trampoline did not reach its title callback");
    Require(created.context->Statistics().import_service_refusals == 1U,
            "the real-image import refusal was not accounted for");

    const x360port::GuestMemoryAllocationResult state_memory =
        created.context->AllocateGuestMemory(20U);
    Require(static_cast<bool>(state_memory), state_memory.failure.detail);
    constexpr std::array<std::byte, 20> empty_input_record{};
    const x360port::RuntimeFailure input_record_initialized =
        created.context->WriteGuestMemory(state_memory.allocation.address, empty_input_record);
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
        created.context->Execute(input_import->address, state_arguments);
    Require(static_cast<bool>(connected) && connected.value == 0U,
            "the real XamInputGetState thunk did not poll the connected pad");
    observations.capture_address = state_memory.allocation.address;
    observations.capture_read = false;
    const x360port::ExecutionResult capture =
        created.context->Execute(first_function_import->address);
    constexpr std::array<std::byte, 20> expected_state{
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x01}, std::byte{0x10},
        std::byte{0x10}, std::byte{0x12}, std::byte{0x34}, std::byte{0xFB}, std::byte{0x2E},
        std::byte{0x23}, std::byte{0x45}, std::byte{0x45}, std::byte{0x67}, std::byte{0xF6},
        std::byte{0xD7}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};
    Require(capture.failure.error == x360port::RuntimeError::ImportServiceRefused &&
                observations.capture_read && observations.captured_state == expected_state,
            "the real input thunk did not publish the retained pad state in guest memory");

    const x360port::ExecutionResult capabilities =
        created.context->Execute(capabilities_import->address, state_arguments);
    Require(static_cast<bool>(capabilities) && capabilities.value == 0U,
            "the real XamInputGetCapabilities thunk did not report the connected virtual pad");
    observations.capture_read = false;
    const x360port::ExecutionResult capabilities_capture =
        created.context->Execute(first_function_import->address);
    constexpr std::array<std::byte, 20> expected_capabilities{
        std::byte{0x01}, std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0xFF},
        std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF},
        std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF},
        std::byte{0xFF}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};
    Require(capabilities_capture.failure.error == x360port::RuntimeError::ImportServiceRefused &&
                observations.capture_read && observations.captured_state == expected_capabilities,
            "the real capabilities thunk did not publish the virtual pad's 20-byte guest record");
    const std::array<std::uint64_t, 3> null_capabilities_arguments{0U, 0U, 0U};
    const x360port::ExecutionResult null_capabilities =
        created.context->Execute(capabilities_import->address, null_capabilities_arguments);
    Require(static_cast<bool>(null_capabilities) &&
                null_capabilities.value == x360port::kXamInputBadArguments,
            "the real capabilities thunk accepted a null guest pointer");

    const std::array<std::uint64_t, 3> query_arguments{0U, 0U, 0U};
    const x360port::ExecutionResult query =
        created.context->Execute(input_import->address, query_arguments);
    Require(static_cast<bool>(query) && query.value == 0U,
            "the real input thunk refused a connected-pad query");
    const std::array<std::uint64_t, 3> other_user_arguments{1U, 0U,
                                                            state_memory.allocation.address};
    const x360port::ExecutionResult other_user =
        created.context->Execute(input_import->address, other_user_arguments);
    Require(static_cast<bool>(other_user) &&
                other_user.value == x360port::kXamInputDeviceNotConnected,
            "the Gears one-local-user policy exposed a second controller slot");
    gears::DisconnectRemotePad();
    const x360port::ExecutionResult disconnected =
        created.context->Execute(input_import->address, state_arguments);
    Require(static_cast<bool>(disconnected) &&
                disconnected.value == x360port::kXamInputDeviceNotConnected,
            "the real input thunk reported a controller after its source disconnected");
    observations.capture_read = false;
    const x360port::ExecutionResult cleared_capture =
        created.context->Execute(first_function_import->address);
    Require(cleared_capture.failure.error == x360port::RuntimeError::ImportServiceRefused &&
                observations.capture_read && observations.captured_state == empty_input_record,
            "the disconnected input service left stale guest controller state");
    const x360port::ExecutionResult disconnected_capabilities =
        created.context->Execute(capabilities_import->address, state_arguments);
    Require(static_cast<bool>(disconnected_capabilities) &&
                disconnected_capabilities.value == x360port::kXamInputDeviceNotConnected,
            "the real capabilities thunk reported a disconnected controller");
    observations.capture_read = false;
    const x360port::ExecutionResult disconnected_capabilities_capture =
        created.context->Execute(first_function_import->address);
    Require(disconnected_capabilities_capture.failure.error ==
                    x360port::RuntimeError::ImportServiceRefused &&
                observations.capture_read && observations.captured_state == empty_input_record,
            "the disconnected capabilities thunk left stale guest bytes");
    const std::array<std::uint64_t, 3> invalid_state_arguments{0U, 0U, UINT32_MAX};
    const x360port::ExecutionResult invalid_state =
        created.context->Execute(input_import->address, invalid_state_arguments);
    Require(invalid_state.failure.error == x360port::RuntimeError::ImportServiceRefused,
            "the real input thunk accepted an unmapped state pointer");
    const x360port::ExecutionResult invalid_capabilities =
        created.context->Execute(capabilities_import->address, invalid_state_arguments);
    Require(invalid_capabilities.failure.error == x360port::RuntimeError::ImportServiceRefused,
            "the real capabilities thunk accepted an unmapped guest pointer");
    observations.capture_address = 0U;

    const std::uint32_t function_calls_before_leaf = observations.function_calls;
    const std::array<std::uint64_t, 1> arguments{object};
    const x360port::ExecutionResult baseline = created.context->Execute(kResourceAddRef, arguments);
    Require(static_cast<bool>(baseline), baseline.failure.detail);
    Require(baseline.value == 5U, "real AddRef leaf returned an unexpected baseline value");
    Require(observations.function_calls == function_calls_before_leaf,
            "real AddRef leaf disturbed the already-tested import callback count");

    const x360port::RuntimeFailure installed =
        created.context->InstallOverride(kResourceAddRef, ScopedOriginal, &observations);
    Require(!installed, installed.detail);
    const x360port::ExecutionResult overridden =
        created.context->Execute(kResourceAddRef, arguments);
    Require(static_cast<bool>(overridden), overridden.failure.detail);
    Require(overridden.value == 6U && observations.override_calls == 1U,
            "enabled native override did not take one scoped original path");
    Require(created.context->Statistics().original_calls == 1U,
            "scoped original call was not counted");

    const x360port::RuntimeFailure removed = created.context->RemoveOverride(kResourceAddRef);
    Require(!removed, removed.detail);
    const x360port::ExecutionResult disabled = created.context->Execute(kResourceAddRef, arguments);
    Require(static_cast<bool>(disabled) && disabled.value == 7U,
            "disabled override did not restore the original guest path");

    const x360port::RuntimeFailure invalidated =
        created.context->NotifyExecutableWrite(kResourceAddRef, 4U);
    Require(!invalidated, invalidated.detail);
    const x360port::ExecutionResult after_invalidation =
        created.context->Execute(kResourceAddRef, arguments);
    Require(static_cast<bool>(after_invalidation) && after_invalidation.value == 8U,
            "real guest execution did not resume after explicit invalidation");
    Require(created.context->Statistics().translation_invalidations >= 2U,
            "override removal and executable write did not invalidate translations");

    const x360port::RuntimeFailure released =
        created.context->ReleaseGuestMemory(object_memory.allocation);
    Require(!released, released.detail);
    const x360port::RuntimeFailure state_released =
        created.context->ReleaseGuestMemory(state_memory.allocation);
    Require(!state_released, state_released.detail);
    if (variable_memory)
    {
        const x360port::RuntimeFailure variables_released =
            created.context->ReleaseGuestMemory(variable_memory.allocation);
        Require(!variables_released, variables_released.detail);
    }

    std::cout << "Gears real-image discriminator: checked XEX, resolved 236 imports into "
                 "owned guest storage, polled retained pad state/capabilities through 401/400, "
                 "executed 0x82233668, "
                 "scoped original, and executable invalidation passed\n";
    return 0;
}
