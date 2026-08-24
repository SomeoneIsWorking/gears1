#!/usr/bin/env python3
"""Synthetic safety tests for the XGD filesystem reader and extractor."""

from __future__ import annotations

import importlib.util
import io
import os
import subprocess
import struct
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "gdf_extract", REPO_ROOT / "tools" / "gdf_extract.py"
)
assert SPEC is not None and SPEC.loader is not None
gdf_extract = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = gdf_extract
SPEC.loader.exec_module(gdf_extract)


ROOT_SECTOR = 40
FILE_SECTOR = 48


def entry(
    name: str,
    *,
    start: int = FILE_SECTOR,
    size: int = 0,
    attr: int = 0,
    left: int = 0,
    right: int = 0,
) -> bytes:
    encoded = name.encode("latin-1")
    return struct.pack("<HHIIBB", left, right, start, size, attr, len(encoded)) + encoded


def table(entries: dict[int, bytes], size: int | None = None) -> bytes:
    required = max((offset * 4 + len(value) for offset, value in entries.items()), default=0)
    result = bytearray(required if size is None else size)
    for offset, value in entries.items():
        start = offset * 4
        result[start:start + len(value)] = value
    return bytes(result)


def image(directory: bytes, payloads: dict[int, bytes] | None = None) -> io.BytesIO:
    payloads = payloads or {}
    extents = [ROOT_SECTOR * gdf_extract.SECTOR + len(directory)]
    extents.extend(
        sector * gdf_extract.SECTOR + len(data)
        for sector, data in payloads.items()
    )
    end = max(extents)
    data = bytearray(end)
    descriptor = 32 * gdf_extract.SECTOR
    data[descriptor:descriptor + len(gdf_extract.MAGIC)] = gdf_extract.MAGIC
    struct.pack_into("<II", data, descriptor + 20, ROOT_SECTOR, len(directory))
    root = ROOT_SECTOR * gdf_extract.SECTOR
    data[root:root + len(directory)] = directory
    for sector, payload in payloads.items():
        offset = sector * gdf_extract.SECTOR
        data[offset:offset + len(payload)] = payload
    return io.BytesIO(data)


def parse(source: io.BytesIO) -> list[tuple[str, int, int, int]]:
    base = gdf_extract.find_base(source)
    sector, size = gdf_extract.read_volume(source, base)
    return gdf_extract.walk_dir(source, base, sector, size)


class ShortPayload(io.BytesIO):
    def __init__(self, data: bytes, short_at: int) -> None:
        super().__init__(data)
        self.short_at = short_at

    def read(self, size: int = -1) -> bytes:
        if self.tell() >= self.short_at:
            return b""
        available = self.short_at - self.tell()
        if size < 0 or size > available:
            size = available
        return super().read(size)


class GdfExtractTests(unittest.TestCase):
    def setUp(self) -> None:
        scratch = REPO_ROOT / "scratch"
        scratch.mkdir(exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=scratch)
        self.root = Path(self.temporary.name)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_valid_tree_extracts_and_same_size_stale_file_is_replaced(self) -> None:
        payload = b"right bytes"
        directory = table({0: entry("data.bin", size=len(payload))})
        source = image(directory, {FILE_SECTOR: payload})
        entries = parse(source)
        destination = self.root / "out"
        destination.mkdir()
        stale = destination / "data.bin"
        stale.write_bytes(b"wrong bytes")

        gdf_extract.extract_all(source, 0, entries, destination)

        self.assertEqual(stale.read_bytes(), payload)

    def test_matching_existing_file_is_verified_and_left_untouched(self) -> None:
        payload = b"already complete"
        directory = table({0: entry("data.bin", size=len(payload))})
        source = image(directory, {FILE_SECTOR: payload})
        entries = parse(source)
        destination = self.root / "out"
        destination.mkdir()
        existing = destination / "data.bin"
        existing.write_bytes(payload)
        original_stat = existing.stat()

        gdf_extract.extract_all(source, 0, entries, destination)

        current_stat = existing.stat()
        self.assertEqual(current_stat.st_ino, original_stat.st_ino)
        self.assertEqual(current_stat.st_mtime_ns, original_stat.st_mtime_ns)

    def test_list_and_single_extract_cli_remain_compatible(self) -> None:
        payload = b"single file"
        source = image(
            table({0: entry("data.bin", size=len(payload))}),
            {FILE_SECTOR: payload},
        )
        disc = self.root / "disc.iso"
        disc.write_bytes(source.getvalue())
        tool = REPO_ROOT / "tools" / "gdf_extract.py"

        listed = subprocess.run(
            [sys.executable, tool, disc, "--list"],
            check=True,
            capture_output=True,
            text=True,
        )
        self.assertIn("FILE", listed.stdout)
        self.assertIn("data.bin", listed.stdout)

        destination = self.root / "single" / "result.bin"
        subprocess.run(
            [
                sys.executable,
                tool,
                disc,
                "--extract",
                "DATA.BIN",
                "--out",
                destination,
            ],
            check=True,
            capture_output=True,
            text=True,
        )
        self.assertEqual(destination.read_bytes(), payload)

    def test_absolute_traversal_and_separator_names_are_refused(self) -> None:
        for unsafe in ("..", "../escape", "/absolute", r"dir\escape", "."):
            with self.subTest(unsafe=unsafe):
                source = image(table({0: entry(unsafe)}))
                with self.assertRaisesRegex(gdf_extract.GdfError, "unsafe.*name"):
                    parse(source)

    def test_directory_entry_pointer_cycle_is_refused(self) -> None:
        directory = table(
            {
                0: entry("root", start=ROOT_SECTOR, left=8),
                8: entry("child", start=ROOT_SECTOR, left=8),
            }
        )
        with self.assertRaisesRegex(gdf_extract.GdfError, "cycle|duplicate reference"):
            parse(image(directory))

    def test_recursive_directory_extent_cycle_is_refused(self) -> None:
        first = entry(
            "loop",
            start=ROOT_SECTOR,
            size=0,
            attr=gdf_extract.ATTR_DIRECTORY,
        )
        directory = entry(
            "loop",
            start=ROOT_SECTOR,
            size=len(first),
            attr=gdf_extract.ATTR_DIRECTORY,
        )
        # The final encoded size is stable because the size field is fixed-width.
        directory = entry(
            "loop",
            start=ROOT_SECTOR,
            size=len(directory),
            attr=gdf_extract.ATTR_DIRECTORY,
        )
        with self.assertRaisesRegex(gdf_extract.GdfError, "directory.*cycle|reused"):
            parse(image(directory))

    def test_duplicate_and_case_colliding_output_paths_are_refused(self) -> None:
        for child_name in ("Name", "name"):
            with self.subTest(child_name=child_name):
                directory = table(
                    {
                        0: entry("Name", start=ROOT_SECTOR, right=8),
                        8: entry(child_name, start=ROOT_SECTOR),
                    }
                )
                with self.assertRaisesRegex(gdf_extract.GdfError, "duplicate|case-colliding"):
                    parse(image(directory))

    def test_file_directory_prefix_conflicts_refuse_before_mutation(self) -> None:
        source = image(table({0: entry("unused", start=ROOT_SECTOR)}))
        conflicting_sets = (
            [
                ("foo", ROOT_SECTOR, 0, 0),
                ("foo/bar", ROOT_SECTOR, 0, 0),
            ],
            [
                ("same", ROOT_SECTOR, 0, gdf_extract.ATTR_DIRECTORY),
                ("same", ROOT_SECTOR, 0, 0),
            ],
        )
        for index, entries in enumerate(conflicting_sets):
            with self.subTest(entries=entries):
                destination = self.root / f"out-{index}"
                with self.assertRaisesRegex(
                    gdf_extract.GdfError, "conflict|duplicate"
                ):
                    gdf_extract.extract_all(source, 0, entries, destination)
                self.assertFalse(destination.exists())

    def test_truncated_directory_header_and_name_are_refused(self) -> None:
        with self.assertRaisesRegex(gdf_extract.GdfError, "truncated directory entry"):
            parse(image(b"\0" * 13))
        truncated_name = struct.pack("<HHIIBB", 0, 0, FILE_SECTOR, 0, 0, 4) + b"ab"
        with self.assertRaisesRegex(gdf_extract.GdfError, "truncated directory name"):
            parse(image(truncated_name))

    def test_out_of_bounds_directory_table_and_file_extent_are_refused(self) -> None:
        source = image(table({0: entry("data.bin")}))
        descriptor = 32 * gdf_extract.SECTOR
        raw = bytearray(source.getvalue())
        struct.pack_into("<II", raw, descriptor + 20, 0xFFFFFFF0, 64)
        with self.assertRaisesRegex(gdf_extract.GdfError, "directory table.*outside"):
            parse(io.BytesIO(raw))

        directory = table({0: entry("data.bin", start=0xFFFFFFF0, size=16)})
        with self.assertRaisesRegex(gdf_extract.GdfError, "file.*outside"):
            parse(image(directory))

    def test_short_source_read_refuses_partial_output(self) -> None:
        payload = b"complete payload"
        normal = image(
            table({0: entry("data.bin", size=len(payload))}),
            {FILE_SECTOR: payload},
        )
        entries = parse(normal)
        short = ShortPayload(normal.getvalue(), FILE_SECTOR * gdf_extract.SECTOR + 4)
        destination = self.root / "out"

        with self.assertRaisesRegex(gdf_extract.GdfError, "short read"):
            gdf_extract.extract_all(short, 0, entries, destination)
        self.assertFalse((destination / "data.bin").exists())

    def test_symlink_in_destination_cannot_escape_extraction_root(self) -> None:
        payload = b"confined"
        child_table = table({0: entry("file.bin", size=len(payload))})
        child_sector = 44
        root_table = table(
            {
                0: entry(
                    "dir",
                    start=child_sector,
                    size=len(child_table),
                    attr=gdf_extract.ATTR_DIRECTORY,
                )
            }
        )
        source = image(
            root_table,
            {child_sector: child_table, FILE_SECTOR: payload},
        )
        entries = parse(source)
        destination = self.root / "out"
        outside = self.root / "outside"
        destination.mkdir()
        outside.mkdir()
        (destination / "dir").symlink_to(outside, target_is_directory=True)

        with self.assertRaisesRegex(gdf_extract.GdfError, "symlink|directory"):
            gdf_extract.extract_all(source, 0, entries, destination)
        self.assertFalse((outside / "file.bin").exists())

    def test_extract_one_refuses_empty_dot_and_root_destinations(self) -> None:
        source = io.BytesIO(b"")
        for destination in ("", ".", "/"):
            with self.subTest(destination=destination):
                with self.assertRaisesRegex(gdf_extract.GdfError, "unsafe.*name"):
                    gdf_extract.extract_one(source, 0, 0, 0, destination)


if __name__ == "__main__":
    unittest.main()
