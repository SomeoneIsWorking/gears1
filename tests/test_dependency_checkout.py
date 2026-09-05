"""Exact and clean Git dependency contract tests."""

from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "tools"))

from gearsue3_bootstrap.dependency_checkout import (
    DependencyCheckoutError,
    require_git_checkout,
)


class DependencyCheckoutTests(unittest.TestCase):
    def setUp(self) -> None:
        scratch = REPO_ROOT / "scratch"
        scratch.mkdir(exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(
            prefix="dependency-checkout-test-", dir=scratch
        )
        self.checkout = Path(self.temporary.name)
        self.run_git("init", "--quiet", "--initial-branch=main")
        self.run_git("config", "user.name", "Gears dependency test")
        self.run_git("config", "user.email", "dependency-test@example.invalid")
        (self.checkout / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.20)\n", encoding="utf-8"
        )
        self.run_git("add", "CMakeLists.txt")
        self.run_git("commit", "--quiet", "-m", "fixture")
        self.revision = self.run_git("rev-parse", "HEAD").stdout.strip()

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def run_git(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            ["git", "-C", self.checkout, *arguments],
            check=True,
            capture_output=True,
            text=True,
        )

    def test_clean_exact_checkout_is_accepted(self) -> None:
        resolved = require_git_checkout(
            self.checkout,
            self.revision,
            "fixture",
            required_file=Path("CMakeLists.txt"),
        )
        self.assertEqual(resolved.root, self.checkout.resolve())
        self.assertEqual(resolved.revision, self.revision)

    def test_wrong_revision_is_refused_with_both_revisions(self) -> None:
        wrong_revision = "0" * 40
        with self.assertRaisesRegex(
            DependencyCheckoutError, f"{wrong_revision}.*{self.revision}"
        ):
            require_git_checkout(self.checkout, wrong_revision, "fixture")

    def test_dirty_checkout_is_refused_with_the_dirty_entry(self) -> None:
        (self.checkout / "dirty.txt").write_text("dirty\n", encoding="utf-8")
        with self.assertRaisesRegex(
            DependencyCheckoutError, r"dirty entries:\n\?\? dirty.txt"
        ):
            require_git_checkout(self.checkout, self.revision, "fixture")


if __name__ == "__main__":
    unittest.main()
