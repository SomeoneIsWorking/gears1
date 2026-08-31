#!/usr/bin/env python3
"""Cold-path and fail-closed tests for shipping title preparation."""

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT))

from tools import title_identity  # noqa: E402
from tools.gearsue3_bootstrap import profile, provision  # noqa: E402


def identified_document(selected: profile.TitleProfile) -> dict[str, object]:
    return {
        "schema": 1,
        "disc": {"sha256": "a" * 64, "size": 123, "format": "XGD", "partition_offset": 0},
        "xex": {
            "sha256": selected.identity.xex_sha256,
            "size": 12,
            "metadata": {
                "format": "XEX2",
                "execution": {
                    "title_id": selected.identity.title_id,
                    "savegame_id": selected.identity.savegame_id,
                    "platform": selected.identity.platform,
                    "disc_number": selected.identity.disc_number,
                    "disc_count": selected.identity.disc_count,
                },
                "image": {"sha256": selected.identity.image_sha256},
            },
        },
    }


class BootstrapProvisionTests(unittest.TestCase):
    def setUp(self) -> None:
        (REPO_ROOT / "scratch").mkdir(exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(
            prefix="bootstrap-provision-test-", dir=REPO_ROOT / "scratch"
        )
        self.root = Path(self.temporary.name)
        (self.root / ".gitignore").write_text("scratch/\nbuild/\nroms/\n")
        self.disc = self.root / "disc.iso"
        self.disc.write_bytes(b"synthetic disc selected by the cold-path test")
        self.selected = profile.load_profile(REPO_ROOT)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_cold_preparation_composes_every_shipping_stage_in_order(self) -> None:
        calls: list[str] = []
        disc_only = {
            "schema": 1,
            "disc": {"sha256": "a" * 64, "size": self.disc.stat().st_size},
        }
        full_identity = identified_document(self.selected)
        title_root = self.root / "scratch/titles" / ("a" * 64)

        def write_identity(_root: Path, identity: dict[str, object]) -> Path:
            calls.append("identity")
            destination = title_root / "identity.json"
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text("{}\n")
            return destination

        def extract(_image: Path, game_dir: Path) -> None:
            calls.append("extract")
            game_dir.mkdir(parents=True)
            (game_dir / "default.xex").write_bytes(b"synthetic xex")

        def generate(
            _repo: Path,
            generated_root: Path,
            _xex: Path,
            ppc_dir: Path,
            _identity: dict[str, object],
            _profile: profile.TitleProfile,
            _tools: Path,
            _runner: object,
        ) -> None:
            calls.append("generate")
            self.assertEqual(generated_root, title_root)
            ppc_dir.mkdir()
            (ppc_dir / "ppc_config.h").write_text("#pragma once\n")

        def build(
            _repo: Path,
            _game: Path,
            _ppc: Path,
            build_dir: Path,
            _runner: object,
        ) -> Path:
            calls.append("build")
            runtime = build_dir / "runtime/gears1"
            runtime.parent.mkdir(parents=True)
            runtime.write_text("#!/bin/sh\n")
            runtime.chmod(0o755)
            return runtime

        resolved = title_identity.ResolvedImage(self.disc, "explicit")
        with (
            patch.object(provision, "require_commands", side_effect=lambda _env: calls.append("requirements")),
            patch.object(provision.title_identity, "resolve_image", return_value=resolved),
            patch.object(provision.title_identity, "build_identity", return_value=disc_only),
            patch.object(provision.title_identity, "write_identity", side_effect=write_identity),
            patch.object(provision, "_ensure_submodules", side_effect=lambda *_args: calls.append("submodules")),
            patch.object(
                provision,
                "_build_recompiler",
                side_effect=lambda *_args: (
                    calls.append("recompiler"),
                    self.root / "build/deps/xenonrecomp",
                )[1],
            ),
            patch.object(provision, "_extract_disc", side_effect=extract),
            patch.object(provision, "_augment_identity", return_value=full_identity),
            patch.object(provision, "_generate_title_module", side_effect=generate),
            patch.object(provision, "_build_product", side_effect=build),
        ):
            prepared = provision.prepare_title(
                self.root, self.selected, image=self.disc, environ={}
            )

        self.assertEqual(
            calls,
            [
                "requirements",
                "identity",
                "submodules",
                "recompiler",
                "extract",
                "identity",
                "generate",
                "build",
            ],
        )
        self.assertEqual(prepared.game_dir, title_root / "game")
        self.assertEqual(prepared.ppc_dir, title_root / "ppc")
        self.assertEqual(
            prepared.build_dir,
            self.root / "build/titles" / ("a" * 64) / "release",
        )
        self.assertFalse((self.root / "scratch/build").exists())

    def test_wrong_title_profile_is_refused_with_every_mismatch_named(self) -> None:
        identity = identified_document(self.selected)
        execution = identity["xex"]["metadata"]["execution"]  # type: ignore[index]
        execution["title_id"] = "00000000"  # type: ignore[index]
        execution["disc_number"] = 2  # type: ignore[index]
        with self.assertRaisesRegex(
            provision.ProvisionError, "title_id.*disc_number"
        ):
            provision._validate_title(identity, self.selected)

    def test_missing_image_refuses_before_submodule_or_build_work(self) -> None:
        with (
            patch.object(provision, "require_commands"),
            patch.object(
                provision,
                "_ensure_submodules",
                side_effect=AssertionError("submodules must not initialize"),
            ),
            patch.object(
                provision,
                "_build_recompiler",
                side_effect=AssertionError("recompiler must not build"),
            ),
        ):
            with self.assertRaisesRegex(provision.ProvisionError, "roms"):
                provision.prepare_title(
                    self.root,
                    self.selected,
                    environ={},
                    env_file=self.root / "missing.env",
                )

    def test_unknown_revision_is_refused_after_title_metadata_matches(self) -> None:
        identity = identified_document(self.selected)
        xex = identity["xex"]
        xex["sha256"] = "0" * 64  # type: ignore[index]
        metadata = xex["metadata"]  # type: ignore[index]
        metadata["image"]["sha256"] = "1" * 64  # type: ignore[index]
        with self.assertRaisesRegex(
            provision.ProvisionError, "unsupported.*xex_sha256.*image_sha256"
        ):
            provision._validate_title(identity, self.selected)

    def test_generated_config_replaces_only_the_three_path_owners(self) -> None:
        destination = self.root / "scratch/titles/key/config/gears.toml"
        executable = self.root / "scratch/titles/key/game/default.xex"
        ppc_dir = self.root / "scratch/titles/key/ppc"
        switches = self.root / "scratch/titles/key/config/switches.toml"
        provision._materialize_recompiler_config(
            REPO_ROOT / self.selected.recompiler_template,
            destination,
            executable,
            ppc_dir,
            switches,
        )
        rendered = destination.read_text(encoding="utf-8")
        self.assertIn('file_path = "../game/default.xex"', rendered)
        self.assertIn('out_directory_path = "../ppc"', rendered)
        self.assertIn('switch_table_file_path = "switches.toml"', rendered)
        self.assertIn("restgprlr_14_address = 0x828D2830", rendered)


if __name__ == "__main__":
    unittest.main()
