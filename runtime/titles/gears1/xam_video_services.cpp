#include "xam_video_services.h"

namespace gears::titles::gears1
{
x360port::ImportClaim XamVideoServices::Claim() noexcept
{
    return {.library = x360port::ExportNames::Library::Xam,
            .export_name = "XGetAVPack",
            .handler = GetAVPack,
            .context = this};
}

void XamVideoServices::GetAVPack(x360port::GuestImportContext &context, void *service) noexcept
{
    auto &video = *static_cast<XamVideoServices *>(service);
    ++video.call_count_;
    context.set_return_value(video.av_pack_);
}

} // namespace gears::titles::gears1
