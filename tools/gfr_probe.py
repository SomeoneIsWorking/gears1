"""Register transforms for synthetic live-EDRAM probes."""


REG_RB_SURFACE_INFO = 0x2000
REG_PA_SC_WINDOW_OFFSET = 0x2080
REG_PA_SC_WINDOW_SCISSOR_TL = 0x2081
REG_PA_SU_SC_MODE_CNTL = 0x2205
REG_RB_COPY_CONTROL = 0x2318
REG_RB_COPY_DEST_BASE = 0x2319
REG_RB_COPY_DEST_PITCH = 0x231A


def _sign_extend_15(value):
    value &= 0x7FFF
    return value - ((value & 0x4000) << 1)


def globalize_probe_y(regs):
    """Turn a lower-tile depth copy into a global-Y live-surface probe."""
    out = list(regs)
    if (out[REG_RB_COPY_CONTROL] & 0x7) < 4:
        raise ValueError("--probe-global-y requires a depth resolve")
    window = out[REG_PA_SC_WINDOW_OFFSET]
    window_x = _sign_extend_15(window)
    window_y = _sign_extend_15(window >> 16)
    if window_x != 0 or window_y >= 0:
        raise ValueError(
            "--probe-global-y requires a zero X and negative Y window offset")
    pitch_height = out[REG_RB_COPY_DEST_PITCH]
    pitch = pitch_height & 0x3FFF
    height = (pitch_height >> 16) & 0x3FFF
    if not pitch or not height:
        raise ValueError("--probe-global-y requires a non-empty copy destination")
    byte_offset = (-window_y) * pitch * 4
    base = out[REG_RB_COPY_DEST_BASE]
    if byte_offset > base or byte_offset & 0xFFF:
        raise ValueError(
            "--probe-global-y cannot derive a page-aligned full texture base")
    full_height = height - window_y
    if full_height > 0x3FFF:
        raise ValueError("--probe-global-y derived an invalid destination height")
    out[REG_RB_COPY_DEST_BASE] = base - byte_offset
    out[REG_RB_COPY_DEST_PITCH] = pitch | (full_height << 16)
    out[REG_PA_SU_SC_MODE_CNTL] &= ~(1 << 16)
    out[REG_PA_SC_WINDOW_SCISSOR_TL] |= 1 << 31
    return out
