#include "gears1_runtime.h"

#include <limits>
#include <string>
#include <utility>

#include "titles/gears1/xam_input_provider.h"

namespace gears
{

Gears1Runtime::Gears1Runtime(std::uint32_t av_pack, x360port::XamPadReader state_reader,
                             x360port::XamCapabilitiesReader capabilities_reader,
                             void *input_context) noexcept
    : video_services_(av_pack), input_service_(state_reader, capabilities_reader, input_context)
{
}

x360port::RuntimeFailure Gears1Runtime::Initialize(std::span<const std::byte> normalized_image,
                                                   const XexIdentity &expected,
                                                   std::span<const ImportSpec> imports)
{
    Reset();

    std::string error;
    if (!module_.Initialize(normalized_image, expected, imports, error))
    {
        return {x360port::RuntimeError::ModuleValidationFailed, std::move(error)};
    }
    return InitializeContext();
}

x360port::RuntimeFailure Gears1Runtime::InitializeCheckedXex(std::span<const std::byte> xex,
                                                             const XexIdentity &expected)
{
    Reset();

    std::string error;
    if (!module_.InitializeCheckedXex(xex, expected, error))
    {
        return {x360port::RuntimeError::ModuleValidationFailed, std::move(error)};
    }
    return InitializeContext();
}

void Gears1Runtime::Reset()
{
    context_.reset();
    variable_storage_ = {};
    variable_count_ = 0;
    next_variable_ = 0;
    bindings_.clear();
}

x360port::RuntimeFailure Gears1Runtime::InitializeContext()
{
    x360port::RuntimeCreateResult created = x360port::RuntimeContext::Create();
    if (!created)
    {
        return created.failure;
    }
    context_ = std::move(created.context);

    for (const x360port::ImportRequirement &import : module_.ImportManifest())
    {
        variable_count_ += import.kind == x360port::ImportKind::Variable ? 1U : 0U;
    }
    if (variable_count_ >
        std::numeric_limits<std::uint32_t>::max() / sizeof(x360port::GuestAddress))
    {
        return {x360port::RuntimeError::VariableResolutionFailed,
                "Gears variable-import manifest exceeds the bounded storage contract"};
    }
    if (variable_count_ != 0U)
    {
        const x360port::GuestMemoryAllocationResult allocated = context_->AllocateGuestMemory(
            static_cast<std::uint32_t>(variable_count_ * sizeof(x360port::GuestAddress)));
        if (!allocated)
        {
            return allocated.failure;
        }
        variable_storage_ = allocated.allocation;
    }
    return ComposeBindings();
}

x360port::ExecutionResult Gears1Runtime::ExecuteEntry(std::span<const std::uint64_t> arguments,
                                                      x360port::ExecutionLimits limits)
{
    if (context_ == nullptr)
    {
        return {{x360port::RuntimeError::LoadStateInvalid,
                 "Gears runtime entry requested before authenticated initialization"},
                0};
    }
    return context_->Execute(module_.Descriptor().image.entry_point, arguments, limits);
}

const x360port::JitStatistics *Gears1Runtime::Statistics() const noexcept
{
    return context_ == nullptr ? nullptr : &context_->Statistics();
}

void Gears1Runtime::RefuseUnsupportedImport(x360port::GuestImportContext &call, void *) noexcept
{
    call.refuse(x360port::ImportRefusalReason::UnsupportedService);
}

x360port::GuestAddress Gears1Runtime::ResolveVariable(void *context) noexcept
{
    auto &runtime = *static_cast<Gears1Runtime *>(context);
    if (runtime.next_variable_ >= runtime.variable_count_)
    {
        return 0;
    }
    const x360port::GuestAddress address =
        runtime.variable_storage_.address +
        static_cast<x360port::GuestAddress>(runtime.next_variable_ *
                                            sizeof(x360port::GuestAddress));
    ++runtime.next_variable_;
    return address;
}

x360port::RuntimeFailure Gears1Runtime::ComposeBindings()
{
    bindings_.reserve(module_.ImportManifest().size());
    for (const x360port::ImportRequirement &import : module_.ImportManifest())
    {
        x360port::ImportBinding binding{.library = import.library,
                                        .ordinal = import.ordinal,
                                        .kind = import.kind,
                                        .function_handler = RefuseUnsupportedImport,
                                        .function_context = this};
        if (import.kind == x360port::ImportKind::Variable)
        {
            binding.function_handler = nullptr;
            binding.function_context = nullptr;
            binding.variable_resolver = ResolveVariable;
            binding.variable_resolution_context = this;
        }
        video_services_.Bind(import, binding);
        input_service_.Bind(import, binding);
        bindings_.push_back(binding);
    }
    next_variable_ = 0;
    return context_->LoadModule(module_, bindings_);
}

} // namespace gears
