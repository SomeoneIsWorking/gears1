// Guest textures and samplers -> host images and samplers. See
// gpu_draw_textures.h for what this is and why every refusal is counted.

#include <cmath>
#include "gpu_draw_textures.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <cstring>

#include <lucent/config.h>
#include <lucent/log.h>

#include "gpu_draw_pixels.h"
#include "guest_texture_hash.h"

namespace gears::draw
{
namespace
{

using Clock = std::chrono::steady_clock;

uint64_t HashTextureStorage(const GuestTexture &texture, const FrameDrawInputs &inputs)
{
    const uint8_t *base =
        texture.baseGuestExtentBytes ? inputs.guestBase + texture.baseAddress : nullptr;
    const uint8_t *mips =
        texture.mipGuestExtentBytes ? inputs.guestBase + texture.mipAddress : nullptr;
    return gears::HashGuestTextureParts(base, texture.baseGuestExtentBytes, mips,
                                        texture.mipGuestExtentBytes);
}

VkFormat hostVkFormat(TexHostFormat f)
{
    switch (f)
    {
    case TexHostFormat::kR8Unorm:
        return VK_FORMAT_R8_UNORM;
    case TexHostFormat::kR8G8Unorm:
        return VK_FORMAT_R8G8_UNORM;
    case TexHostFormat::kR8G8B8A8Unorm:
        return VK_FORMAT_R8G8B8A8_UNORM;
    case TexHostFormat::kR5G6B5Pack16:
        return VK_FORMAT_B5G6R5_UNORM_PACK16;
    case TexHostFormat::kA1R5G5B5Pack16:
        return VK_FORMAT_A1R5G5B5_UNORM_PACK16;
    case TexHostFormat::kB4G4R4A4Pack16:
        return VK_FORMAT_B4G4R4A4_UNORM_PACK16;
    case TexHostFormat::kA2B10G10R10Pack32:
        return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case TexHostFormat::kR16Sfloat:
        return VK_FORMAT_R16_SFLOAT;
    case TexHostFormat::kR16G16Sfloat:
        return VK_FORMAT_R16G16_SFLOAT;
    case TexHostFormat::kR16G16B16A16Sfloat:
        return VK_FORMAT_R16G16B16A16_SFLOAT;
    case TexHostFormat::kR16Unorm:
        return VK_FORMAT_R16_UNORM;
    case TexHostFormat::kR16G16Unorm:
        return VK_FORMAT_R16G16_UNORM;
    case TexHostFormat::kR16G16B16A16Unorm:
        return VK_FORMAT_R16G16B16A16_UNORM;
    case TexHostFormat::kR32Sfloat:
        return VK_FORMAT_R32_SFLOAT;
    case TexHostFormat::kR32G32Sfloat:
        return VK_FORMAT_R32G32_SFLOAT;
    case TexHostFormat::kR32G32B32A32Sfloat:
        return VK_FORMAT_R32G32B32A32_SFLOAT;
    case TexHostFormat::kBc1RgbaUnorm:
        return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case TexHostFormat::kBc2Unorm:
        return VK_FORMAT_BC2_UNORM_BLOCK;
    case TexHostFormat::kBc3Unorm:
        return VK_FORMAT_BC3_UNORM_BLOCK;
    case TexHostFormat::kBc4Unorm:
        return VK_FORMAT_BC4_UNORM_BLOCK;
    case TexHostFormat::kBc5Unorm:
        return VK_FORMAT_BC5_UNORM_BLOCK;
    default:
        return VK_FORMAT_UNDEFINED;
    }
}

VkComponentSwizzle compSwizzle(uint8_t s)
{
    switch (s)
    {
    case 0:
        return VK_COMPONENT_SWIZZLE_R;
    case 1:
        return VK_COMPONENT_SWIZZLE_G;
    case 2:
        return VK_COMPONENT_SWIZZLE_B;
    case 3:
        return VK_COMPONENT_SWIZZLE_A;
    case 4:
        return VK_COMPONENT_SWIZZLE_ZERO;
    default:
        return VK_COMPONENT_SWIZZLE_ONE;
    }
}

VkSamplerAddressMode vkAddressMode(uint32_t clamp)
{
    switch (clamp)
    {
    case 0:
        return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 1:
        return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 2:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case 3:
        return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    // Half-way clamps have no host equivalent; edge is the closest and
    // is recorded as such rather than silently pretended to be exact.
    case 4:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case 5:
        return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    case 6:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    default:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    }
}

} // namespace

// Builds (once per distinct fetch) the host image for one texture fetch
// constant, or returns VK_NULL_HANDLE with the reason counted.
VkImageView TextureUploader::Upload(const uint32_t *fetch6, uint32_t wantDim)
{
    ++texBindingCalls;
    // The mip address/range and packed-tail flag live in dwords 4 and 5. A
    // partial descriptor key aliases different authored mip chains and keeps
    // sampling the first image uploaded for the shared base level.
    const RendererPersistent::GuestTextureKey key{fetch6[0], fetch6[1], fetch6[2],
                                                  fetch6[3], fetch6[4], fetch6[5]};
    auto it = texCache.find(key);
    if (it != texCache.end())
    {
        // IS THE CACHED IMAGE STILL WHAT THE GUEST HAS THERE? The key is the
        // fetch constant, which does not change when the guest overwrites the
        // pixels at the same address -- so without this, a texture the guest
        // rewrites in place is frozen at its first upload forever. That is not
        // theoretical: the startup movie hands the GPU new Y/U/V planes at the
        // same three addresses every frame, and the whole movie showed as one
        // stuck near-black image (catalog #53).
        //
        // Ask the decoder for the header only -- pure arithmetic on the fetch
        // constant, no data copied -- so the guest byte extent is known, then
        // hash those bytes and compare with what this entry was built from.
        GuestTexture header;
        if (it->second != VK_NULL_HANDLE && texCheckedThisFrame.insert(key).second &&
            DecodeGuestTexture(fetch6, in.guestBase, uint64_t(in.guestWindowBytes),
                               /*wantData=*/false, header) &&
            header.skipReason == nullptr && header.baseGuestExtentBytes != 0)
        {
            const auto tHash = Clock::now();
            const uint64_t now = HashTextureStorage(header, in);
            const double thisHashMs =
                std::chrono::duration<double, std::milli>(Clock::now() - tHash).count();
            msTexHash += thisHashMs;
            const uint64_t hashBytes =
                uint64_t(header.baseGuestExtentBytes) + header.mipGuestExtentBytes;
            texHashBytes += hashBytes;
            if (thisHashMs > texHashWorstMs)
            {
                texHashWorstMs = thisHashMs;
                texHashWorstBytes = hashBytes;
                texHashWorstBase = header.baseAddress;
            }
            ++texContentChecked;
            const auto known = texContentHash.find(key);
            if (known != texContentHash.end() && !gears::GuestTextureUnchanged(known->second, now))
            {
                // EVICT: retire the stale image and fall through to a fresh
                // upload of the bytes that are there now. The old image is not
                // destroyed here -- earlier draws of this same frame may
                // already reference it -- it goes on the retirement list and
                // dies after the frame's fence.
                ++texContentChanged;
                auto old = guestTextures.find(key);
                if (old != guestTextures.end())
                {
                    texRetired.push_back(old->second);
                    guestTextures.erase(old);
                }
                texCache.erase(key);
                texContentHash.erase(known);
            }
            else
            {
                return it->second;
            }
        }
        else
        {
            return it->second;
        }
    }
    texDistinct.insert(key);

    GuestTexture gt;
    if (!DecodeGuestTexture(fetch6, in.guestBase, uint64_t(in.guestWindowBytes), /*wantData=*/true,
                            gt))
    {
        ++texSkips["not a texture fetch constant"];
        texCache[key] = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }
    ++texFormatBindings[gt.formatName];
    {
        std::string s =
            std::format("{:#x} {} {}x{}x{} dim{} {} endian{} swizzle{:#05x} mips{}-{}{}",
                        gt.baseAddress, gt.formatName, gt.width, gt.height, gt.depthOrArraySize,
                        gt.dimension, gt.tiled ? "tiled" : "linear", gt.endian, gt.guestSwizzle,
                        gt.mipMin, gt.mipMax, gt.packedMips ? " packed" : "");
        ++texFormatCensus[s];
    }
    if (gt.skipReason)
    {
        ++texSkips[gt.skipReason];
        texCache[key] = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }
    // The shader's declared image type has to match the view type, and the
    // shader derived it from this same fetch's dimension -- a mismatch
    // means our decode disagrees with the translator, which we report
    // rather than paper over.
    const uint32_t declDim = gt.dimension <= 1 ? 1 : gt.dimension;
    if ((wantDim <= 1 ? 1u : wantDim) != declDim)
    {
        ++texSkips["shader/fetch dimension mismatch"];
        texCache[key] = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }
    const VkFormat vf = hostVkFormat(gt.hostFormat);
    if (vf == VK_FORMAT_UNDEFINED)
    {
        ++texSkips["no host VkFormat"];
        texCache[key] = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(R.physical, vf, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
    {
        ++texSkips["host format not sampleable on this R.device"];
        texCache[key] = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }

    const bool is3D = gt.dimension == 2;
    const bool isCube = gt.dimension == 3;
    GuestTex tex;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.flags = isCube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    ci.imageType = is3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ci.format = vf;
    ci.extent = {gt.width, gt.height, gt.depth3D};
    ci.mipLevels = uint32_t(gt.levels.size());
    ci.arrayLayers = gt.layers;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const bool canStore = FormatSupportsStorage(R.physical, ci.format);
    if (canStore)
        ci.usage |= VK_IMAGE_USAGE_STORAGE_BIT; // the resolve writes it
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(R.device, &ci, nullptr, &tex.image) != VK_SUCCESS)
    {
        ++texSkips["vkCreateImage failed"];
        texCache[key] = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(R.device, tex.image, &req);
    uint32_t mtype = 0;
    if (!R.FindMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, mtype))
        R.FindMemory(req.memoryTypeBits, 0, mtype);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = mtype;
    if (vkAllocateMemory(R.device, &ai, nullptr, &tex.mem) != VK_SUCCESS ||
        vkBindImageMemory(R.device, tex.image, tex.mem, 0) != VK_SUCCESS)
    {
        vkDestroyImage(R.device, tex.image, nullptr);
        ++texSkips["image memory allocation failed"];
        texCache[key] = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = tex.image;
    vi.viewType = is3D     ? VK_IMAGE_VIEW_TYPE_3D
                  : isCube ? VK_IMAGE_VIEW_TYPE_CUBE
                           : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vi.format = vf;
    vi.components.r = compSwizzle(gt.hostSwizzle[0]);
    vi.components.g = compSwizzle(gt.hostSwizzle[1]);
    vi.components.b = compSwizzle(gt.hostSwizzle[2]);
    vi.components.a = compSwizzle(gt.hostSwizzle[3]);
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, uint32_t(gt.levels.size()), 0, gt.layers};
    if (vkCreateImageView(R.device, &vi, nullptr, &tex.view) != VK_SUCCESS)
    {
        vkDestroyImage(R.device, tex.image, nullptr);
        vkFreeMemory(R.device, tex.mem, nullptr);
        ++texSkips["vkCreateImageView failed"];
        texCache[key] = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }

    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;
    if (!R.MakeBuffer(gt.data.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staging, stagingMem))
    {
        vkDestroyImageView(R.device, tex.view, nullptr);
        vkDestroyImage(R.device, tex.image, nullptr);
        vkFreeMemory(R.device, tex.mem, nullptr);
        ++texSkips["staging buffer allocation failed"];
        texCache[key] = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }
    {
        void *p = nullptr;
        vkMapMemory(R.device, stagingMem, 0, gt.data.size(), 0, &p);
        std::memcpy(p, gt.data.data(), gt.data.size());
        vkUnmapMemory(R.device, stagingMem);
    }
    // GEARS_DRAW_TEX_DUMP=1 writes the decoded (detiled, endian-swapped)
    // blob so the decode can be checked outside the renderer -- the only
    // way to tell "detiling is right" from "the shader is dark".
    if (lucent::config::flag("DRAW_TEX_DUMP"))
    {
        std::filesystem::create_directories("scratch/raw/textures");
        const std::string fn = std::format("scratch/raw/textures/{:08x}_{}_{}x{}x{}_{}.bin",
                                           gt.baseAddress, gt.formatName, gt.width, gt.height,
                                           gt.layers * gt.depth3D, gt.tiled ? "tiled" : "linear");
        if (FILE *f = std::fopen(fn.c_str(), "wb"))
        {
            std::fwrite(gt.data.data(), 1, gt.data.size(), f);
            std::fclose(f);
        }
    }
    stagingBufs.push_back(staging);
    stagingMems.push_back(stagingMem);
    uploadedBytes += gt.data.size();
    PendingUpload upload{tex.image, staging, uint32_t(gt.levels.size()), {}};
    upload.regions.reserve(gt.levels.size());
    for (uint32_t level = 0; level < gt.levels.size(); ++level)
    {
        const GuestTextureLevel &decoded = gt.levels[level];
        VkBufferImageCopy region{};
        region.bufferOffset = decoded.dataOffset;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, gt.layers};
        region.imageExtent = {decoded.width, decoded.height, decoded.depth};
        upload.regions.push_back(region);
    }
    uploads.push_back(std::move(upload));
    guestTextures[key] = tex;
    texCache[key] = tex.view;
    // Remember what these bytes hashed to, so a later frame can tell that the
    // guest has overwritten the texture under an unchanged fetch constant.
    // Unconditional: without this record there is nothing to compare against,
    // and the entry would then look unchanged forever.
    {
        GuestTexture header;
        if (DecodeGuestTexture(fetch6, in.guestBase, uint64_t(in.guestWindowBytes),
                               /*wantData=*/false, header) &&
            header.skipReason == nullptr && header.baseGuestExtentBytes != 0)
        {
            texContentHash[key] = HashTextureStorage(header, in);
        }
    }
    return tex.view;
}

VkSampler TextureUploader::GetSampler(const GuestSamplerState &gs)
{
    const uint64_t k = uint64_t(gs.magFilter) | (uint64_t(gs.minFilter) << 4) |
                       (uint64_t(gs.mipFilter) << 8) | (uint64_t(gs.clamp[0]) << 12) |
                       (uint64_t(gs.clamp[1]) << 16) | (uint64_t(gs.clamp[2]) << 20) |
                       (uint64_t(gs.anisoMax) << 24);
    auto it = samplerCache.find(k);
    if (it != samplerCache.end())
        return it->second;
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = gs.magFilter == 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.minFilter = gs.minFilter == 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.mipmapMode =
        gs.mipFilter == 1 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = vkAddressMode(gs.clamp[0]);
    si.addressModeV = vkAddressMode(gs.clamp[1]);
    si.addressModeW = vkAddressMode(gs.clamp[2]);
    si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    si.maxLod = VK_LOD_CLAMP_NONE;
    if (gs.anisoMax && R.hasSamplerAnisotropy)
    {
        si.anisotropyEnable = VK_TRUE;
        si.maxAnisotropy = std::min(float(gs.anisoMax), R.maxSamplerAnisotropy);
    }
    VkSampler s = VK_NULL_HANDLE;
    if (vkCreateSampler(R.device, &si, nullptr, &s) != VK_SUCCESS)
        return P.stubSampler;
    samplerCache[k] = s;
    return s;
}

VkImageView TextureUploader::ResolveTargetView(ResolveTarget &rt, uint32_t guestSwizzle)
{
    // XYZW is what an unmapped view already is, so it costs nothing and, more
    // usefully, keeps the identity case on the SAME code path as the swizzled
    // one -- a bug that only appears once a mapping is applied would otherwise
    // hide behind the common case.
    static constexpr uint32_t kIdentity = 0x688u; // X,Y,Z,W at 3 bits each
    if (guestSwizzle == kIdentity)
        return rt.view;

    auto it = rt.swizzleViews.find(guestSwizzle);
    if (it != rt.swizzleViews.end())
        return it->second;

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = rt.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY; // as the unmapped view is
    vi.format = rt.hostFormat;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    // The host image is canonical RGBA (R16G16B16A16_SFLOAT), or R for a depth
    // destination, so unlike a guest texture there is no host-format order to
    // compose with: the guest swizzle maps straight onto components. Vulkan's
    // component mapping also defines constants and missing source components,
    // so an R32 depth view can implement the title's X111 request -- it is not
    // limited to the unmapped R001 default.
    vi.components.r = compSwizzle(uint8_t((guestSwizzle >> 0) & 7));
    vi.components.g = compSwizzle(uint8_t((guestSwizzle >> 3) & 7));
    vi.components.b = compSwizzle(uint8_t((guestSwizzle >> 6) & 7));
    vi.components.a = compSwizzle(uint8_t((guestSwizzle >> 9) & 7));
    VkImageView v = VK_NULL_HANDLE;
    if (vkCreateImageView(R.device, &vi, nullptr, &v) != VK_SUCCESS)
    {
        // Falling back to the unmapped view would reintroduce the exact defect
        // this function exists to fix, and silently. Say which target.
        lucent::warn("draw",
                     "could not create a swizzled view of resolve"
                     " destination {:#x} for swizzle {:#05x}; serving the UNMAPPED view,"
                     " so this binding reads the channels as stored",
                     rt.base, guestSwizzle);
        return rt.view;
    }
    rt.swizzleViews[guestSwizzle] = v;
    return v;
}

VkImageView TextureBinder::SelectView(const uint32_t *regs, const ShaderTextureBinding &tb)
{
    const uint32_t fc = tb.fetchConstant & 31;
    const uint32_t dword1 = regs[0x4800 + fc * 6 + 1];
    const uint32_t base = (dword1 >> 12) << 12;
    // GEARS_DRAW_TEX_BINDS=<ps hash>: WHAT THIS DRAW ACTUALLY SAMPLES.
    //
    // The frame report counts bindings by KIND across the whole frame, which
    // cannot answer "this one pass renders black -- is it reading a resolve
    // target, a guest texture, or a stub, and from what address". Three
    // separate investigations have needed that and had to infer it from
    // aggregates. Each line names the fetch constant, the base the constant
    // points at, the dimension, and which of the three sources served it.
    //
    // A base of 0 is reported too: a fetch constant that names nothing is a
    // real answer and is invisible in every count.
    const bool logBinds = currentPsHash != 0 && currentPsHash == textureBindingsPsHash;
    const auto say = [&](const char *how, VkImageView v)
    {
        if (logBinds)
        {
            // exp_adjust, fetch constant word 3 bits 13:18, 6-bit SIGNED. This
            // is the sampling side of a resolve's copy_dest_exp_bias: a resolve
            // written with bias -3 stores value/8, and only a +3 here scales it
            // back. Reported with the binding because "the texture is 8x too
            // dark" and "the shader is meant to see it 8x darker" look
            // identical from the image alone.
            const uint32_t w3 = regs[0x4800 + fc * 6 + 3];
            int32_t expAdjust = int32_t((w3 >> 13) & 0x3F);
            if (expAdjust & 0x20)
                expAdjust -= 64;
            // The SWIZZLE the guest asks this binding to be read with. It is
            // here because a resolve-target binding is served by the target's
            // own host view, which carries NO component mapping -- so for those
            // this line is the only place the guest's request is visible at
            // all, and a mismatch between it and what the resolve baked into
            // the image is exactly catalog #62's channel exchange. Printed for
            // every binding, not only the ones that differ: "the swizzle is
            // identity" and "nobody looked at the swizzle" must not read alike.
            static const char kSwz[8] = {'X', 'Y', 'Z', 'W', '0', '1', '?', '?'};
            const uint32_t swizzle = (w3 >> 1) & 0xFFFu;
            const char swz[5] = {kSwz[swizzle & 7], kSwz[(swizzle >> 3) & 7],
                                 kSwz[(swizzle >> 6) & 7], kSwz[(swizzle >> 9) & 7], '\0'};
            // The ADDRESS MODE, per axis. It belongs here for the same
            // reason the swizzle does: a shader that samples outside [0,1]
            // reads the edge under CLAMP and wraps under REPEAT, and those
            // differ by the whole texture. Catalog #77's character material
            // biases its lookup coordinate by +1, so which of the two applies
            // decides whether it reads real data or the edge texel.
            static const char *kClamp[8] = {"repeat",       "mirror",
                                            "clamp-edge",   "mirror-clamp-edge",
                                            "clamp-half",   "mirror-clamp-half",
                                            "clamp-border", "?7"};
            const uint32_t d0 = regs[0x4800 + fc * 6];
            const uint32_t cx = (d0 >> 10) & 7, cy = (d0 >> 13) & 7;
            lucent::info("draw",
                         "  tex bind ps {:#x}: fc{} base {:#x} dim {}"
                         " exp_adjust {:+d} (x{}) swizzle {} ({:#05x}) clamp x={} y={}"
                         " -> {}",
                         currentPsHash, fc, base, tb.dimension, expAdjust,
                         std::ldexp(1.0, expAdjust), swz, swizzle, kClamp[cx], kClamp[cy], how);
        }
        return v;
    };
    const bool isRt = base != 0 && resolveDests.count(base) != 0;
    if (base != 0 && depthResolveDests.count(base))
        ++depthDestSamplers[{base, currentPsHash}];
    ++baseCount[base];
    if (isRt)
        ++baseRtCount[base];
    // The translated fetch reads texture_swizzled_signs to choose its sign
    // remap. The system-constant builder now populates it; this census remains
    // necessary because kSigned additionally needs a signed host image view.
    {
        const uint32_t d0 = regs[0x4800 + fc * 6];
        const uint32_t signs = (d0 >> 2) & 0xFF; // sign_x/y/z/w, 2 bits each
        if (signs != 0)
        {
            ++fetchesWithSigns[signs];
            // Resolve destinations use floating-point host images, where the
            // signed and unsigned descriptors legitimately share a view and
            // negative values are already preserved. Only a decoded guest
            // fixed-point texture needs the alternate signed-normalized view
            // that is still missing. Naming the float velocity/HDR targets as
            // broken produced a false warning on every gameplay report.
            const bool wantsSigned = ((signs >> 0) & 3) == 1 || ((signs >> 2) & 3) == 1 ||
                                     ((signs >> 4) & 3) == 1 || ((signs >> 6) & 3) == 1;
            if (wantsSigned && !isRt && base != 0)
                ++signedBases[base];
        }
    }
    // A binding that names a resolve destination of THIS frame reads that
    // destination's own host image -- the surface it was resolved from, in
    // that surface's format. Each destination has its own image, so two
    // passes sampling two different resolves no longer collide.
    if (isRt && rtLinkEnabled && tb.dimension <= 1)
    {
        auto rt = P.resolveTargets.find(base);
        if (rt != P.resolveTargets.end())
        {
            ++bindsRt;
            return say("this frame's RESOLVE TARGET",
                       TX.ResolveTargetView(rt->second, (regs[0x4800 + fc * 6 + 3] >> 1) & 0xFFFu));
        }
    }
    // The guest's own texture, decoded from this fetch constant. The stub
    // below is only reached when the decode reports a reason it cannot.
    if (texUploadEnabled)
    {
        const auto t0 = std::chrono::steady_clock::now();
        VkImageView v = TX.Upload(&regs[0x4800 + fc * 6], tb.dimension);
        msUpload += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                        .count();
        if (v != VK_NULL_HANDLE)
        {
            ++bindsGuest;
            return say("a GUEST TEXTURE", v);
        }
    }
    ++bindsStub;
    switch (tb.dimension)
    {
    case 2:
        return say("a STUB (decode refused)", P.stub3D.view);
    case 3:
        return say("a STUB (decode refused)", P.stubCube.view);
    default:
        return say("a STUB (decode refused)", P.stub2D.view);
    }
}

} // namespace gears::draw
