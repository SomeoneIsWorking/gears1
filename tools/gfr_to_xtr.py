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
SHADER_TYPE_VERTEX = 0
SHADER_TYPE_PIXEL = 1
# A PM4 count field is 14 bits, so a packet cannot carry more than this many
# dwords. A shader bigger than that needs the pointer-based PM4_IM_LOAD instead.
PM4_MAX_COUNT = 0x3FFF
SRC_SELECT_DMA = 0
SRC_SELECT_AUTO_INDEX = 2
REG_VGT_DMA_BASE = 0x21FA
REG_VGT_DMA_SIZE = 0x21FB
REG_COUNT = 0x8000
# Xenia's register file is SMALLER than the one we capture (register_file.h:
# kRegisterCount). RestoreRegisters rejects the whole command when it overruns
# -- with a warning, then renders anyway from an unset register file, which
# looks like a backend failure rather than a format mismatch. Send the prefix
# Xenia can hold; every register either renderer reads lives well below it.
XENIA_REG_COUNT = 0x5003

def front_buffer_extent(cap):
    """The guest bytes the swap will read as the presented image, [lo, hi).

    Sized from the fetch constant the guest itself posted, not from the frame's
    1280x720: a tiled surface is stored at its row PITCH and padded to whole
    32-pixel tiles, so the buffer is larger than the visible image and the tail
    is exactly the part that looks free. Empty when the capture cannot say,
    which excludes nothing -- the caller is then no worse off than before.
    """
    if not cap.front_fetch or not any(cap.front_fetch):
        return (0, 0)
    base = (((cap.front_fetch[1] >> 12) << 12) & GUEST_PHYSICAL_MASK)
    pitch = ((cap.front_fetch[0] >> 22) & 0x1FF) * 32
    width = (cap.front_fetch[2] & 0x1FFF) + 1
    height = ((cap.front_fetch[2] >> 13) & 0x1FFF) + 1
    row_pixels = pitch if pitch else width
    rows = (height + 31) & ~31
    # Every format the front buffer can take is 32 bits per pixel: the kernel
    # accepts only k_8_8_8_8 and k_2_10_10_10_AS_16_16_16_16 (VdSwap_entry).
    return (base, base + row_pixels * rows * 4)


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

    The index base and size come from the draw's OWN register snapshot rather
    than being invented here: the capture does not store the index buffer's
    endian swap mode, but VGT_DMA_SIZE does, and inventing one would silently
    reorder every index.
    """
    regs = draw["regs"]
    src = SRC_SELECT_DMA if draw["indexed"] else SRC_SELECT_AUTO_INDEX
    initiator = ((draw["prim"] & 0x3F) | (src << 6) |
                 ((1 if draw["index32"] else 0) << 11) |
                 ((draw["index_count"] & 0xFFFF) << 16))
    body = [0, initiator]                         # viz query token, initiator
    if draw["indexed"]:
        dma_size = regs[REG_VGT_DMA_SIZE] if len(regs) > REG_VGT_DMA_SIZE else 0
        body += [draw["index_base"], dma_size]
    header = 0xC0000000 | ((len(body) - 1) << 16) | (PM4_DRAW_INDX << 8)
    return [header] + body


def emit_swap(w, cap, scratch):
    """Emit the swap, or say exactly why the trace will produce no image.

    The negative is written out deliberately. Without a swap the dump renders
    every draw, exits 1 and writes NOTHING -- which is indistinguishable from a
    trace whose draws all failed, and cost a session once already. So a trace
    that cannot swap says so here, at build time, rather than being discovered
    as a missing file an hour later.
    """
    if cap.front_fetch is None:
        return (f"NO SWAP: capture is v{cap.version}, which predates the front "
                f"buffer's fetch constant. Xenia takes the swap texture from "
                f"fetch slot 0, so this trace WILL render its draws and write no "
                f"image. Re-capture with a current build.")
    if not any(cap.front_fetch):
        return ("NO SWAP: the capture's front-buffer fetch constant is all "
                "zeroes -- the guest passed none, or the swap packet predates "
                "it. No image will be written.")
    type0, type3, width, height, physical = swap_packets(cap.front_fetch,
                                                         cap.front_buffer)
    # The fetch constant and the separately-recorded front buffer address are two
    # independent statements by the guest about the same buffer. Xenia's kernel
    # asserts they agree; if they do not here, one of them is being read wrong,
    # and rendering anyway would present some other surface convincingly.
    stated = (cap.front_fetch[1] >> 12) << 12
    if cap.front_buffer and (cap.front_buffer & GUEST_PHYSICAL_MASK) != \
            (stated & GUEST_PHYSICAL_MASK):
        raise SystemExit(
            f"REFUSING: the capture's front buffer address {cap.front_buffer:#x} "
            f"and its fetch constant's base {stated:#x} name different buffers. "
            f"One of the two is being read wrong; a trace built from either "
            f"would present a surface the guest did not name.")
    w.packet(scratch, type0)
    w.packet(scratch, type3)
    w.swap()
    return (f"swap: front buffer {physical:#x} ({width}x{height}), fetch "
            f"constant 0 posted as the guest's VdSwap does")


def convert(src: Path, dst: Path, max_draws=None):
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
    swap_note = emit_swap(w, cap, scratch)
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
    check("indexed packet length", len(pkt), 5)

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

    print("\nSELFTEST FAILED: " + ", ".join(failures) if failures
          else "\nselftest passed: the packing matches Xenia's headers by hand-check.")
    return 1 if failures else 0


def main(argv):
    args = argv[1:]
    if args[:1] == ["--selftest"]:
        return selftest()
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
    return convert(src, dst, max_draws)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
