#!/usr/bin/env python3
"""Tests for the tracked title profile and its oracle schedule renderer."""

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

    def test_timed_walk_renders_the_oracle_presses(self) -> None:
        self.assertEqual(
            profile.oracle_timed_schedule("25000:START,25300:,30500:A,30800:"),
            "START@25,A@30.5",
        )
        menu = profile.load_profile(REPO_ROOT).navigation.menu_walk
        self.assertTrue(profile.oracle_timed_schedule(menu).startswith("START@25,A@30,B@35"))

    def test_an_unreplayable_timed_step_is_refused(self) -> None:
        with self.assertRaisesRegex(profile.ProfileError, "only single buttons"):
            profile.oracle_timed_schedule("1000:LY+,2000:")
        with self.assertRaisesRegex(profile.ProfileError, "only single buttons"):
            profile.oracle_timed_schedule("1000:A&B,2000:")
        with self.assertRaisesRegex(profile.ProfileError, "presses nothing"):
            profile.oracle_timed_schedule("1000:")

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
