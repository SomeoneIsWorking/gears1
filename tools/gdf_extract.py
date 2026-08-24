#!/usr/bin/env python3
"""Xbox 360 GDF (XGD) ISO reader.

Lists or extracts individual files from an Xbox 360 game disc image without
unpacking the whole 7.8 GB filesystem.

Usage:
    gdf_extract.py <iso> --list
    gdf_extract.py <iso> --extract default.xex --out <path>
    gdf_extract.py <iso> --extract-all <directory>

The ISO path defaults to $GEARS_ISO (see .env).  Nothing from the disc is ever
written inside the repo's tracked tree -- point --out at scratch/.
"""
import argparse
import errno
import os
from pathlib import Path
import stat
import struct
import sys

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
# Candidate offsets of the game partition inside the image file.
# 0            : raw GDF dump
# 0xFD90000    : XGD1 / XGD2 video-partition offset
# 0x02080000   : XGD2 alternate
# 0x18300000   : XGD3
CANDIDATE_BASES = (0x0, 0xFD90000, 0x02080000, 0x18300000)

ATTR_DIRECTORY = 0x10
ENTRY_HEADER_SIZE = 14
COPY_CHUNK = 8 << 20


class GdfError(Exception):
    """The image or requested extraction violates the GDF safety contract."""


def _stream_size(f):
    position = f.tell()
    f.seek(0, os.SEEK_END)
    size = f.tell()
    f.seek(position)
    return size


def _extent_offset(base, sector):
    return base + sector * SECTOR


def _validate_extent(image_size, base, sector, size, description):
    start = _extent_offset(base, sector)
    end = start + size
    if start < base or start > image_size or end < start or end > image_size:
        raise GdfError(
            f"{description} extent lies outside the image: "
            f"offset 0x{start:X}, size {size}, image size {image_size}"
        )
    return start


def _read_exact(f, offset, size, description):
    f.seek(offset)
    data = f.read(size)
    if len(data) != size:
        raise GdfError(
            f"short read for {description}: expected {size} bytes, got {len(data)}"
        )
    return data


def find_base(f):
    for base in CANDIDATE_BASES:
        f.seek(base + 32 * SECTOR)
        if f.read(20) == MAGIC:
            return base
    raise GdfError("no MICROSOFT*XBOX*MEDIA volume descriptor found; "
                   "not a recognised XGD image")


def read_volume(f, base):
    descriptor_offset = base + 32 * SECTOR
    image_size = _stream_size(f)
    if descriptor_offset + SECTOR > image_size:
        raise GdfError("truncated volume descriptor")
    hdr = _read_exact(f, descriptor_offset, SECTOR, "volume descriptor")
    if hdr[:20] != MAGIC:
        raise GdfError("volume descriptor magic mismatch")
    root_sector, root_size = struct.unpack_from("<II", hdr, 20)
    return root_sector, root_size


def walk_dir(f, base, sector, size, prefix=""):
    """Yield (path, start_sector, size, attributes) for a directory table."""
    state = {
        "image_size": _stream_size(f),
        "directory_extents": set(),
    }
    entries = _walk_dir(f, base, sector, size, prefix, state)
    _validate_output_entries(entries)
    return entries


def _validate_name(name):
    if not name or name in (".", "..") or "/" in name or "\\" in name or "\0" in name:
        raise GdfError(f"unsafe GDF entry name {name!r}")


def _walk_dir(f, base, sector, size, prefix, state):
    extent = (sector, size)
    if extent in state["directory_extents"]:
        raise GdfError(
            f"directory extent cycle or reused directory table at sector {sector}, size {size}"
        )
    state["directory_extents"].add(extent)

    table_offset = _validate_extent(
        state["image_size"], base, sector, size, "directory table"
    )
    table = _read_exact(f, table_offset, size, "directory table")
    if not table:
        return []

    out = []
    stack = [0]
    visited = set()
    while stack:
        entry_offset = stack.pop()
        if entry_offset in visited:
            raise GdfError(
                f"directory entry pointer cycle or duplicate reference at offset {entry_offset * 4}"
            )
        visited.add(entry_offset)
        byte_offset = entry_offset * 4
        if byte_offset + ENTRY_HEADER_SIZE > len(table):
            raise GdfError(
                f"truncated directory entry at table offset {byte_offset}"
            )
        left, right, start, file_size, attr, name_length = struct.unpack_from(
            "<HHIIBB", table, byte_offset
        )
        if left == 0xFFFF:
            continue
        name_end = byte_offset + ENTRY_HEADER_SIZE + name_length
        if name_end > len(table):
            raise GdfError(
                f"truncated directory name at table offset {byte_offset}"
            )
        name = table[byte_offset + ENTRY_HEADER_SIZE:name_end].decode("latin-1")
        _validate_name(name)
        path = prefix + name
        description = "directory" if attr & ATTR_DIRECTORY else "file"
        _validate_extent(
            state["image_size"], base, start, file_size, f"{description} {path!r}"
        )
        out.append((path, start, file_size, attr))
        for child in (right, left):
            if child:
                stack.append(child)

    result = []
    for path, start, file_size, attr in out:
        result.append((path, start, file_size, attr))
        if attr & ATTR_DIRECTORY and file_size:
            result.extend(
                _walk_dir(f, base, start, file_size, path + "/", state)
            )
    return result


def _validate_output_entries(entries):
    seen = {}
    for path, _start, _size, attr in entries:
        components = path.split("/")
        if not components:
            raise GdfError("empty output path in GDF directory tree")
        for component in components:
            _validate_name(component)
        folded = path.casefold()
        previous = seen.get(folded)
        if previous is not None:
            previous_path, _previous_is_directory = previous
            if previous_path == path:
                raise GdfError(f"duplicate output path {path!r}")
            raise GdfError(
                f"case-colliding output paths {previous_path!r} and {path!r}"
            )
        seen[folded] = (path, bool(attr & ATTR_DIRECTORY))

    for folded, (path, _is_directory) in seen.items():
        components = folded.split("/")
        for count in range(1, len(components)):
            prefix = "/".join(components[:count])
            owner = seen.get(prefix)
            if owner is not None and not owner[1]:
                raise GdfError(
                    f"output path conflict: file {owner[0]!r} is a parent of {path!r}"
                )


def _open_extraction_root(root):
    root = Path(root)
    if os.path.lexists(root) and root.is_symlink():
        raise GdfError(f"extraction destination is a symlink: {root}")
    root.mkdir(parents=True, exist_ok=True)
    if not root.is_dir():
        raise GdfError(f"extraction destination is not a directory: {root}")
    flags = os.O_RDONLY | os.O_DIRECTORY
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    try:
        return os.open(root, flags)
    except OSError as exc:
        raise GdfError(f"cannot open extraction destination {root}: {exc}") from exc


def _open_output_parent(root_fd, components):
    current = os.dup(root_fd)
    flags = os.O_RDONLY | os.O_DIRECTORY
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    try:
        for component in components[:-1]:
            try:
                os.mkdir(component, dir_fd=current)
            except FileExistsError:
                pass
            try:
                child = os.open(component, flags, dir_fd=current)
            except OSError as exc:
                raise GdfError(
                    f"output parent {component!r} is not a safe directory or is a symlink"
                ) from exc
            os.close(current)
            current = child
        return current, components[-1]
    except Exception:
        os.close(current)
        raise


def _source_matches_fd(f, source_offset, size, output_fd):
    output_stat = os.fstat(output_fd)
    if not stat.S_ISREG(output_stat.st_mode) or output_stat.st_size != size:
        return False
    f.seek(source_offset)
    os.lseek(output_fd, 0, os.SEEK_SET)
    remaining = size
    while remaining:
        wanted = min(remaining, COPY_CHUNK)
        source_chunk = f.read(wanted)
        if len(source_chunk) != wanted:
            raise GdfError(
                f"short read while verifying existing output: expected {wanted} bytes, "
                f"got {len(source_chunk)}"
            )
        output_chunk = os.read(output_fd, wanted)
        if output_chunk != source_chunk:
            return False
        remaining -= wanted
    return os.read(output_fd, 1) == b""


def _write_all(fd, data):
    view = memoryview(data)
    while view:
        written = os.write(fd, view)
        if written <= 0:
            raise GdfError("short write while extracting file")
        view = view[written:]


def _copy_to_output(f, source_offset, size, parent_fd, name, verify_existing):
    existing_fd = None
    try:
        flags = os.O_RDONLY
        if hasattr(os, "O_NOFOLLOW"):
            flags |= os.O_NOFOLLOW
        try:
            existing_fd = os.open(name, flags, dir_fd=parent_fd)
        except FileNotFoundError:
            pass
        except OSError as exc:
            if exc.errno == errno.ELOOP:
                raise GdfError(f"output file {name!r} is a symlink") from exc
            raise GdfError(f"cannot inspect existing output {name!r}: {exc}") from exc
        if existing_fd is not None:
            existing_stat = os.fstat(existing_fd)
            if not stat.S_ISREG(existing_stat.st_mode):
                raise GdfError(f"existing output {name!r} is not a regular file")
            if verify_existing and _source_matches_fd(
                f, source_offset, size, existing_fd
            ):
                return False
    finally:
        if existing_fd is not None:
            os.close(existing_fd)

    temporary_name = None
    temporary_fd = None
    try:
        for attempt in range(100):
            temporary_name = f".gdf-extract-{os.getpid()}-{attempt}"
            flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
            if hasattr(os, "O_NOFOLLOW"):
                flags |= os.O_NOFOLLOW
            try:
                temporary_fd = os.open(
                    temporary_name, flags, 0o644, dir_fd=parent_fd
                )
                break
            except FileExistsError:
                continue
        if temporary_fd is None:
            raise GdfError("could not allocate a temporary extraction file")

        f.seek(source_offset)
        remaining = size
        while remaining:
            wanted = min(remaining, COPY_CHUNK)
            chunk = f.read(wanted)
            if len(chunk) != wanted:
                raise GdfError(
                    f"short read while extracting {name!r}: expected {wanted} bytes, "
                    f"got {len(chunk)}"
                )
            _write_all(temporary_fd, chunk)
            remaining -= wanted
        os.fsync(temporary_fd)
        os.close(temporary_fd)
        temporary_fd = None
        os.replace(
            temporary_name,
            name,
            src_dir_fd=parent_fd,
            dst_dir_fd=parent_fd,
        )
        temporary_name = None
        os.fsync(parent_fd)
        return True
    finally:
        if temporary_fd is not None:
            os.close(temporary_fd)
        if temporary_name is not None:
            try:
                os.unlink(temporary_name, dir_fd=parent_fd)
            except FileNotFoundError:
                pass


def extract_all(f, base, entries, root):
    """Safely extract validated entries beneath *root*, resuming by byte equality."""
    _validate_output_entries(entries)
    files = [entry for entry in entries if not (entry[3] & ATTR_DIRECTORY)]
    total = sum(entry[2] for entry in files)
    completed = 0
    root_fd = _open_extraction_root(root)
    try:
        for index, (path, start, size, _attr) in enumerate(sorted(files), 1):
            parent_fd, name = _open_output_parent(root_fd, path.split("/"))
            try:
                _copy_to_output(
                    f,
                    _extent_offset(base, start),
                    size,
                    parent_fd,
                    name,
                    verify_existing=True,
                )
            finally:
                os.close(parent_fd)
            completed += size
            if index % 100 == 0 or index == len(files):
                sys.stderr.write(
                    f"\r{index}/{len(files)} files, "
                    f"{completed / 1e9:.2f}/{total / 1e9:.2f} GB"
                )
                sys.stderr.flush()
    finally:
        os.close(root_fd)
    if files:
        sys.stderr.write("\n")


def extract_one(f, base, start, size, destination):
    destination = Path(destination)
    _validate_name(destination.name)
    destination.parent.mkdir(parents=True, exist_ok=True)
    parent_fd = _open_extraction_root(destination.parent)
    try:
        _copy_to_output(
            f,
            _extent_offset(base, start),
            size,
            parent_fd,
            destination.name,
            verify_existing=False,
        )
    finally:
        os.close(parent_fd)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("iso", nargs="?", default=os.environ.get("GEARS_ISO"))
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--extract", help="path inside the disc, e.g. default.xex")
    ap.add_argument("--out", help="destination file for --extract")
    ap.add_argument("--extract-all", metavar="DIR",
                    help="extract the whole disc into DIR, preserving layout")
    args = ap.parse_args()

    if not args.iso:
        raise SystemExit("no ISO given and $GEARS_ISO is unset")

    with open(args.iso, "rb") as f:
        base = find_base(f)
        root_sector, root_size = read_volume(f, base)
        entries = walk_dir(f, base, root_sector, root_size)
        sys.stderr.write(
            f"partition base 0x{base:X}, root sector {root_sector}, "
            f"{len(entries)} entries\n")

        if args.list:
            for path, start, size, attr in sorted(entries):
                kind = "DIR " if attr & ATTR_DIRECTORY else "FILE"
                print(f"{kind} {size:>12} {path}")
            return

        if args.extract_all:
            extract_all(f, base, entries, args.extract_all)
            sys.stderr.write("done\n")
            return

        if args.extract:
            want = args.extract.lower().replace("\\", "/")
            for path, start, size, attr in entries:
                if path.lower() == want and not (attr & ATTR_DIRECTORY):
                    if not args.out:
                        raise SystemExit("--out is required with --extract")
                    extract_one(f, base, start, size, args.out)
                    sys.stderr.write(f"wrote {size} bytes to {args.out}\n")
                    return
            raise SystemExit(f"{args.extract!r} not found on disc")

        raise SystemExit("nothing to do: pass --list or --extract")


if __name__ == "__main__":
    try:
        main()
    except GdfError as exc:
        raise SystemExit(str(exc)) from exc
