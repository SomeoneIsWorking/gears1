#!/usr/bin/env python3
"""Convert one of our frame captures (.gfr) into a Xenia GPU trace (.xtr).

WHY. Xenia renders this title correctly, and it ships a HEADLESS renderer --
`xenia-gpu-vulkan-trace-dump` -- that turns a trace into an image with no
window, no controller and no playthrough. If our capture can be expressed as a
trace, every open question of the form "what should this frame look like?"
becomes one command instead of a play session. See catalog #7 and #62.

WHAT THIS CAN AND CANNOT SETTLE -- read before trusting a run.

A .gfr stores per-draw REGISTER SNAPSHOTS, not the PM4 packet stream the guest
actually sent. This SYNTHESISES a command stream by restoring the registers
changed since the preceding draw, then emitting only the DRAW_INDX packet.

  * Sound for SHADING, RESOLVE and FORMAT questions -- everything downstream of
    the registers. Both renderers are driven from the same register values, so a
    difference in the output is a difference in how they were interpreted.
  * WORTHLESS for command-stream DECODE questions. Where we misread the guest's
    packets, this bakes our misreading into the oracle's input and it will agree
    with us precisely where we are wrong.

Say which kind of question is being asked before quoting a result.

    tools/gfr_to_xtr.py <capture.gfr> <out.xtr> [--draws N] [--regs delta|full]
                        [--present guest|frame]
                        [--checkpoint-copy RESOLVE:PREFIX]
                        [--probe-copy RESOLVE:PREFIX [--probe-msaa 1x|2x|4x]
                         [--probe-global-y]]

--present guest (default) swaps the buffer the guest's VdSwap named, which is
the faithful thing and, for a ONE-FRAME capture, is the buffer the PREVIOUS
frame filled -- so the oracle renders it black and is right to. --present frame
swaps the address this frame's own last colour resolve wrote, which is what
makes the oracle show the output of the draws in the trace. Use `frame` to
compare renderers and `guest` to ask about the present path.

--checkpoint-copy R:N executes the first N capture draw items, then appends the
capture's real resolve R. It exposes exact intermediate EDRAM through Xenia's
normal copy path without pretending a truncated frame is a complete frame. The
faithful default is --regs delta; --regs full replays unchanged callbacks too.

--probe-copy R:N also executes N draw items and appends resolve R, but may reuse
a resolve recorded before N. This reads the live EDRAM state at N through a
known copy command; it is explicitly a probe, not a claim about the original
ordering of resolve R. --probe-msaa changes only RB_SURFACE_INFO's sample count
on that appended copy, so the probe can read the same EDRAM sample view as the
pass being diagnosed. Without it, the recorded resolve's original sample count
is retained and the output is evidence only about that original view.

--probe-global-y converts a vertically tiled resolve command back to its global
Y coordinates. It removes the negative window offset from the resolve vertices
and scissor, rebases the destination to the start of the full-height texture,
and expands its declared height. Without it, a lower-tile command reads local
EDRAM rows starting at zero, not the global lower band of a full-height surface.
"""
import struct
import sys
from pathlib import Path

from gfr_io import Capture, K_REGISTERS, TraceWriter
from gfr_probe import (
    REG_PA_SC_WINDOW_OFFSET,
    REG_PA_SC_WINDOW_SCISSOR_TL,
    REG_PA_SU_SC_MODE_CNTL,
    REG_RB_SURFACE_INFO,
    globalize_probe_y,
)
from gfr_trace_plan import (
    changed_runs,
    parse_checkpoint,
    parse_probe,
    selftest_cases,
)

# --- Xenos (extern/xenia/src/xenia/gpu/xenos.h, registers.h) ----------------
PM4_DRAW_INDX = 0x22
PM4_IM_LOAD_IMMEDIATE = 0x2B
# The Xenia-private packet its own kernel's VdSwap posts to trigger a swap
# (xenos.h PM4_XE_SWAP, xboxkrnl_video.cc VdSwap_entry). Nothing else makes
# Xenia produce an image: the trace format's kEvent/kSwap marker is only a
# playback break hint (trace_player.cc), so a trace carrying only that renders
# every draw and then writes no file.
PM4_XE_SWAP = 0x64
SWAP_SIGNATURE = 0x53574150            # make_fourcc("SWAP")
# XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 (register_table.inc). Xenia's swap takes
# the front buffer from FETCH CONSTANT 0, not from the address in the packet
# (vulkan_texture_cache.cc RequestSwapTexture), exactly as the hardware does.
REG_FETCH_CONSTANT_00 = 0x4800
# The DC_LUT gamma-ramp window (register_table.inc 0x1921..0x1934). Excluded
# from the per-draw register restore -- see the restore loop for why.
REG_DC_LUT_FIRST = 0x1921
REG_DC_LUT_LAST = 0x1934
# The console maps physical RAM through several virtual aliases; the fetch
# constant names one of them. Xenia's kernel translates to physical before
# posting, because the GPU works in physical addresses -- and our capture's
# memory blocks are stored at physical offsets too, so the same translation is
# what makes the address land on the captured bytes.
GUEST_PHYSICAL_MASK = 0x1FFFFFFF

# Resolve ("copy") state, read exactly where runtime/gpu_draw.cpp reads it, so
# the two agree by construction rather than by transcription:
#   RB_MODECONTROL.edram_mode == 6      a draw that is a resolve, not geometry
#   RB_COPY_CONTROL.copy_src_select     >= 4 means the source is depth
#   RB_COPY_DEST_BASE                   where the resolved pixels land in memory
#   RB_COPY_DEST_PITCH                  pitch in the low 14 bits, height above 16
REG_RB_MODECONTROL = 0x2208
REG_RB_COPY_CONTROL = 0x2318
REG_RB_COPY_DEST_BASE = 0x2319
REG_RB_COPY_DEST_PITCH = 0x231A
REG_RB_COPY_DEST_INFO = 0x231B
SHADER_TYPE_VERTEX = 0
SHADER_TYPE_PIXEL = 1
# A PM4 count field is 14 bits, so a packet cannot carry more than this many
# dwords. A shader bigger than that needs the pointer-based PM4_IM_LOAD instead.
PM4_MAX_COUNT = 0x3FFF
SRC_SELECT_DMA = 0
SRC_SELECT_AUTO_INDEX = 2
REG_VGT_DMA_BASE = 0x21FA
REG_VGT_DMA_SIZE = 0x21FB
# xenos::Endian. Our renderer reads indices with a full byte swap at both
# widths, so these are the modes that reproduce ITS interpretation.
ENDIAN_8IN16 = 1
ENDIAN_8IN32 = 2
REG_COUNT = 0x8000
# Xenia's register file is SMALLER than the one we capture (register_file.h:
# kRegisterCount). RestoreRegisters rejects the whole command when it overruns
# -- with a warning, then renders anyway from an unset register file, which
# looks like a backend failure rather than a format mismatch. Send the prefix
# Xenia can hold; every register either renderer reads lives well below it.
XENIA_REG_COUNT = 0x5003


def final_colour_resolve(cap):
    """Where this frame's last COLOUR resolve puts its pixels, or None.

    This is the frame's own finished composite landing in memory -- the thing a
    later VdSwap would present. It is read from the frame's own registers, not
    chosen: `--present frame` needs an address, and the only honest source for
    one is the resolve the frame actually performed.
    """
    for d in reversed(cap.draws):
        regs = d["regs"]
        if len(regs) <= REG_RB_COPY_DEST_INFO:
            continue
        if (regs[REG_RB_MODECONTROL] & 0x7) != 6:      # not a resolve
            continue
        if (regs[REG_RB_COPY_CONTROL] & 0x7) >= 4:     # depth, not colour
            continue
        dest = regs[REG_RB_COPY_DEST_BASE] & ~0xFFF
        if not dest:
            continue
        return dict(base=dest,
                    pitch=regs[REG_RB_COPY_DEST_PITCH] & 0x3FFF,
                    height=(regs[REG_RB_COPY_DEST_PITCH] >> 16) & 0x3FFF,
                    info=regs[REG_RB_COPY_DEST_INFO])
    return None


def all_resolves(cap):
    """Every resolve the frame performs, in draw order, with its draw index.

    Numbered the same way the fork's `IssueCopy` log numbers them, so a resolve
    named here and a resolve named in an oracle log are the same one.
    """
    out = []
    for i, d in enumerate(cap.draws):
        regs = d["regs"]
        if len(regs) <= REG_RB_COPY_DEST_INFO:
            continue
        if (regs[REG_RB_MODECONTROL] & 0x7) != 6:
            continue
        out.append(dict(index=len(out), draw=i,
                        depth=(regs[REG_RB_COPY_CONTROL] & 0x7) >= 4,
                        base=regs[REG_RB_COPY_DEST_BASE] & ~0xFFF,
                        pitch=regs[REG_RB_COPY_DEST_PITCH] & 0x3FFF,
                        height=(regs[REG_RB_COPY_DEST_PITCH] >> 16) & 0x3FFF,
                        info=regs[REG_RB_COPY_DEST_INFO]))
    return out


def fetch_from_resolve(resolve):
    """Describe a resolve destination as a texture fetch constant.

    Every field comes from the resolve's OWN registers, which is what makes this
    usable on a capture that predates the front-buffer fetch constant -- and
    the resolve and texture descriptors use the same format numbering.

    The self-test uses a synthetic, field-isolating input and independently
    calculated dwords. Real capture bytes remain runtime inputs rather than
    tracked test fixtures.
    """
    info = resolve["info"]
    pitch, height = resolve["pitch"], resolve["height"]
    fmt = (info >> 7) & 0x3F                   # ColorFormat == TextureFormat
    endian = info & 0x7
    # copy_dest_swap says the destination is written red/blue swapped, which is
    # the same statement the fetch constant makes with its swizzle: (Z,Y,X,1)
    # rather than (X,Y,Z,1).
    swizzle = 0xA0A if (info >> 24) & 1 else 0xA88
    dword_0 = (1 << 31) | ((pitch // 32) << 22) | 2      # tiled, pitch, kTexture
    dword_1 = (fmt & 0x3F) | ((endian & 3) << 6) | ((resolve["base"] >> 12) << 12)
    dword_2 = ((pitch and (min(pitch, 0x2000) - 1)) & 0x1FFF) | \
              (((height - 1) & 0x1FFF) << 13)
    dword_3 = (swizzle & 0xFFF) << 1
    dword_5 = 1 << 9                                     # k2DOrStacked
    return [dword_0, dword_1, dword_2, dword_3, 0, dword_5]


def front_buffer_extent(cap):
    """The guest bytes the swap will read as the presented image, [lo, hi).

    Sized from the fetch constant the guest itself posted, not from the frame's
    1280x720: a tiled surface is stored at its row PITCH and padded to whole
    32-pixel tiles, so the buffer is larger than the visible image and the tail
    is exactly the part that looks free. Empty when the capture cannot say,
    which excludes nothing -- the caller is then no worse off than before.
    """
    spans = []
    if cap.front_fetch and any(cap.front_fetch):
        spans.append((((cap.front_fetch[1] >> 12) << 12) & GUEST_PHYSICAL_MASK,
                      ((cap.front_fetch[0] >> 22) & 0x1FF) * 32,
                      ((cap.front_fetch[2] >> 13) & 0x1FFF) + 1))
    # The frame's own resolve destination counts too, and on a capture that
    # predates the fetch constant it is the ONLY thing that does. It was not
    # excluded once, and the packets went into the buffer the oracle presents --
    # a v2 capture put them at 0x320000 inside a destination based at 0x311000.
    resolve = final_colour_resolve(cap)
    if resolve:
        spans.append((resolve["base"] & GUEST_PHYSICAL_MASK, resolve["pitch"],
                      resolve["height"]))
    if not spans:
        return (0, 0)
    # Every format either buffer can take here is 32 bits per pixel, and a tiled
    # surface is padded to whole 32-pixel tile rows.
    lo = min(base for base, _, _ in spans)
    hi = max(base + (pitch or 1280) * ((height + 31) & ~31) * 4
             for base, pitch, height in spans)
    return (lo, hi)


def pick_packet_scratch(cap):
    """A guest page to place synthesised packets in, chosen per capture.

    It must not overlap any page the capture occupies: a packet written over the
    frame's own vertices would corrupt them, and the render would look plausible
    and be wrong. A hardcoded address is not safe to assume -- the first one
    tried (0x01000000) collided with real data in courtyard.gfr -- so the free
    page is FOUND, and if there is none this refuses rather than picking one
    anyway.

    A page being all-zero in the capture is NOT enough on its own. The front
    buffer is several megabytes and much of it can be zero at capture time --
    0x320000 sat inside a front buffer based at 0x311000 -- and packets written
    there are read back by the swap as pixels, so the oracle's image would carry
    a block of garbage that looks like a rendering defect. Its whole extent is
    therefore excluded, zero or not.
    """
    occupied = {base for base, _ in cap.blocks}
    lo, hi = front_buffer_extent(cap)
    for guest in range(0, min(cap.window, cap.mirror), cap.block_size):
        if guest in occupied:
            continue
        if guest < hi and guest + cap.block_size > lo:
            continue
        return guest
    raise SystemExit(
        "REFUSING: every page in this capture's guest window is occupied, so "
        "there is nowhere to put a packet that would not overwrite frame data.")

def swap_packets(fetch, front_buffer):
    """The two packets Xenia's own VdSwap posts, in its order.

    Mirrored from xboxkrnl_video.cc VdSwap_entry, which is the contract the GPU
    side reads: a TYPE0 write of the six-dword front-buffer fetch constant into
    fetch slot 0, then PM4_XE_SWAP carrying the signature, the front buffer's
    PHYSICAL address, and its size.

    The width and height are taken from the fetch constant's own size_2d rather
    than carried separately: Xenia's kernel asserts the two are equal
    (`*width == 1 + gpu_fetch.size_2d.width`), so reading them from the fetch is
    the same number by the hardware's own definition -- and its Vulkan backend
    ignores these packet fields anyway, sizing the swap image from the texture.

    Returns (type0_dwords, type3_dwords, width, height, physical_address).
    """
    fetch = list(fetch)
    virtual = (fetch[1] >> 12) << 12       # base_address is dword_1 bits 12..31
    physical = virtual & GUEST_PHYSICAL_MASK
    fetch[1] = (fetch[1] & 0xFFF) | ((physical >> 12) << 12)
    # size_2d: width-1 in bits 0..12 and height-1 in 13..25 of dword_2.
    width = (fetch[2] & 0x1FFF) + 1
    height = ((fetch[2] >> 13) & 0x1FFF) + 1

    type0 = [(0 << 30) | ((6 - 1) << 16) | REG_FETCH_CONSTANT_00] + fetch
    type3 = [0xC0000000 | ((4 - 1) << 16) | (PM4_XE_SWAP << 8),
             SWAP_SIGNATURE, physical, width, height]
    return type0, type3, width, height, physical


def im_load_packet(shader_type, ucode):
    """PM4_IM_LOAD_IMMEDIATE: bind a shader by embedding its microcode.

    Xenia sets active_vertex_shader_/active_pixel_shader_ ONLY from an IM_LOAD
    (pm4_command_processor_implement.h). No register assignment binds a shader,
    so a trace built purely from register snapshots draws with none and every
    draw fails in the backend -- which is exactly what happened before this.

    Returns (header_dwords, payload_bytes); the payload stays in guest order.
    """
    size_dwords = len(ucode) // 4
    body = [shader_type, size_dwords]             # start == 0, so no high half
    count = len(body) + size_dwords
    if count > PM4_MAX_COUNT:
        return None                               # caller reports it
    header = 0xC0000000 | ((count - 1) << 16) | (PM4_IM_LOAD_IMMEDIATE << 8)
    return [header] + body, ucode[:size_dwords * 4]


def draw_packet(draw):
    """Build one PM4_DRAW_INDX, per ExecutePacketType3Draw.

    VGT_DMA_SIZE carries the index buffer's length in its low 24 bits
    (`num_words`) and its endian swap mode in the top two. It was previously
    copied from the draw's register snapshot, on the belief that the capture did
    not record the swap mode but the register did. THE REGISTER IS ZERO: our
    command processor takes the index parameters from the DRAW_INDX packet and
    never writes 0x21FB, so all 659 indexed draws of the courtyard frame carried
    "this index buffer contains 0 indices". Xenia obeyed exactly that -- 665
    "index buffer only containing 0" warnings and a black frame -- and the frame
    looked like a shading failure rather than a malformed trace.

    So the length comes from the draw's own index count, which the capture does
    store. The swap mode is set to the FULL swap our renderer performs when it
    reads indices (gpu_draw_indices.cpp: bswap32 for 32-bit, byte swap for
    16-bit), i.e. k8in32 and k8in16 respectively.

    That last part is a DECODE assumption, and it is the kind this tool's header
    warns about: if our reading of index endianness is wrong, the oracle is now
    wrong the same way and will agree with us. It is not evidence about index
    endianness. It is only sound for the shading, resolve and format questions.
    """
    src = SRC_SELECT_DMA if draw["indexed"] else SRC_SELECT_AUTO_INDEX
    initiator = ((draw["prim"] & 0x3F) | (src << 6) |
                 ((1 if draw["index32"] else 0) << 11) |
                 ((draw["index_count"] & 0xFFFF) << 16))
    body = [0, initiator]                         # viz query token, initiator
    if draw["indexed"]:
        swap_mode = ENDIAN_8IN32 if draw["index32"] else ENDIAN_8IN16
        dma_size = (draw["index_count"] & 0xFFFFFF) | (swap_mode << 30)
        body += [draw["index_base"], dma_size]
    header = 0xC0000000 | ((len(body) - 1) << 16) | (PM4_DRAW_INDX << 8)
    return [header] + body


def emit_swap(w, cap, scratch, present="guest"):
    """Emit the swap, or say exactly why the trace will produce no image.

    The negative is written out deliberately. Without a swap the dump renders
    every draw, exits 1 and writes NOTHING -- which is indistinguishable from a
    trace whose draws all failed, and cost a session once already. So a trace
    that cannot swap says so here, at build time, rather than being discovered
    as a missing file an hour later.
    """
    if present == "guest":
        if cap.front_fetch is None:
            return (f"NO SWAP: capture is v{cap.version}, which predates the "
                    f"front buffer's fetch constant, and --present guest has "
                    f"nothing else to describe it. Re-capture, or use "
                    f"--present frame, which describes the buffer from the "
                    f"frame's own resolve registers.")
        if not any(cap.front_fetch):
            return ("NO SWAP: the capture's front-buffer fetch constant is all "
                    "zeroes -- the guest passed none, or the swap packet "
                    "predates it. No image will be written.")
    fetch = list(cap.front_fetch) if cap.front_fetch else [0] * 6
    note_prefix = ""
    if present.startswith("resolve:"):
        # WHY AN INTERMEDIATE RESOLVE CAN BE PRESENTED AT ALL, and why this is
        # not cherry-picking. Measured in catalog #79: in trace playback the
        # frame's EARLY resolves land in shared memory and its LATE ones write
        # zeros over what is already there, including the final composite the
        # swap reads. So --present frame shows an emptied buffer even though the
        # scene did render. Naming a resolve by its own index -- the same index
        # the fork's IssueCopy log prints -- shows the buffer the frame actually
        # filled. The caller MUST truncate playback to that resolve's draw
        # (convert() does it automatically) or a later resolve erases it again.
        resolves = all_resolves(cap)
        try:
            n = int(present.split(":", 1)[1], 0)
        except ValueError:
            return f"NO SWAP: --present {present} is not a resolve index."
        if not 0 <= n < len(resolves):
            listing = ", ".join(f"{r['index']}:{r['base']:#x}"
                                f"{'(depth)' if r['depth'] else ''}"
                                for r in resolves)
            return (f"NO SWAP: this frame performs {len(resolves)} resolves, so "
                    f"resolve {n} does not exist. They are: "
                    f"{listing or '(none at all)'}")
        resolve = resolves[n]
        if resolve["depth"]:
            return (f"NO SWAP: resolve {n} at {resolve['base']:#x} is a DEPTH "
                    f"resolve. Presenting it would show depth bits decoded as "
                    f"colour, which looks like an image and is not one.")
        if not resolve["base"]:
            return f"NO SWAP: resolve {n} names destination 0."
        fetch = fetch_from_resolve(resolve)
        note_prefix = (f"presenting RESOLVE {n} of {len(resolves)} "
                       f"({resolve['base']:#x}, dest pitch {resolve['pitch']}, "
                       f"written by draw {resolve['draw']}) rather than the "
                       f"frame's last -- see --present. ")
    if present == "frame":
        # WHY THIS EXISTS, because it is a deliberate departure from what the
        # guest said and must not be used without knowing that.
        #
        # VdSwap names the buffer the PREVIOUS frame resolved into -- that is
        # what double buffering means. A capture holds ONE frame, and our
        # runtime resolves on the host rather than into guest memory, so the
        # named front buffer arrives at the oracle empty: measured, 34 non-zero
        # bytes in 3.6 MiB, and the oracle correctly renders black. Presenting
        # it answers "what did the previous frame leave in memory", which is not
        # a question anyone is asking.
        #
        # So this points the swap at the address THIS frame's own last colour
        # resolve wrote -- read from the frame's registers, not chosen -- which
        # makes the oracle show the output of the draws it just executed. That
        # is the comparison: same registers, same draws, whose pixels differ.
        # It is NOT a claim about the present path, and a present-path question
        # must use --present guest.
        resolve = final_colour_resolve(cap)
        if resolve is None:
            return ("NO SWAP: --present frame, but this frame performs no "
                    "colour resolve, so it never puts a composite in memory "
                    "for a swap to name. No image will be written.")
        # The descriptor is BUILT from the resolve's own registers rather than
        # borrowed from the front buffer, so this works on captures that predate
        # the fetch constant -- and so no field is taken on trust.
        fetch = fetch_from_resolve(resolve)
        note_prefix = (f"presenting THIS frame's final colour resolve "
                       f"({resolve['base']:#x}, dest pitch {resolve['pitch']}) "
                       f"instead of the front buffer the guest named "
                       f"({cap.front_buffer:#x}) -- see --present. ")
    type0, type3, width, height, physical = swap_packets(fetch,
                                                         cap.front_buffer)
    # The fetch constant and the separately-recorded front buffer address are two
    # independent statements by the guest about the same buffer. Xenia's kernel
    # asserts they agree; if they do not here, one of them is being read wrong,
    # and rendering anyway would present some other surface convincingly.
    stated = (cap.front_fetch[1] >> 12) << 12 if cap.front_fetch else 0
    if present == "guest" and cap.front_buffer and \
            (cap.front_buffer & GUEST_PHYSICAL_MASK) != \
            (stated & GUEST_PHYSICAL_MASK):
        raise SystemExit(
            f"REFUSING: the capture's front buffer address {cap.front_buffer:#x} "
            f"and its fetch constant's base {stated:#x} name different buffers. "
            f"One of the two is being read wrong; a trace built from either "
            f"would present a surface the guest did not name.")
    w.packet(scratch, type0)
    w.packet(scratch, type3)
    w.swap()
    return (f"swap: {note_prefix}buffer {physical:#x} ({width}x{height}), "
            f"fetch constant 0 posted as the guest's VdSwap does")


REGISTER_RESTORE_RANGES = ((0, REG_DC_LUT_FIRST),
                           (REG_DC_LUT_LAST + 1, XENIA_REG_COUNT))

def convert(src: Path, dst: Path, max_draws=None, present="guest", regs="delta",
            checkpoint=None, probe=None, probe_msaa=None,
            probe_global_y=False):
    cap = Capture(src)
    w = TraceWriter()
    w.header()
    # Guest memory first: vertices, indices, textures and the shader microcode
    # the registers point at all live here.
    for base, data in cap.blocks:
        w.memory_read(base, data)
    scratch = pick_packet_scratch(cap)
    # --present resolve:N is useless without stopping there: the measured
    # behaviour is that later resolves zero the buffer again. Truncating is done
    # HERE rather than left to the caller because a caller who forgets gets a
    # black image and no hint why -- which is exactly the failure this whole
    # option exists to escape.
    if present.startswith("resolve:"):
        rs = all_resolves(cap)
        try:
            n = int(present.split(":", 1)[1], 0)
        except ValueError:
            n = -1
        if 0 <= n < len(rs):
            stop = rs[n]["draw"] + 1
            if max_draws is None or max_draws > stop:
                if max_draws is not None:
                    print(f"   NOTE: --draws {max_draws} overridden to {stop} "
                          f"so playback ends at resolve {n}")
                max_draws = stop
    checkpoint_plan = None
    copy_plan_kind = None
    if checkpoint is not None:
        try:
            checkpoint_plan = parse_checkpoint(
                checkpoint, all_resolves(cap), len(cap.draws))
        except ValueError as error:
            print(f"REFUSING: {error}. Nothing was converted.")
            return 2
        draws = [cap.draws[index] for index in checkpoint_plan.draw_indices]
        present = checkpoint_plan.present
        copy_plan_kind = "CHECKPOINT"
    elif probe is not None:
        try:
            checkpoint_plan = parse_probe(
                probe, all_resolves(cap), len(cap.draws))
        except ValueError as error:
            print(f"REFUSING: {error}. Nothing was converted.")
            return 2
        draws = [cap.draws[index] for index in checkpoint_plan.draw_indices]
        present = checkpoint_plan.present
        copy_plan_kind = "PROBE"
    else:
        draws = cap.draws if max_draws is None else cap.draws[:max_draws]
    emitted = skipped_no_regs = 0
    too_big = set()
    last_vs = last_ps = None
    prev_regs = None
    reg_writes = reg_changed = reg_commands = 0
    draws_with_no_change = 0
    for draw_position, d in enumerate(draws):
        if len(d["regs"]) < REG_COUNT:
            # A draw whose snapshot is short cannot restore state; dropping it
            # silently would make the oracle render a DIFFERENT frame and look
            # authoritative doing it.
            skipped_no_regs += 1
            continue
        # execute_callbacks=TRUE, and the gamma-ramp window EXCLUDED.
        #
        # With callbacks false CommandProcessor::RestoreRegisters does a raw
        # memcpy into the register file and NEVER calls WriteRegister, which
        # is what drives every register side effect Xenia has. The registers
        # arrive, the draws rasterise, the resolve reports a written length, and
        # NOTHING lands in shared memory. That is catalog #79's black dump, and
        # it is why traces this tool produced have never rendered.
        #
        # But the callbacks cannot be run over the DC_LUT window. Writing
        # DC_LUT_30_COLOR auto-increments DC_LUT_RW_INDEX (catalog #78), so
        # restoring those registers once per draw pushes 844 bogus entries into
        # the gamma ramp -- measured: the frame comes out uniformly 1.0 on every
        # channel instead of black. They are pure display state and no draw or
        # resolve depends on them, so the restore skips them and the ramp keeps
        # whatever the capture's own kGammaRamp command set.
        #
        # --regs delta sends only the registers that CHANGED since the previous
        # draw. A .gfr stores per-draw snapshots rather than the PM4 stream, so
        # Xenia's render target cache tracks EDRAM ownership INCREMENTALLY on
        # WriteRegister callbacks for the registers that actually move. Re-
        # writing all 0x5003 registers before every draw fires those callbacks
        # ~20k times per draw with unchanged values, which is not a state the
        # cache was ever designed to see. Delta emission reproduces what the
        # guest's own command stream looks like. Catalog #79.
        cur = d["regs"]
        if (probe_msaa is not None and copy_plan_kind == "PROBE" and
                draw_position == checkpoint_plan.prefix_count):
            # A live EDRAM probe must be explicit about the sample view it is
            # reading. Reusing a 2X copy after a 1X depth restore aliases the
            # same EDRAM tiles through a different pixel layout and produces a
            # real but irrelevant image. Preserve pitch and HiZ pitch; replace
            # only RB_SURFACE_INFO.msaa_samples (bits 16:17).
            cur = list(cur)
            cur[REG_RB_SURFACE_INFO] = (
                (cur[REG_RB_SURFACE_INFO] & ~(3 << 16)) |
                (probe_msaa << 16))
        if (probe_global_y and copy_plan_kind == "PROBE" and
                draw_position == checkpoint_plan.prefix_count):
            try:
                cur = globalize_probe_y(cur)
            except ValueError as error:
                print(f"REFUSING: {error}. Nothing was converted.")
                return 2
        for lo, hi in REGISTER_RESTORE_RANGES:
            if regs == "delta":
                runs, nchanged = changed_runs(prev_regs, cur, lo, hi)
            else:
                runs, nchanged = [(lo, hi)], hi - lo
            for a, b in runs:
                callbacks = (checkpoint_plan is None or
                             checkpoint_plan.execute_register_callbacks(draw_position))
                w.registers(a, cur[a:b], execute_callbacks=callbacks)
                reg_writes += b - a
                reg_commands += 1
            reg_changed += nchanged
        if regs == "delta" and prev_regs is not None and cur == prev_regs:
            draws_with_no_change += 1
        prev_regs = cur
        # Bind this draw's shaders. Re-emitted only when they CHANGE: the bind
        # is sticky in Xenia, and re-sending every shader for all 744 draws
        # doubled the trace for no effect.
        for slot, kind, blob_i in ((0, SHADER_TYPE_VERTEX, d["vs"]),
                                   (1, SHADER_TYPE_PIXEL, d["ps"])):
            prev = last_vs if slot == 0 else last_ps
            if blob_i == prev or blob_i >= len(cap.blobs):
                continue
            built = im_load_packet(kind, cap.blobs[blob_i])
            if built is None:
                too_big.add(blob_i)
                continue
            dwords, payload = built
            w.packet_with_payload(scratch, dwords, payload)
            if slot == 0:
                last_vs = blob_i
            else:
                last_ps = blob_i
        w.packet(scratch, draw_packet(d))
        emitted += 1
    swap_note = emit_swap(w, cap, scratch, present)
    dst.write_bytes(bytes(w.buf))
    print(f"{src.name} -> {dst.name}")
    print(f"   capture: v{cap.version} {cap.width}x{cap.height}, "
          f"{len(cap.draws)} draws, {len(cap.blocks)} guest pages "
          f"({sum(len(b) for _, b in cap.blocks) / (1 << 20):.1f} MiB)")
    print(f"   packets at guest {scratch:#x} (a page this capture leaves empty)")
    print(f"   trace:   {emitted} draws emitted, {len(dst.read_bytes()) / (1 << 20):.1f} MiB")
    # ALWAYS printed, in both modes, including when nothing changed. "delta
    # emitted nothing" and "delta was never asked for" are different failures
    # and a silent tool cannot tell them apart.
    full_would_be = emitted * sum(hi - lo for lo, hi in REGISTER_RESTORE_RANGES)
    print(f"   regs:    --regs {regs}: {reg_writes} register writes in "
          f"{reg_commands} commands ({reg_changed} actually differed; a full "
          f"restore would send {full_would_be})")
    if regs == "delta":
        print(f"            {draws_with_no_change} of {emitted} draws had a "
              f"register snapshot identical to the previous draw")
        if reg_changed == 0 and emitted > 1:
            print(f"            WARNING: NOTHING changed across {emitted} draws. "
                  f"Either the capture stores one snapshot for the whole frame "
                  f"or the .gfr reader is handing back the same list each time; "
                  f"this trace does NOT carry per-draw state.")
    print(f"   {swap_note}")
    if too_big:
        print(f"   WARNING: {len(too_big)} shaders exceed a PM4 packet's 14-bit "
              f"count and were NOT bound, so some draws used whatever shader was "
              f"bound before them. Those draws are WRONG, not missing.")
    if skipped_no_regs:
        print(f"   WARNING: {skipped_no_regs} draws had no full register snapshot "
              f"and were DROPPED -- the trace is not the whole frame")
    if max_draws is not None and len(cap.draws) > max_draws:
        print(f"   NOTE: --draws {max_draws} truncated {len(cap.draws) - max_draws} "
              f"draws; this trace is a PREFIX of the frame, not the frame")
    if checkpoint_plan is not None:
        ordering = ("no later draw was executed" if copy_plan_kind == "CHECKPOINT"
                    else "the resolve state is reused only to probe live EDRAM")
        print(f"   {copy_plan_kind}: {checkpoint_plan.prefix_count} capture draw(s), then "
              f"original resolve {checkpoint_plan.resolve_index} from draw "
              f"{checkpoint_plan.resolve_draw}; {ordering}")
        if probe_msaa is not None:
            print(f"          appended copy reads the live EDRAM through the "
                  f"{(1, 2, 4)[probe_msaa]}X sample view")
        if probe_global_y:
            print("          appended copy reads the recorded lower tile at its"
                  " global Y coordinates")
    return 0


def selftest():
    """Prove the bit-packing, not just that it runs.

    Every constant here is transcribed from Xenia's headers, and a transcription
    error produces a trace that parses and renders the wrong thing -- the exact
    failure this whole tool exists to avoid. So the packing is checked against
    values worked out by hand.
    """
    failures = []

    def check(what, got, want):
        ok = got == want
        fmt = (lambda v: f"{v:#x}") if isinstance(got, int) and isinstance(want, int) else repr
        print(f"   {'ok  ' if ok else 'FAIL'}  {what}: got {fmt(got)}, want {fmt(want)}")
        if not ok:
            failures.append(what)

    # Indexed, 16-bit, triangle list (prim 4), 804 indices.
    d = dict(regs=[0] * REG_COUNT, prim=4, index_count=804, indexed=True,
             index32=False, index_base=0xDEAD00)
    pkt = draw_packet(d)
    # header: type3 | (count-1)<<16 | opcode<<8, body is 4 dwords
    check("indexed packet header", pkt[0], 0xC0000000 | (3 << 16) | (0x22 << 8))
    check("indexed initiator", pkt[2], 4 | (0 << 6) | (0 << 11) | (804 << 16))
    check("indexed dma base", pkt[3], 0xDEAD00)
    # num_words is the INDEX COUNT, not whatever the register held: it held zero,
    # and that zero is what made the oracle render an empty frame.
    check("indexed dma size carries the count and the swap mode", pkt[4],
          804 | (ENDIAN_8IN16 << 30))
    check("indexed packet length", len(pkt), 5)
    pkt32 = draw_packet(dict(d, index32=True, index_count=96))
    check("32-bit indices use k8in32", pkt32[4], 96 | (ENDIAN_8IN32 << 30))

    # Auto-index (non-indexed) draws carry no DMA registers at all.
    d2 = dict(d, indexed=False, index_count=3, index32=True)
    pkt2 = draw_packet(d2)
    check("auto packet header", pkt2[0], 0xC0000000 | (1 << 16) | (0x22 << 8))
    check("auto initiator", pkt2[2],
          4 | (SRC_SELECT_AUTO_INDEX << 6) | (1 << 11) | (3 << 16))
    check("auto packet length", len(pkt2), 3)

    # A lower EDRAM tile stores global rows 512..719 at local rows 0..207.
    # Globalizing its copy must undo both coordinate shifts and rebase the
    # contiguous 1280-wide, 32bpp destination from row 512 back to row zero.
    lower = [0] * REG_COUNT
    lower[REG_RB_COPY_CONTROL] = 4
    lower[REG_PA_SC_WINDOW_OFFSET] = (-512 & 0x7FFF) << 16
    lower[REG_PA_SC_WINDOW_SCISSOR_TL] = 512 << 16
    lower[REG_PA_SU_SC_MODE_CNTL] = 1 << 16
    lower[REG_RB_COPY_DEST_BASE] = 0x0BCD0000
    lower[REG_RB_COPY_DEST_PITCH] = 1280 | (208 << 16)
    lower_global = globalize_probe_y(lower)
    check("global-Y probe rebases destination", lower_global[REG_RB_COPY_DEST_BASE],
          0x0BA50000)
    check("global-Y probe expands destination height",
          lower_global[REG_RB_COPY_DEST_PITCH], 1280 | (720 << 16))
    check("global-Y probe disables vertex window offset",
          lower_global[REG_PA_SU_SC_MODE_CNTL] & (1 << 16), 0)
    check("global-Y probe disables scissor window offset",
          lower_global[REG_PA_SC_WINDOW_SCISSOR_TL] >> 31, 1)

    # The registers command's layout is where a silent format break would live:
    # a bool padded to a dword. 2 registers => 6 dwords of header + 2 of payload.
    w = TraceWriter()
    w.registers(0x2000, [0x11111111, 0x22222222])
    check("registers command size", len(w.buf), 6 * 4 + 2 * 4)
    check("registers command tag", struct.unpack_from("<I", w.buf, 0)[0], K_REGISTERS)
    check("registers first index", struct.unpack_from("<I", w.buf, 4)[0], 0x2000)
    check("registers payload length", struct.unpack_from("<I", w.buf, 20)[0], 8)

    # IM_LOAD_IMMEDIATE: the count covers the two body dwords PLUS the inline
    # microcode, and the microcode must NOT be byte-swapped.
    ucode = bytes(range(16))                      # 4 dwords
    dwords, payload = im_load_packet(SHADER_TYPE_PIXEL, ucode)
    check("im_load header", dwords[0],
          0xC0000000 | ((2 + 4 - 1) << 16) | (0x2B << 8))
    check("im_load shader type", dwords[1], SHADER_TYPE_PIXEL)
    check("im_load size dwords", dwords[2], 4)
    w3 = TraceWriter()
    w3.packet_with_payload(0x1000, dwords, payload)
    # count must be body + microcode, or Xenia reads the next command as opcodes
    check("im_load packet count", struct.unpack_from("<I", w3.buf, 8)[0], 3 + 4)
    check("microcode kept in guest byte order",
          w3.buf[12 + 3 * 4:12 + 3 * 4 + 4], ucode[:4])
    # A shader too large for a 14-bit count must be REFUSED, not truncated: a
    # truncated shader translates to something plausible and wrong.
    check("oversized shader refused",
          im_load_packet(SHADER_TYPE_VERTEX, b"\0" * (PM4_MAX_COUNT * 4)), None)

    # Packets are big-endian in guest memory; trace headers are little. Getting
    # this backwards yields a trace Xenia reads as garbage opcodes.
    w2 = TraceWriter()
    w2.packet(0x1000, [0xAABBCCDD])
    check("packet payload is big-endian",
          struct.unpack_from(">I", w2.buf, 12)[0], 0xAABBCCDD)

    # The swap packets, whose absence was the whole reason a trace rendered 744
    # draws and produced no file. Every field is checked against Xenia's own
    # VdSwap_entry, because a swap that parses but names the wrong buffer would
    # present something plausible.
    #   dword_1: format/endian/etc in the low 12 bits, base_address>>12 above.
    #   The base is a VIRTUAL address in the 0xC0000000 alias; the kernel posts
    #   the physical one.
    fetch = [0, (0xC1234 << 12) | 0x086, (720 - 1) << 13 | (1280 - 1), 0, 0, 0]
    type0, type3, w_, h_, phys = swap_packets(fetch, 0xC1234000)
    check("swap type0 header", type0[0], (5 << 16) | 0x4800)
    check("swap type0 carries six dwords", len(type0) - 1, 6)
    check("swap fetch base translated to physical", type0[2] >> 12, 0x01234)
    check("swap fetch low bits untouched", type0[2] & 0xFFF, 0x086)
    check("swap physical address", phys, 0x01234000)
    check("swap width from size_2d", w_, 1280)
    check("swap height from size_2d", h_, 720)
    check("swap type3 header", type3[0], 0xC0000000 | (3 << 16) | (0x64 << 8))
    check("swap signature is 'SWAP'", type3[1],
          int.from_bytes(b"SWAP", "big"))
    check("swap packet address", type3[2], 0x01234000)

    # And the negative: a capture that cannot state the front buffer must REFUSE
    # to swap and SAY so, not quietly emit a trace that renders nothing.
    class _Cap:
        version, front_buffer, front_fetch = 2, 0x1234000, None
    note = emit_swap(TraceWriter(), _Cap(), 0)
    check("v2 capture refuses to swap", note.startswith("NO SWAP"), True)
    _Cap.version, _Cap.front_fetch = 3, [0] * 6
    check("all-zero fetch refuses to swap",
          emit_swap(TraceWriter(), _Cap(), 0).startswith("NO SWAP"), True)
    # Disagreement between the two statements of the front buffer must stop the
    # build, not pick one.
    _Cap.front_fetch = [0, (0xC1234 << 12) | 0x086, 0, 0, 0, 0]
    _Cap.front_buffer = 0x5678000
    try:
        emit_swap(TraceWriter(), _Cap(), 0)
        check("mismatched front buffer refused", False, True)
    except SystemExit:
        check("mismatched front buffer refused", True, True)

    # Synthetic values deliberately isolate the base, pitch, height, format,
    # endian, swap, and dimensionality fields. None are copied from a capture.
    built = fetch_from_resolve(dict(base=0x1234000, pitch=96, height=17,
                                    info=(1 << 24) | (0x15 << 7) | 2))
    synthetic_expected = [0x80C00002, 0x1234095, 0x2005F,
                          0x1414, 0x0, 0x200]
    for i, want in enumerate(synthetic_expected):
        check(f"resolve-derived fetch dword_{i}", built[i], want)

    # --present frame: the swap must move to the frame's OWN resolve
    # destination, and must refuse when the frame performs no colour resolve.
    # Both are checked because a silent fallback to the guest's buffer would
    # render black and look like a renderer defect rather than a missing swap.
    def _cap_with_resolve(mode, src_select, dest):
        regs = [0] * REG_COUNT
        regs[REG_RB_MODECONTROL] = mode
        regs[REG_RB_COPY_CONTROL] = src_select
        regs[REG_RB_COPY_DEST_BASE] = dest
        regs[REG_RB_COPY_DEST_PITCH] = 1280 | (720 << 16)

        class C:
            version = 3
            front_buffer = 0xC1234000
            front_fetch = [0, (0xC1234 << 12) | 0x086,
                           (720 - 1) << 13 | (1280 - 1), 0, 0, 0]
            draws = [dict(regs=regs)]
        return C()

    note = emit_swap(TraceWriter(), _cap_with_resolve(6, 0, 0xC4000000), 0,
                     present="frame")
    check("--present frame moves the swap to the frame's resolve",
          "0x4000000" in note, True)
    check("--present frame says it departed from the guest's buffer",
          "instead of the front buffer the guest named" in note, True)
    # A DEPTH resolve is not a composite and must not be picked.
    note = emit_swap(TraceWriter(), _cap_with_resolve(6, 4, 0xC4000000), 0,
                     present="frame")
    check("--present frame ignores a depth resolve", note.startswith("NO SWAP"),
          True)
    # A frame with no resolve at all must refuse rather than swap something.
    note = emit_swap(TraceWriter(), _cap_with_resolve(4, 0, 0xC4000000), 0,
                     present="frame")
    check("--present frame refuses a frame with no colour resolve",
          note.startswith("NO SWAP"), True)

    # --present resolve:N must REFUSE the cases that would produce a confident
    # non-image, and must accept the one that would not. Both classes run.
    def _cap_with_resolves(specs):
        draws = []
        for mode, src_select, dest in specs:
            regs = [0] * REG_COUNT
            regs[REG_RB_MODECONTROL] = mode
            regs[REG_RB_COPY_CONTROL] = src_select
            regs[REG_RB_COPY_DEST_BASE] = dest
            regs[REG_RB_COPY_DEST_PITCH] = 1280 | (720 << 16)
            draws.append(dict(regs=regs))

        class C:
            version = 3
            front_buffer = 0xC1234000
            front_fetch = [0, (0xC1234 << 12) | 0x086,
                           (720 - 1) << 13 | (1280 - 1), 0, 0, 0]
        C.draws = draws
        return C()

    two = _cap_with_resolves([(6, 0, 0xC4000000), (6, 4, 0xC5000000),
                              (6, 0, 0xC6000000)])
    check("all_resolves numbers every resolve, depth included",
          [(r["index"], r["draw"], r["depth"]) for r in all_resolves(two)],
          [(0, 0, False), (1, 1, True), (2, 2, False)])
    note = emit_swap(TraceWriter(), two, 0, present="resolve:0")
    check("--present resolve:0 presents the FIRST resolve, not the last",
          "0x4000000" in note and "RESOLVE 0 of 3" in note, True)
    note = emit_swap(TraceWriter(), two, 0, present="resolve:1")
    check("--present resolve:N refuses a depth resolve",
          note.startswith("NO SWAP") and "DEPTH" in note, True)
    note = emit_swap(TraceWriter(), two, 0, present="resolve:9")
    check("--present resolve:N out of range lists what does exist",
          note.startswith("NO SWAP") and "0:0xc4000000" in note
          and "1:0xc5000000(depth)" in note, True)

    for label, got, want in selftest_cases():
        check(label, got, want)

    print("\nSELFTEST FAILED: " + ", ".join(failures) if failures
          else "\nselftest passed: the packing matches Xenia's headers by hand-check.")
    return 1 if failures else 0


def main(argv):
    args = argv[1:]
    if args[:1] == ["--selftest"]:
        return selftest()
    present = "guest"
    present_set = False
    if "--present" in args:
        present_set = True
        i = args.index("--present")
        present = args[i + 1]
        del args[i:i + 2]
        if present not in ("guest", "frame") and not present.startswith("resolve:"):
            print("--present takes 'guest' (the buffer VdSwap named), 'frame' "
                  "(this frame's own final colour resolve), or 'resolve:N' "
                  "(the Nth resolve, numbered as the oracle's IssueCopy log "
                  "numbers them; playback is truncated to end there)")
            return 2
    regs = "delta"
    if "--regs" in args:
        i = args.index("--regs")
        regs = args[i + 1]
        del args[i:i + 2]
        if regs not in ("full", "delta"):
            print("--regs takes 'delta' (faithful default: send only changes) "
                  "or 'full' (diagnostic: replay unchanged callbacks too)")
            return 2
    max_draws = None
    if "--draws" in args:
        i = args.index("--draws")
        max_draws = int(args[i + 1])
        del args[i:i + 2]
    checkpoint = None
    if "--checkpoint-copy" in args:
        i = args.index("--checkpoint-copy")
        checkpoint = args[i + 1]
        del args[i:i + 2]
        if max_draws is not None or present_set:
            print("--checkpoint-copy owns the prefix and presentation; do not "
                  "combine it with --draws or --present")
            return 2
    probe = None
    if "--probe-copy" in args:
        i = args.index("--probe-copy")
        probe = args[i + 1]
        del args[i:i + 2]
        if checkpoint is not None or max_draws is not None or present_set:
            print("--probe-copy owns the prefix and presentation and cannot be "
                  "combined with --checkpoint-copy, --draws or --present")
            return 2
    probe_msaa = None
    if "--probe-msaa" in args:
        i = args.index("--probe-msaa")
        value = args[i + 1].lower()
        del args[i:i + 2]
        choices = {"1x": 0, "2x": 1, "4x": 2}
        if probe is None or value not in choices:
            print("--probe-msaa takes 1x, 2x or 4x and may only be combined "
                  "with --probe-copy")
            return 2
        probe_msaa = choices[value]
    probe_global_y = "--probe-global-y" in args
    if probe_global_y:
        args.remove("--probe-global-y")
        if probe is None:
            print("--probe-global-y may only be combined with --probe-copy")
            return 2
    if len(args) != 2:
        print(__doc__)
        return 2
    src, dst = Path(args[0]), Path(args[1])
    if not src.is_file():
        print(f"REFUSING: {src} does not exist. Nothing was converted.")
        return 1
    return convert(src, dst, max_draws, present, regs, checkpoint, probe,
                   probe_msaa, probe_global_y)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
