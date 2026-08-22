#!/usr/bin/env python3
"""Synthetic stdlib tests for the clean disc-identity provisioning boundary."""

from __future__ import annotations

import hashlib
import importlib.util
import io
import json
import struct
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
TOOLS = REPO_ROOT / "tools"
SPEC = importlib.util.spec_from_file_location("title_identity", TOOLS / "title_identity.py")
assert SPEC is not None and SPEC.loader is not None
title_identity = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = title_identity
SPEC.loader.exec_module(title_identity)


def synthetic_xex(path: Path, execution_values: tuple[int, ...] | None = None) -> None:
    execution_offset = 0x40
    header_size = execution_offset + title_identity.XEX_EXECUTION_INFO_SIZE
    header = bytearray(header_size)
    header[:24] = struct.pack(
        ">4s5I", title_identity.XEX_MAGIC, 0x10, header_size, 0, 0x30, 1
    )
    header[24:32] = struct.pack(
        ">2I", title_identity.XEX_HEADER_EXECUTION_INFO, execution_offset
    )
    if execution_values is not None:
        header[execution_offset:execution_offset + 24] = struct.pack(
            ">4I4BI", *execution_values
        )
    else:
        header[24:32] = struct.pack(">2I", 0x00010201, 0x10000000)
    path.write_bytes(header)


def synthetic_disc(path: Path, payload: bytes = b"") -> None:
    descriptor = title_identity.XGD_VOLUME_DESCRIPTOR_SECTOR * title_identity.XGD_SECTOR_SIZE
    image = bytearray(descriptor + len(title_identity.XGD_VOLUME_MAGIC) + len(payload))
    image[descriptor:descriptor + len(title_identity.XGD_VOLUME_MAGIC)] = (
        title_identity.XGD_VOLUME_MAGIC
    )
    if payload:
        image[-len(payload):] = payload
    path.write_bytes(image)


class TitleIdentityTests(unittest.TestCase):
    def setUp(self) -> None:
        scratch = REPO_ROOT / "scratch"
        scratch.mkdir(exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=scratch)
        self.root = Path(self.temporary.name)
        (self.root / ".gitignore").write_text(".env\nscratch/\nroms/\n")
        (self.root / "roms").mkdir()

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def image(self, name: str, data: bytes) -> Path:
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return path

    def test_resolution_priority_is_explicit_environment_dotenv_drop_in(self) -> None:
        explicit = self.image("explicit.iso", b"explicit")
        environment = self.image("environment.iso", b"environment")
        dotenv = self.image("dotenv.iso", b"dotenv")
        drop_in = self.image("roms/drop.iso", b"drop-in")
        (self.root / ".env").write_text(f'{title_identity.ENV_IMAGE}="{dotenv}"\n')

        selected = title_identity.resolve_image(
            explicit,
            self.root,
            {title_identity.ENV_IMAGE: str(environment)},
            current_directory=self.root,
        )
        self.assertEqual(selected.path, explicit.resolve())
        self.assertEqual(selected.source, "explicit")

        selected = title_identity.resolve_image(
            None, self.root, {title_identity.ENV_IMAGE: str(environment)}
        )
        self.assertEqual(selected.path, environment.resolve())
        self.assertEqual(selected.source, "environment")

        selected = title_identity.resolve_image(None, self.root, {})
        self.assertEqual(selected.path, dotenv.resolve())
        self.assertEqual(selected.source, "dotenv")

        (self.root / ".env").unlink()
        selected = title_identity.resolve_image(None, self.root, {})
        self.assertEqual(selected.path, drop_in.resolve())
        self.assertEqual(selected.source, "drop-in")

    def test_invalid_selected_path_never_falls_back(self) -> None:
        self.image("roms/drop.iso", b"drop-in")
        with self.assertRaisesRegex(title_identity.IdentityError, "does not exist"):
            title_identity.resolve_image(
                "missing.iso", self.root, {}, current_directory=self.root
            )
        with self.assertRaisesRegex(title_identity.IdentityError, "does not exist"):
            title_identity.resolve_image(
                None,
                self.root,
                {title_identity.ENV_IMAGE: "missing.iso"},
            )

    def test_ambiguous_and_unknown_drop_ins_are_refused(self) -> None:
        self.image("roms/a.iso", b"a")
        self.image("roms/b.xiso", b"b")
        with self.assertRaisesRegex(title_identity.IdentityError, "ambiguous"):
            title_identity.resolve_image(None, self.root, {})

        (self.root / "roms" / "a.iso").unlink()
        (self.root / "roms" / "b.xiso").unlink()
        self.image("roms/readme.txt", b"not an image")
        with self.assertRaisesRegex(title_identity.IdentityError, "no supported"):
            title_identity.resolve_image(None, self.root, {})

    def test_duplicate_and_malformed_dotenv_values_are_refused(self) -> None:
        env_file = self.root / ".env"
        env_file.write_text(f"{title_identity.ENV_IMAGE}=a.iso\n{title_identity.ENV_IMAGE}=b.iso\n")
        with self.assertRaisesRegex(title_identity.IdentityError, "more than once"):
            title_identity.resolve_image(None, self.root, {})
        env_file.write_text(f'{title_identity.ENV_IMAGE}="unterminated\n')
        with self.assertRaisesRegex(title_identity.IdentityError, "unterminated"):
            title_identity.resolve_image(None, self.root, {})

    def test_fingerprint_streams_complete_file(self) -> None:
        data = bytes(range(251)) * 10000
        image = self.image("large.iso", data)
        result = title_identity.fingerprint(image)
        self.assertEqual(result["size"], len(data))
        self.assertEqual(result["sha256"], hashlib.sha256(data).hexdigest())

    def test_unknown_disc_content_is_refused(self) -> None:
        unknown = self.image("unknown.iso", b"not an Xbox disc")
        with self.assertRaisesRegex(title_identity.IdentityError, "unknown disc"):
            title_identity.build_identity(unknown)

    def test_xex_metadata_is_factual_and_path_free(self) -> None:
        values = (
            0x01020304,
            0x05060708,
            0x090A0B0C,
            0x0D0E0F10,
            1,
            2,
            1,
            1,
            0x11121314,
        )
        xex = self.root / "default.xex"
        synthetic_xex(xex, values)
        metadata = title_identity.parse_xex_metadata(xex)
        self.assertEqual(metadata["format"], "XEX2")
        self.assertEqual(metadata["execution"]["media_id"], "0x01020304")
        self.assertEqual(metadata["execution"]["title_id"], "0x0D0E0F10")
        self.assertEqual(metadata["execution"]["disc_count"], 1)
        self.assertNotIn(str(self.root), json.dumps(metadata))

    def test_unknown_or_incomplete_xex_is_refused(self) -> None:
        unknown = self.image("default.xex", b"not-xex")
        with self.assertRaisesRegex(title_identity.IdentityError, "unknown"):
            title_identity.parse_xex_metadata(unknown)
        synthetic_xex(unknown, None)
        with self.assertRaisesRegex(title_identity.IdentityError, "missing"):
            title_identity.parse_xex_metadata(unknown)
        wrong_name = self.image("other.xex", b"XEX2")
        with self.assertRaisesRegex(title_identity.IdentityError, "must be named"):
            title_identity.parse_xex_metadata(wrong_name)

    def test_manifest_is_deterministic_isolated_and_can_be_enriched(self) -> None:
        disc = self.root / "disc.iso"
        synthetic_disc(disc, b"synthetic-fixture")
        values = (1, 2, 3, 4, 5, 6, 1, 1, 7)
        xex = self.root / "default.xex"
        synthetic_xex(xex, values)

        disc_only = title_identity.build_identity(disc)
        destination = title_identity.write_identity(self.root, disc_only)
        expected_parent = self.root / "scratch" / "titles" / disc_only["disc"]["sha256"]
        self.assertEqual(destination.parent, expected_parent)
        self.assertEqual(list(self.root.glob("scratch/titles/*/identity.json")), [destination])

        complete = title_identity.build_identity(disc, xex)
        destination_again = title_identity.write_identity(self.root, complete)
        self.assertEqual(destination_again, destination)
        stored = json.loads(destination.read_text())
        self.assertEqual(stored, complete)
        self.assertEqual(stored["disc"]["format"], "XGD")
        self.assertEqual(stored["disc"]["partition_offset"], 0)
        encoded = destination.read_text()
        self.assertNotIn(str(disc), encoded)
        self.assertNotIn(str(xex), encoded)
        self.assertNotIn("timestamp", encoded.lower())
        title_identity.write_identity(self.root, complete)
        self.assertEqual(destination.read_text(), encoded)

    def test_conflicting_xex_for_same_disc_is_refused(self) -> None:
        disc = self.root / "disc.iso"
        synthetic_disc(disc, b"conflict-fixture")
        first = self.root / "first" / "default.xex"
        second = self.root / "second" / "default.xex"
        first.parent.mkdir()
        second.parent.mkdir()
        synthetic_xex(first, (1, 2, 3, 4, 5, 6, 1, 1, 7))
        synthetic_xex(second, (8, 9, 10, 11, 12, 13, 1, 1, 14))
        title_identity.write_identity(self.root, title_identity.build_identity(disc, first))
        with self.assertRaisesRegex(title_identity.IdentityError, "different default.xex"):
            title_identity.write_identity(
                self.root, title_identity.build_identity(disc, second)
            )

    def test_unignored_scratch_refuses_before_creating_output(self) -> None:
        disc = self.root / "disc.iso"
        synthetic_disc(disc, b"ignore-fixture")
        (self.root / ".gitignore").write_text(".env\n")
        with self.assertRaisesRegex(title_identity.IdentityError, "does not ignore"):
            title_identity.write_identity(
                self.root, title_identity.build_identity(disc)
            )
        self.assertFalse((self.root / "scratch").exists())

    def test_cli_outputs_json_without_machine_paths(self) -> None:
        disc = self.root / "disc.iso"
        synthetic_disc(disc, b"cli-fixture")
        output = io.StringIO()
        errors = io.StringIO()
        with redirect_stdout(output), redirect_stderr(errors):
            result = title_identity.main(
                [str(disc), "--repo-root", str(self.root)]
            )
        self.assertEqual(result, 0)
        self.assertGreater(json.loads(output.getvalue())["disc"]["size"], 4)
        self.assertNotIn(str(disc), output.getvalue())
        self.assertIn("scratch/titles/", errors.getvalue())


if __name__ == "__main__":
    unittest.main()
