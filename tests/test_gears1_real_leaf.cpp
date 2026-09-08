#include "gears1_guest_image.h"

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
};

void UnexpectedImport(void *, void *, void *context) noexcept
{
    ++static_cast<Observations *>(context)->function_calls;
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
    Observations observations;
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
                                        .function_handler = UnexpectedImport,
                                        .function_context = &observations};
        if (import.kind == x360port::ImportKind::Variable)
        {
            binding.function_handler = nullptr;
            binding.function_context = nullptr;
            binding.variable_resolver = ResolveVariable;
            binding.variable_resolution_context = &observations;
        }
        bindings.push_back(binding);
    }

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

    const auto first_function_import =
        std::find_if(module.ImportManifest().begin(), module.ImportManifest().end(),
                     [](const x360port::ImportRequirement &import)
                     { return import.kind == x360port::ImportKind::Function; });
    Require(first_function_import != module.ImportManifest().end(),
            "the real image did not retain a function import");
    const x360port::ExecutionResult imported_call =
        created.context->Execute(first_function_import->address);
    Require(static_cast<bool>(imported_call), imported_call.failure.detail);
    Require(observations.function_calls == 1U,
            "the real image function-import trampoline did not reach its title callback");

    const std::array<std::uint64_t, 1> arguments{object};
    const x360port::ExecutionResult baseline = created.context->Execute(kResourceAddRef, arguments);
    Require(static_cast<bool>(baseline), baseline.failure.detail);
    Require(baseline.value == 5U, "real AddRef leaf returned an unexpected baseline value");
    Require(observations.function_calls == 1U,
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
    if (variable_memory)
    {
        const x360port::RuntimeFailure variables_released =
            created.context->ReleaseGuestMemory(variable_memory.allocation);
        Require(!variables_released, variables_released.detail);
    }

    std::cout << "Gears real-image discriminator: checked XEX, resolved 236 imports into "
                 "owned guest storage, invoked a real function thunk, executed 0x82233668, "
                 "scoped original, and executable invalidation passed\n";
    return 0;
}
