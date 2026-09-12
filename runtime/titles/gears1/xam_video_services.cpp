#include "xam_video_services.h"

namespace gears::titles::gears1
{
namespace
{

constexpr std::uint32_t kXGetAVPackOrdinal = 971U;

} // namespace

void XamVideoServices::Bind(const x360port::ImportRequirement &requirement,
                            x360port::ImportBinding &binding) noexcept
{
    if (requirement.kind != x360port::ImportKind::Function || requirement.library != "xam.xex" ||
        requirement.ordinal != kXGetAVPackOrdinal)
    {
        return;
    }
    binding.function_handler = GetAVPack;
    binding.function_context = this;
}

void XamVideoServices::GetAVPack(x360port::GuestImportContext &context, void *service) noexcept
{
    auto &video = *static_cast<XamVideoServices *>(service);
    ++video.call_count_;
    context.set_return_value(video.av_pack_);
}

} // namespace gears::titles::gears1
