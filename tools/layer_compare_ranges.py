"""Range-location policy for legacy layer-comparison captures."""

from xenos_tiled import stored_rows


def merge_bands(dumps, color_bytes_per_pixel):
    """Join structurally identical, address-contiguous horizontal bands."""
    merged = []
    changed = True
    while changed:
        changed = False
        for key in sorted(dumps):
            path, dest, length, endian, base, extra = dumps[key]
            bpp = 4 if key[0] == "D" else color_bytes_per_pixel.get(key[4], 4)
            rows = stored_rows(length, key[2], bpp)
            if not rows:
                continue
            want = dest + key[2] * rows * bpp
            following = next(
                (
                    other
                    for other in sorted(dumps)
                    if other != key
                    and (other[0], other[1], other[2], other[4])
                    == (key[0], key[1], key[2], key[4])
                    and other[3] < key[3]
                    and dumps[other][1] == want
                ),
                None,
            )
            if following is None:
                continue
            path2, dest2, length2, endian2, base2, extra2 = dumps.pop(following)
            dumps[key] = (
                path,
                dest,
                length + length2,
                endian,
                base,
                extra + [(path2, dest2, length2, endian2, base2)] + extra2,
            )
            merged.append((key, following))
            changed = True
            break
    return merged


def infer_legacy_texture_bases(dumps, color_bytes_per_pixel):
    """Recover a missing texture base from a unique complete sibling dump.

    Older oracle captures recorded only the probed range address. That cannot
    locate an arbitrary range in a tiled texture, but a complete dump of the
    same structural destination pins the allocation base. Ambiguous and
    unsupported groups remain undecodable rather than acquiring a guessed
    offset.
    """
    groups = {}
    for key, value in dumps.items():
        groups.setdefault(key[:5], []).append((key, value))

    receipts = []
    for structural_key, entries in groups.items():
        src, _source_base, width, height, fmt = structural_key
        bpp = 4 if src == "D" else color_bytes_per_pixel.get(fmt, 0)
        if not bpp:
            continue
        candidates = {
            base if base is not None else dest
            for _key, (_path, dest, length, _endian, base, _extra) in entries
            if stored_rows(length, width, bpp) == height
        }
        if len(candidates) != 1:
            continue
        inferred = next(iter(candidates))
        updated = 0
        for key, value in entries:
            path, dest, length, endian, base, extra = value
            if base is not None:
                continue
            dumps[key] = (path, dest, length, endian, inferred, extra)
            updated += 1
        if updated:
            receipts.append((structural_key, inferred, updated))
    return receipts


def selftest_legacy_texture_bases():
    """Exercise the unique proof and the ambiguous refusal."""
    key = ("D", 0x123, 32, 32, 22)
    full_bytes = 32 * 32 * 4
    legacy = {
        key + (0,): ("full", 0x2000, full_bytes, 2, None, []),
        key + (1,): ("part", 0x2200, 777, 2, None, []),
    }
    receipts = infer_legacy_texture_bases(legacy, {})
    unique_ok = (
        len(receipts) == 1
        and legacy[key + (0,)][4] == 0x2000
        and legacy[key + (1,)][4] == 0x2000
    )
    ambiguous = {
        key + (0,): ("full-a", 0x2000, full_bytes, 2, None, []),
        key + (1,): ("part", 0x2200, 777, 2, None, []),
        key + (2,): ("full-b", 0x9000, full_bytes, 2, None, []),
    }
    ambiguous_ok = infer_legacy_texture_bases(ambiguous, {}) == []
    return unique_ok, ambiguous_ok
