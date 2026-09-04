#!/usr/bin/env python3
"""Tests for the tracked title profile and its two schedule renderers."""

from __future__ import annotations

import os
import sys
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "tools"))

from gearsue3_bootstrap import profile  # noqa: E402


class BootstrapProfileTests(unittest.TestCase):
    def test_repository_profile_is_strict_and_path_portable(self) -> None:
        selected = profile.load_profile(REPO_ROOT)
        self.assertEqual(selected.key, "gears1")
        self.assertEqual(selected.identity.title_id, "4d5307d5")
        self.assertEqual(selected.identity.savegame_id, "00000000")
        self.assertEqual(selected.identity.platform, 0)
        self.assertEqual(
            selected.identity.image_sha256,
            "f61cc78e4057bc68a2c65386a0341f6d26a7add3dfd9918007a455750ec6ed5c",
        )

    def test_frame_table_renders_both_shipping_notations(self) -> None:
        navigation = profile.load_profile(REPO_ROOT).navigation
        native = profile.native_schedule(navigation, "90:START~120 600:A 700:LY+ 800:LY0")
        oracle = profile.oracle_schedule(navigation, "90:START~120 600:A 700:LY+ 800:LY0")
        self.assertIn("f90:START", native)
        self.assertIn("f210:", native)
        self.assertIn("START@90", oracle)
        self.assertIn("START@202", oracle)
        self.assertIn("A@600", oracle)
        self.assertIn("LY+@700", oracle)
        self.assertEqual(profile.last_frame(navigation, "90:START~120 600:A"), 600)

    def test_invalid_table_is_refused_instead_of_silently_skipped(self) -> None:
        with self.assertRaisesRegex(profile.ProfileError, "invalid frame-walk action"):
            profile.parse_frame_walk("90:START 100:TYPO")
        with self.assertRaisesRegex(profile.ProfileError, "unique and ordered"):
            profile.parse_frame_walk("100:A 90:B")

    def test_profile_contains_no_machine_path(self) -> None:
        contents = (REPO_ROOT / "config/titles/gears1.toml").read_text()
        self.assertNotIn(str(Path.home()), contents)
        self.assertNotIn(os.environ.get("USER", "__unset__"), contents)

    def test_platform_accepts_the_xex_unsigned_byte_domain(self) -> None:
        self.assertEqual(profile._unsigned_byte({"platform": 0}, "platform", "identity"), 0)
        for invalid in (-1, 256, True, "0"):
            with self.subTest(invalid=invalid):
                with self.assertRaisesRegex(profile.ProfileError, "unsigned byte"):
                    profile._unsigned_byte({"platform": invalid}, "platform", "identity")


if __name__ == "__main__":
    unittest.main()
