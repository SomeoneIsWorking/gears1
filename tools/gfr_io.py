"""Binary I/O for Gears frame captures and Xenia GPU traces."""

import struct


TRACE_FORMAT_VERSION = 1
(K_PRIMARY_BUFFER_START, K_PRIMARY_BUFFER_END, K_INDIRECT_BUFFER_START,
 K_INDIRECT_BUFFER_END, K_PACKET_START, K_PACKET_END, K_MEMORY_READ,
 K_MEMORY_WRITE, K_EDRAM_SNAPSHOT, K_EVENT, K_REGISTERS, K_GAMMA_RAMP) = range(12)
ENCODING_NONE = 0
EVENT_SWAP = 0
GFR_MAGIC = b"GEARSFR1"


class Capture:
    """A parsed .gfr. Mirrors runtime/frame_capture.cpp's writer exactly."""

    def __init__(self, path):
        data = path.read_bytes()
        if data[:8] != GFR_MAGIC:
            raise ValueError(f"{path}: not a GEARSFR1 capture")
        offset = 8
        (self.version,) = struct.unpack_from("<I", data, offset); offset += 4
        if self.version not in (1, 2, 3, 4):
            raise ValueError(f"{path}: capture version {self.version} unsupported")
        (self.width, self.height, self.mirror) = struct.unpack_from(
            "<3I", data, offset)
        offset += 12
        if self.version >= 2:
            (self.front_buffer,) = struct.unpack_from("<I", data, offset)
            offset += 4
        else:
            self.front_buffer = 0
        if self.version >= 3:
            self.front_fetch = list(struct.unpack_from("<6I", data, offset))
            offset += 24
        else:
            # Unknown is distinct from an all-zero fetch, so callers refuse to
            # synthesize a swap rather than inventing a plausible descriptor.
            self.front_fetch = None
        (self.window,) = struct.unpack_from("<I", data, offset); offset += 4
        offset += 8  # sequence (int64)
        (draw_count,) = struct.unpack_from("<I", data, offset); offset += 4
        (self.block_size,) = struct.unpack_from("<I", data, offset); offset += 4
        (block_count,) = struct.unpack_from("<I", data, offset); offset += 4
        ids = struct.unpack_from(f"<{block_count}I", data, offset)
        offset += 4 * block_count
        self.blocks = []
        for block in ids:
            guest = block * self.block_size
            size = min(self.block_size, self.window - guest)
            self.blocks.append((guest, data[offset:offset + size]))
            offset += size
        (blob_count,) = struct.unpack_from("<I", data, offset); offset += 4
        self.blobs = []
        for _ in range(blob_count):
            offset += 8  # hash
            (size,) = struct.unpack_from("<I", data, offset); offset += 4
            self.blobs.append(data[offset:offset + size])
            offset += size
        self.draws = []
        for _ in range(draw_count):
            (reg_count,) = struct.unpack_from("<I", data, offset); offset += 4
            regs = (struct.unpack_from(f"<{reg_count}I", data, offset)
                    if reg_count else ())
            offset += 4 * reg_count
            values = struct.unpack_from("<6I", data, offset); offset += 24
            vs, ps, prim, index_count, flags, index_base = values
            self.draws.append(dict(
                regs=regs, prim=prim, index_count=index_count,
                indexed=bool(flags & 1), index32=bool(flags & 2),
                index_endian=((flags >> 2) & 3) if self.version >= 4 else 2,
                index_base=index_base, vs=vs, ps=ps))
        self.trailing = len(data) - offset


class TraceWriter:
    """Encoder for the subset of Xenia's trace protocol this converter emits."""

    def __init__(self):
        self.buf = bytearray()

    def header(self, title_id=0):
        self.buf += struct.pack("<I", TRACE_FORMAT_VERSION)
        self.buf += b"0" * 40
        self.buf += struct.pack("<I", title_id)

    def memory_read(self, base, data):
        """Load captured guest bytes during playback.

        Trace playback decompresses kMemoryRead into guest memory, while
        kMemoryWrite is a no-op there. Using the latter produces a valid trace
        that renders from empty memory.
        """
        self.buf += struct.pack("<IIIII", K_MEMORY_READ, base, ENCODING_NONE,
                                len(data), len(data))
        self.buf += data

    def registers(self, first, values, execute_callbacks=False):
        payload = struct.pack(f"<{len(values)}I", *values)
        self.buf += struct.pack("<IIIIiI", K_REGISTERS, first, len(values),
                                int(execute_callbacks), ENCODING_NONE,
                                len(payload))
        self.buf += payload

    def packet(self, base, dwords):
        """Encode PM4 dwords in their guest-memory byte order."""
        self.buf += struct.pack("<III", K_PACKET_START, base, len(dwords))
        self.buf += struct.pack(f">{len(dwords)}I", *dwords)
        self.buf += struct.pack("<I", K_PACKET_END)

    def packet_with_payload(self, base, dwords, payload):
        """Encode a packet whose tail is raw guest bytes.

        IM_LOAD_IMMEDIATE reads its microcode directly rather than through the
        packet reader's byte swap, so the payload must retain capture order.
        """
        total = len(dwords) + len(payload) // 4
        self.buf += struct.pack("<III", K_PACKET_START, base, total)
        self.buf += struct.pack(f">{len(dwords)}I", *dwords)
        self.buf += payload
        self.buf += struct.pack("<I", K_PACKET_END)

    def swap(self):
        self.buf += struct.pack("<II", K_EVENT, EVENT_SWAP)
