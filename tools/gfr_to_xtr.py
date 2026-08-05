#!/usr/bin/env python3
"""Convert one of our frame captures (.gfr) into a Xenia GPU trace (.xtr).

WHY. Xenia renders this title correctly, and it ships a HEADLESS renderer --
`xenia-gpu-vulkan-trace-dump` -- that turns a trace into an image with no
window, no controller and no playthrough. If our capture can be expressed as a
trace, every open question of the form "what should this frame look like?"
becomes one command instead of a play session. See catalog #7 and #62.

WHAT THIS CAN AND CANNOT SETTLE -- read before trusting a run.

A .gfr stores per-draw REGISTER SNAPSHOTS, not the PM4 packet stream the guest
actually sent; we never recorded the packets. So this SYNTHESISES a command
stream: it restores each draw's register file wholesale (Xenia's trace format
has a command for exactly that) and emits only the DRAW_INDX packet itself.

  * Sound for SHADING, RESOLVE and FORMAT questions -- everything downstream of
    the registers. Both renderers are driven from the same register values, so a
    difference in the output is a difference in how they were interpreted.
  * WORTHLESS for command-stream DECODE questions. Where we misread the guest's
    packets, this bakes our misreading into the oracle's input and it will agree
    with us precisely where we are wrong.

Say which kind of question is being asked before quoting a result.

    tools/gfr_to_xtr.py <capture.gfr> <out.xtr> [--draws N] [--selftest]
                        [--present guest|frame]

--present guest (default) swaps the buffer the guest's VdSwap named, which is
the faithful thing and, for a ONE-FRAME capture, is the buffer the PREVIOUS
frame filled -- so the oracle renders it black and is right to. --present frame
swaps the address this frame's own last colour resolve wrote, which is what
makes the oracle show the output of the draws in the trace. Use `frame` to
compare renderers and `guest` to ask about the present path.
"""
import struct
import sys
from pathlib import Path

# --- Xenia trace format (extern/xenia/src/xenia/gpu/trace_protocol.h) --------
TRACE_FORMAT_VERSION = 1
(K_PRIMARY_BUFFER_START, K_PRIMARY_BUFFER_END, K_INDIRECT_BUFFER_START,
 K_INDIRECT_BUFFER_END, K_PACKET_START, K_PACKET_END, K_MEMORY_READ,
 K_MEMORY_WRITE, K_EDRAM_SNAPSHOT, K_EVENT, K_REGISTERS, K_GAMMA_RAMP) = range(12)
ENCODING_NONE = 0
EVENT_SWAP = 0

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


def fetch_from_resolve(resolve):
    """Describe a resolve destination as a texture fetch constant.

    Every field comes from the resolve's OWN registers, which is what makes this
    usable on a capture that predates the front-buffer fetch constant -- and
    ColorFormat casts straight onto TextureFormat, exactly as Xenia does it
    (draw_util.cc: `TextureFormat(rb_copy_dest_info.copy_dest_format)`).

    VALIDATED, not asserted: on a capture where the frame resolves into the very
    buffer the guest then swaps, this reproduces the guest's own descriptor
    dword for dword apart from the address alias. The selftest pins that against
    real captured values, so a wrong field here fails a test rather than
    producing a plausible image.
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

GFR_MAGIC = b"GEARSFR1"


class Capture:
    """A parsed .gfr. Mirrors runtime/frame_capture.cpp's writer exactly."""

    def __init__(self, path: Path):
        d = path.read_bytes()
        if d[:8] != GFR_MAGIC:
            raise ValueError(f"{path}: not a GEARSFR1 capture")
        p = 8
        (self.version,) = struct.unpack_from("<I", d, p); p += 4
        if self.version not in (1, 2, 3):
            raise ValueError(f"{path}: capture version {self.version} unsupported")
        (self.width, self.height, self.mirror) = struct.unpack_from("<3I", d, p); p += 12
        if self.version >= 2:
            (self.front_buffer,) = struct.unpack_from("<I", d, p); p += 4
        else:
            self.front_buffer = 0
        if self.version >= 3:
            self.front_fetch = list(struct.unpack_from("<6I", d, p)); p += 24
        else:
            # Not "no fetch constant" but "this capture cannot say". Kept
            # distinct so the swap is REFUSED rather than synthesised from a
            # guessed format -- an invented one would make the oracle agree with
            # our guess about the front buffer by construction.
            self.front_fetch = None
        (self.window,) = struct.unpack_from("<I", d, p); p += 4
        p += 8                                    # sequence (int64)
        (draw_count,) = struct.unpack_from("<I", d, p); p += 4
        (self.block_size,) = struct.unpack_from("<I", d, p); p += 4
        (block_count,) = struct.unpack_from("<I", d, p); p += 4
        ids = struct.unpack_from(f"<{block_count}I", d, p); p += 4 * block_count
        self.blocks = []                          # (guest_addr, bytes)
        for b in ids:
            guest = b * self.block_size
            n = min(self.block_size, self.window - guest)
            self.blocks.append((guest, d[p:p + n]))
            p += n
        (blob_count,) = struct.unpack_from("<I", d, p); p += 4
        # The microcode blobs. KEPT, not skipped: Xenia binds a shader only via
        # an IM_LOAD packet, so the bytes have to be replayed into the trace.
        self.blobs = []
        for _ in range(blob_count):
            p += 8                                # hash
            (size,) = struct.unpack_from("<I", d, p); p += 4
            self.blobs.append(d[p:p + size])
            p += size
        self.draws = []
        for _ in range(draw_count):
            (reg_count,) = struct.unpack_from("<I", d, p); p += 4
            regs = struct.unpack_from(f"<{reg_count}I", d, p) if reg_count else ()
            p += 4 * reg_count
            (vs, ps, prim, index_count, flags, index_base) = struct.unpack_from("<6I", d, p)
            p += 24
            self.draws.append(dict(regs=regs, prim=prim, index_count=index_count,
                                   indexed=bool(flags & 1), index32=bool(flags & 2),
                                   index_base=index_base, vs=vs, ps=ps))
        self.trailing = len(d) - p


class TraceWriter:
    def __init__(self):
        self.buf = bytearray()

    def header(self, title_id=0):
        self.buf += struct.pack("<I", TRACE_FORMAT_VERSION)
        self.buf += b"0" * 40                     # build_commit_sha
        self.buf += struct.pack("<I", title_id)

    def memory_read(self, base, data):
        """kMemoryRead: playback decompresses this INTO guest memory.

        kMemoryWrite is a no-op on playback (trace_player.cc), so loading the
        capture's pages must use kMemoryRead -- picking the wrong one produces a
        trace that parses perfectly and renders from empty memory.
        """
        self.buf += struct.pack("<IIIII", K_MEMORY_READ, base, ENCODING_NONE,
                                len(data), len(data))
        self.buf += data

    def registers(self, first, values, execute_callbacks=False):
        payload = struct.pack(f"<{len(values)}I", *values)
        # bool is one byte and the struct is 4-aligned, so the compiler pads it
        # to a dword before encoding_format. Matching that padding is what makes
        # the file readable at all.
        self.buf += struct.pack("<IIIIiI", K_REGISTERS, first, len(values),
                                1 if execute_callbacks else 0, ENCODING_NONE,
                                len(payload))
        self.buf += payload

    def packet(self, base, dwords):
        """A PM4 packet: copied to guest memory, then executed.

        Xenia reads packets with ReadAndSwap, so they live BIG-endian in guest
        memory while every trace command header is host-endian little.
        """
        self.buf += struct.pack("<III", K_PACKET_START, base, len(dwords))
        self.buf += struct.pack(f">{len(dwords)}I", *dwords)
        self.buf += struct.pack("<I", K_PACKET_END)

    def packet_with_payload(self, base, dwords, payload):
        """A packet whose tail is raw guest bytes rather than dwords.

        IM_LOAD_IMMEDIATE reads its microcode through read_ptr() directly, NOT
        through ReadAndSwap like the packet's own dwords -- so the microcode must
        stay in the byte order it has in guest memory, which is how the capture
        stored it. Swapping it here as well would hand Xenia a shader with every
        instruction word reversed.
        """
        total = len(dwords) + len(payload) // 4
        self.buf += struct.pack("<III", K_PACKET_START, base, total)
        self.buf += struct.pack(f">{len(dwords)}I", *dwords)
        self.buf += payload
        self.buf += struct.pack("<I", K_PACKET_END)

    def swap(self):
        self.buf += struct.pack("<II", K_EVENT, EVENT_SWAP)


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


def convert(src: Path, dst: Path, max_draws=None, present="guest"):
    cap = Capture(src)
    w = TraceWriter()
    w.header()
    # Guest memory first: vertices, indices, textures and the shader microcode
    # the registers point at all live here.
    for base, data in cap.blocks:
        w.memory_read(base, data)
    scratch = pick_packet_scratch(cap)
    draws = cap.draws if max_draws is None else cap.draws[:max_draws]
    emitted = skipped_no_regs = 0
    too_big = set()
    last_vs = last_ps = None
    for d in draws:
        if len(d["regs"]) < REG_COUNT:
            # A draw whose snapshot is short cannot restore state; dropping it
            # silently would make the oracle render a DIFFERENT frame and look
            # authoritative doing it.
            skipped_no_regs += 1
            continue
        w.registers(0, d["regs"][:XENIA_REG_COUNT])
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

    # The resolve-derived descriptor, pinned against REAL captured values.
    # These registers are swap_v3.gfr's final colour resolve, and the expected
    # dwords are that capture's own front-buffer fetch constant as the guest
    # posted it -- the two describe the same buffer, so agreement here is the
    # discriminator actually run against a case whose answer is known
    # independently. Only the address alias differs, which Xenia's kernel
    # strips.
    built = fetch_from_resolve(dict(base=0x311000, pitch=1280, height=720,
                                    info=0x1000300))
    guest_posted = [0x8A000002, 0xA0311006, 0x59E4FF, 0x1414, 0x0, 0x200]
    for i, want in enumerate(guest_posted):
        if i == 1:
            want &= ~0xA0000000        # the alias, stripped
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

    print("\nSELFTEST FAILED: " + ", ".join(failures) if failures
          else "\nselftest passed: the packing matches Xenia's headers by hand-check.")
    return 1 if failures else 0


def main(argv):
    args = argv[1:]
    if args[:1] == ["--selftest"]:
        return selftest()
    present = "guest"
    if "--present" in args:
        i = args.index("--present")
        present = args[i + 1]
        del args[i:i + 2]
        if present not in ("guest", "frame"):
            print("--present takes 'guest' (what VdSwap named) or 'frame' "
                  "(this frame's own final colour resolve)")
            return 2
    max_draws = None
    if "--draws" in args:
        i = args.index("--draws")
        max_draws = int(args[i + 1])
        del args[i:i + 2]
    if len(args) != 2:
        print(__doc__)
        return 2
    src, dst = Path(args[0]), Path(args[1])
    if not src.is_file():
        print(f"REFUSING: {src} does not exist. Nothing was converted.")
        return 1
    return convert(src, dst, max_draws, present)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
