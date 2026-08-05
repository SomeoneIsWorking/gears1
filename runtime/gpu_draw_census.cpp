// The frame census. gpu_draw_census.h says why each counter exists; this is
// what each one prints.

#include "gpu_draw_census.h"

#include <format>

#include <lucent/log.h>

namespace gears::draw
{

void FrameCensus::NoteDraw(uint32_t surfaceBase, uint32_t colorFormat, uint32_t edramMode)
{
    SurfaceStat& st = surfaces[surfaceBase];
    ++st.draws;
    st.format = colorFormat;
    st.mode = edramMode;
    if (edramMode == 4) ++st.colorDepth;
    else if (edramMode == 5) ++st.depthOnly;
    else ++st.otherMode;
    ++edramModes[edramMode];
}

void FrameCensus::NoteDepth(uint32_t surfaceBase, uint32_t depthBase)
{
    depthBases.insert(depthBase);
    ++surfaceDepthPairs[{surfaceBase, depthBase}];
}

void FrameCensus::ReportSurfaces() const
{
    lucent::Line sl;
    sl.add("frame EDRAM surfaces: {} distinct RB_COLOR_INFO bases"
           " (draws@base:fmt colour/depth-only/other):", surfaces.size());
    for (const auto& [base, st] : surfaces)
        sl.add(" {}@{:#x}:f{} {}/{}/{}", st.draws, base, st.format,
               st.colorDepth, st.depthOnly, st.otherMode);
    sl.flush(lucent::Level::Info, "draw");
}

void FrameCensus::ReportModes() const
{
    lucent::Line ml;
    ml.add("frame EDRAM modes (RB_MODECONTROL.edram_mode):");
    for (const auto& [mode, n] : edramModes)
    {
        const char* name =
            mode == 0 ? "no_op" : mode == 4 ? "color_depth" :
            mode == 5 ? "depth_only" : mode == 6 ? "COPY(resolve)" : "?";
        ml.add(" {}={}", name, n);
    }
    ml.add("; {} draws issued with no fragment stage", drawsNoPixelShader);
    ml.flush(lucent::Level::Info, "draw");
}

void FrameCensus::ReportDepthPairs() const
{
    lucent::Line sd;
    sd.add("frame (colour surface, depth base) pairs:");
    for (const auto& [k, n] : surfaceDepthPairs)
        sd.add(" {:#x}/{:#x}x{}", k.first, k.second, n);
    sd.flush(lucent::Level::Info, "draw");
}

void FrameCensus::ReportReach(uint64_t mirrorBytes) const
{
    const std::string reach = std::format(
        "frame geometry reach: {} draws fetch vertices inside the {:#x}-byte"
        " SSBO mirror, {} draws fetch PAST it (those read zero and collapse);"
        " highest vertex-buffer end seen {:#x}",
        vfDrawsInMirror, mirrorBytes, vfDrawsPastMirror, vfHighestByte);
    if (vfDrawsPastMirror != 0)
        lucent::warn("draw", "{} -- THE FRAME IS MISSING WORLD GEOMETRY", reach);
    else
        lucent::info("draw", "{}", reach);
}

void FrameCensus::ReportViewports() const
{
    for (const auto& [what, n] : viewportCensus)
        lucent::info("draw", "  guest viewport {} x{} draws", what, n);
}

void FrameCensus::ReportSkips(size_t) const
{
    if (!skipped)
        return;
    for (const auto& [code, n] : skipReasons)
    {
        const char* why =
            code == 1 ? "no snapshot/ucode" :
            code == 2 ? "shader translate failed" :
            code == 3 ? "pipeline create failed" :
            code == 4 ? "UBO alloc failed" :
            code == 5 ? "index buffer failed" :
            code == 6 ? "descriptor alloc failed" :
            code == 7 ? "quad list with fewer than 4 vertices" :
            code == 8 ? "no host target for the draw's EDRAM surface format"
                      : "unknown";
        lucent::warn("draw", "  skipped {}x: {}", n, why);
    }
}

} // namespace gears::draw
