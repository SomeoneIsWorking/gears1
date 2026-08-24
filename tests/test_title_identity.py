#!/usr/bin/env python3
"""Synthetic stdlib tests for the clean disc-identity provisioning boundary."""

from __future__ import annotations

import hashlib
import importlib.util
import io
import json
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
TOOLS = REPO_ROOT / "tools"
SPEC = importlib.util.spec_from_file_location(
    "title_identity", TOOLS / "title_identity.py"
)
assert SPEC is not None and SPEC.loader is not None
title_identity = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = title_identity
SPEC.loader.exec_module(title_identity)


def inspection_document(
    xex: bytes, image: bytes = b"normalized-image"
) -> dict[str, object]:
    base = 0x82000000
    return {
        "schema": title_identity.XEX_INSPECT_SCHEMA,
        "format": "XEX2",
        "xex": {
            "sha256": hashlib.sha256(xex).hexdigest(),
            "size": len(xex),
        },
        "execution": {
            "title_id": "415607d5",
            "media_id": "5a20a6d4",
            "version": "00010000",
            "base_version": "00010000",
            "platform": 2,
            "executable_table": 0,
            "disc_number": 1,
            "disc_count": 1,
            "savegame_id": "415607d5",
        },
        "image": {
            "sha256": hashlib.sha256(image).hexdigest(),
            "base": f"{base:08x}",
            "size": len(image),
            "entry": f"{base:08x}",
        },
        "sections": [
            {
                "name": ".text",
                "base": f"{base:08x}",
                "size": len(image),
                "code": True,
            }
        ],
        "imports": [],
        "helpers": {name: [] for name in title_identity.XEX_INSPECT_HELPERS},
    }


def synthetic_disc(path: Path, payload: bytes = b"") -> None:
    descriptor = (
        title_identity.XGD_VOLUME_DESCRIPTOR_SECTOR * title_identity.XGD_SECTOR_SIZE
    )
    image = bytearray(descriptor + len(title_identity.XGD_VOLUME_MAGIC) + len(payload))
    image[descriptor : descriptor + len(title_identity.XGD_VOLUME_MAGIC)] = (
        title_identity.XGD_VOLUME_MAGIC
    )
    if payload:
        image[-len(payload) :] = payload
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

    def fake_inspector(
        self,
        document: dict[str, object] | str,
        *,
        image: bytes = b"normalized-image",
        returncode: int = 0,
        stderr: str = "",
        write_image: bool = True,
    ) -> tuple[Path, object]:
        executable = self.image("bin/xex-inspect", b"#!/usr/bin/env python3\n")
        executable.chmod(0o755)

        def runner(
            command: list[Path | str], **kwargs: object
        ) -> subprocess.CompletedProcess[str]:
            self.assertEqual(command[2], "--image-out")
            self.assertEqual(kwargs["capture_output"], True)
            self.assertEqual(kwargs["check"], False)
            self.assertEqual(kwargs["text"], True)
            if write_image:
                Path(command[3]).write_bytes(image)
            stdout = document if isinstance(document, str) else json.dumps(document)
            return subprocess.CompletedProcess(command, returncode, stdout, stderr)

        return executable, runner

    def identified_xex(
        self, path: Path, image: bytes = b"normalized-image"
    ) -> dict[str, object]:
        document = inspection_document(path.read_bytes(), image)
        executable, runner = self.fake_inspector(document, image=image)
        return title_identity.build_identity(
            self.root / "disc.iso",
            path,
            repo_root=self.root,
            xex_inspect=executable,
            environ={},
            runner=runner,
        )

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
        env_file.write_text(
            f"{title_identity.ENV_IMAGE}=a.iso\n{title_identity.ENV_IMAGE}=b.iso\n"
        )
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
        xex = self.image("default.xex", b"synthetic XEX container")
        document = inspection_document(xex.read_bytes())
        executable, runner = self.fake_inspector(document)
        metadata = title_identity.parse_xex_metadata(
            xex,
            repo_root=self.root,
            xex_inspect=executable,
            environ={},
            runner=runner,
        )
        self.assertEqual(metadata["format"], "XEX2")
        self.assertEqual(metadata["execution"]["media_id"], "5a20a6d4")
        self.assertEqual(metadata["execution"]["title_id"], "415607d5")
        self.assertEqual(metadata["execution"]["disc_count"], 1)
        self.assertEqual(
            metadata["image"]["sha256"], hashlib.sha256(b"normalized-image").hexdigest()
        )
        self.assertNotIn("xex", metadata)
        self.assertNotIn("schema", metadata)
        self.assertNotIn(str(self.root), json.dumps(metadata))

    def test_missing_and_nonzero_xex_inspector_are_refused(self) -> None:
        xex = self.image("default.xex", b"synthetic XEX container")
        with self.assertRaisesRegex(title_identity.IdentityError, "does not exist"):
            title_identity.parse_xex_metadata(
                xex,
                repo_root=self.root,
                xex_inspect="missing/xex-inspect",
                environ={},
            )
        executable, runner = self.fake_inspector(
            "", returncode=2, stderr="malformed XEX"
        )
        with self.assertRaisesRegex(
            title_identity.IdentityError, "refused.*malformed XEX"
        ):
            title_identity.parse_xex_metadata(
                xex,
                repo_root=self.root,
                xex_inspect=executable,
                environ={},
                runner=runner,
            )

    def test_invalid_json_and_schema_are_refused(self) -> None:
        xex = self.image("default.xex", b"synthetic XEX container")
        executable, runner = self.fake_inspector("not JSON")
        with self.assertRaisesRegex(title_identity.IdentityError, "invalid JSON"):
            title_identity.parse_xex_metadata(
                xex,
                repo_root=self.root,
                xex_inspect=executable,
                environ={},
                runner=runner,
            )
        document = inspection_document(xex.read_bytes())
        del document["execution"]
        executable, runner = self.fake_inspector(document)
        with self.assertRaisesRegex(title_identity.IdentityError, "missing execution"):
            title_identity.parse_xex_metadata(
                xex,
                repo_root=self.root,
                xex_inspect=executable,
                environ={},
                runner=runner,
            )

    def test_mismatched_and_ambiguous_metadata_are_refused(self) -> None:
        xex = self.image("default.xex", b"synthetic XEX container")
        document = inspection_document(b"different container")
        executable, runner = self.fake_inspector(document)
        with self.assertRaisesRegex(title_identity.IdentityError, "does not identify"):
            title_identity.parse_xex_metadata(
                xex,
                repo_root=self.root,
                xex_inspect=executable,
                environ={},
                runner=runner,
            )

        valid = json.dumps(inspection_document(xex.read_bytes()))
        duplicate = valid.replace('"schema": 1', '"schema": 1, "schema": 1', 1)
        executable, runner = self.fake_inspector(duplicate)
        with self.assertRaisesRegex(
            title_identity.IdentityError, "ambiguous duplicate"
        ):
            title_identity.parse_xex_metadata(
                xex,
                repo_root=self.root,
                xex_inspect=executable,
                environ={},
                runner=runner,
            )

    def test_missing_or_mismatched_emitted_image_is_refused(self) -> None:
        xex = self.image("default.xex", b"synthetic XEX container")
        document = inspection_document(xex.read_bytes())
        executable, runner = self.fake_inspector(document, write_image=False)
        with self.assertRaisesRegex(title_identity.IdentityError, "does not exist"):
            title_identity.parse_xex_metadata(
                xex,
                repo_root=self.root,
                xex_inspect=executable,
                environ={},
                runner=runner,
            )
        executable, runner = self.fake_inspector(document, image=b"different image")
        with self.assertRaisesRegex(
            title_identity.IdentityError, "does not identify its emitted image"
        ):
            title_identity.parse_xex_metadata(
                xex,
                repo_root=self.root,
                xex_inspect=executable,
                environ={},
                runner=runner,
            )

    def test_xex_input_name_and_environment_override_are_strict(self) -> None:
        xex = self.image("default.xex", b"synthetic XEX container")
        document = inspection_document(xex.read_bytes())
        executable, runner = self.fake_inspector(document)
        metadata = title_identity.parse_xex_metadata(
            xex,
            repo_root=self.root,
            environ={title_identity.XEX_INSPECT_ENV: str(executable)},
            runner=runner,
        )
        self.assertEqual(metadata["format"], "XEX2")
        wrong_name = self.image("other.xex", b"XEX2")
        with self.assertRaisesRegex(title_identity.IdentityError, "must be named"):
            title_identity.parse_xex_metadata(
                wrong_name,
                repo_root=self.root,
                xex_inspect=executable,
                environ={},
                runner=runner,
            )

    def test_manifest_is_deterministic_isolated_and_can_be_enriched(self) -> None:
        disc = self.root / "disc.iso"
        synthetic_disc(disc, b"synthetic-fixture")
        xex = self.image("default.xex", b"first synthetic XEX")

        disc_only = title_identity.build_identity(disc)
        destination = title_identity.write_identity(self.root, disc_only)
        expected_parent = self.root / "scratch" / "titles" / disc_only["disc"]["sha256"]
        self.assertEqual(destination.parent, expected_parent)
        self.assertEqual(
            list(self.root.glob("scratch/titles/*/identity.json")), [destination]
        )

        complete = self.identified_xex(xex)
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
        first.write_bytes(b"first synthetic XEX")
        second.write_bytes(b"second synthetic XEX")
        title_identity.write_identity(self.root, self.identified_xex(first))
        with self.assertRaisesRegex(
            title_identity.IdentityError, "different default.xex"
        ):
            title_identity.write_identity(self.root, self.identified_xex(second))

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
            result = title_identity.main([str(disc), "--repo-root", str(self.root)])
        self.assertEqual(result, 0)
        self.assertGreater(json.loads(output.getvalue())["disc"]["size"], 4)
        self.assertNotIn(str(disc), output.getvalue())
        self.assertIn("scratch/titles/", errors.getvalue())


if __name__ == "__main__":
    unittest.main()
