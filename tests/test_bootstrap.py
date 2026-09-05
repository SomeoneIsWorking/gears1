"""Shipping bootstrap CLI, prerequisite, logging, and lifecycle tests."""

from __future__ import annotations

import os
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT))
sys.path.insert(0, str(REPO_ROOT / "tools"))

import replay_corpus

from tools import clean_build
from tools.gearsue3_bootstrap import (
    environment,
    launcher,
    paths,
    process,
    requirements,
)


class BootstrapTests(unittest.TestCase):
    def setUp(self) -> None:
        (REPO_ROOT / "scratch").mkdir(exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(
            prefix="bootstrap-test-", dir=REPO_ROOT / "scratch"
        )
        self.root = Path(self.temporary.name)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_shipping_arguments_preserve_order_and_runtime_tail(self) -> None:
        options = launcher.parse_arguments(
            [
                "--menu-walk",
                "--script",
                "custom",
                "--headless",
                "--http-port",
                "0",
                "--",
                "--runtime-option",
                "value",
            ],
            "maintained-walk",
        )
        self.assertEqual(options.input_script, "custom")
        self.assertTrue(options.headless)
        self.assertEqual(options.http_port, "0")
        self.assertEqual(options.runtime_arguments, ["--runtime-option", "value"])

    def test_shipping_arguments_refuse_missing_and_invalid_values(self) -> None:
        with self.assertRaisesRegex(launcher.CliError, "requires a value"):
            launcher.parse_arguments(["--log"], "walk")
        with self.assertRaisesRegex(launcher.CliError, "integer from 0"):
            launcher.parse_arguments(["--http-port", "70000"], "walk")
        with self.assertRaisesRegex(launcher.CliError, "unknown option"):
            launcher.parse_arguments(["--no-build"], "walk")
        with self.assertRaisesRegex(launcher.CliError, "unknown option"):
            launcher.parse_arguments(["--typo"], "walk")

    def test_launch_environment_propagates_existing_and_explicit_values(self) -> None:
        base = {"KEPT": "yes", "GEARS_PRESENT_DUMP_AT": "42"}
        options = launcher.LaunchOptions(
            headless=True,
            input_script="f10:A",
            http_port="1234",
            present_dump="2",
        )
        environment = launcher._launch_environment(base, options, self.root)
        self.assertEqual(environment["KEPT"], "yes")
        self.assertEqual(environment["GEARS_NO_WINDOW"], "1")
        self.assertEqual(environment["GEARS_INPUT_SCRIPT"], "f10:A")
        self.assertEqual(environment["GEARS_DEBUG_HTTP_PORT"], "1234")
        self.assertEqual(environment["GEARS_PRESENT_DUMP"], "2")
        self.assertEqual(environment["GEARS_PRESENT_DUMP_AT"], "42")

    def test_missing_tools_name_every_missing_command_and_package_action(self) -> None:
        available = {"git", "cc", "c++"}
        with self.assertRaises(requirements.RequirementError) as caught:
            requirements.require_commands(
                {}, lambda name: name if name in available else None
            )
        message = str(caught.exception)
        for name in ("cmake", "ninja", "make", "pkg-config"):
            self.assertIn(name, message)
        self.assertIn("Install them with", message)

    def test_platform_package_commands_are_exact(self) -> None:
        self.assertEqual(
            requirements.package_command("Linux", "fedora"),
            "sudo dnf install cmake ninja-build make pkgconf-pkg-config gcc gcc-c++ SDL3-devel vulkan-loader-devel vulkan-headers",
        )
        self.assertIn(
            "sudo apt install", requirements.package_command("Linux", "ubuntu")
        )
        self.assertIn("brew install", requirements.package_command("Darwin", ""))
        self.assertIn("winget install", requirements.package_command("Windows", ""))

    def test_archive_tool_is_required_only_for_7z_inputs(self) -> None:
        with self.assertRaisesRegex(requirements.RequirementError, "7z"):
            requirements.require_archive_command(
                self.root / "disc.7z", lambda name: None
            )
        requirements.require_archive_command(self.root / "disc.iso", lambda name: None)
        self.assertIn(
            "7zip",
            requirements.package_command("Linux", "fedora", include_archive_tools=True),
        )

    def test_dotenv_is_data_not_shell_and_process_values_win(self) -> None:
        env_file = self.root / ".env"
        env_file.write_text(
            "GEARS_ISO='disc image.iso'\n"
            "IGNORED=$(touch should-never-exist)\n"
            "GEARS_BUILD_DIR=build/from-file\n",
            encoding="utf-8",
        )
        loaded = environment.load_environment(
            self.root,
            {"GEARS_BUILD_DIR": "build/from-process"},
            env_file,
        )
        self.assertEqual(loaded["GEARS_ISO"], "disc image.iso")
        self.assertEqual(loaded["GEARS_BUILD_DIR"], "build/from-process")
        self.assertNotIn("IGNORED", loaded)
        self.assertFalse((self.root / "should-never-exist").exists())

    def test_selected_environment_file_is_the_only_dotenv_input(self) -> None:
        selected = environment.environment_file(
            self.root, {"GEARS_ENV_FILE": "chosen.env"}
        )
        self.assertEqual(selected, self.root / "chosen.env")

    def test_launcher_uses_selected_environment_file_for_preparation(self) -> None:
        selected = self.root / "selected.env"
        with (
            patch.object(launcher, "environment_file", return_value=selected),
            patch.object(
                launcher,
                "load_profile",
                return_value=SimpleNamespace(
                    display_name="test", navigation=SimpleNamespace(menu_walk="walk")
                ),
            ),
            patch.object(launcher, "load_environment", return_value={}),
            patch.object(
                launcher,
                "parse_arguments",
                return_value=launcher.LaunchOptions(prepare_only=True),
            ),
            patch.object(
                launcher,
                "prepare_title",
                side_effect=launcher.ProvisionError("missing product composition"),
            ) as prepare,
            self.assertRaisesRegex(launcher.ProvisionError, "product composition"),
        ):
            launcher.main(["--prepare"], self.root)
        self.assertEqual(prepare.call_args.kwargs["env_file"], selected)

    def test_build_directory_refuses_scratch_and_external_roots(self) -> None:
        self.assertEqual(
            paths.build_directory(
                self.root, "build/debug", self.root / "build/release"
            ),
            self.root / "build/debug",
        )
        with self.assertRaisesRegex(paths.BuildPathError, "must be a child"):
            paths.build_directory(
                self.root, "scratch/build", self.root / "build/release"
            )
        with self.assertRaisesRegex(paths.BuildPathError, "must be a child"):
            paths.build_directory(
                self.root, self.root.parent, self.root / "build/release"
            )

    def test_diagnostic_integer_is_bounded_and_names_invalid_input(self) -> None:
        self.assertEqual(
            replay_corpus.environment_integer({"LIMIT": "4"}, "LIMIT", 2, minimum=1),
            4,
        )
        with self.assertRaisesRegex(replay_corpus.ReplayCorpusError, "LIMIT.*integer"):
            replay_corpus.environment_integer({"LIMIT": "oops"}, "LIMIT", 2)
        with self.assertRaisesRegex(replay_corpus.ReplayCorpusError, "LIMIT.*1..3"):
            replay_corpus.environment_integer(
                {"LIMIT": "4"}, "LIMIT", 2, minimum=1, maximum=3
            )

    def test_build_cleanup_accepts_only_one_named_top_level_child(self) -> None:
        self.assertEqual(
            clean_build.build_target("verification"), REPO_ROOT / "build/verification"
        )
        for invalid in ("", ".", "../build", "nested/debug", "/absolute"):
            with (
                self.subTest(invalid=invalid),
                self.assertRaises(clean_build.CleanBuildError),
            ):
                clean_build.build_target(invalid)

    def test_logged_child_tees_output_and_preserves_nonzero_status(self) -> None:
        log = self.root / "run.log"
        returncode = process.run_logged_child(
            [sys.executable, "-c", "print('child-output'); raise SystemExit(7)"],
            cwd=REPO_ROOT,
            environ=dict(os.environ),
            log_path=log,
        )
        self.assertEqual(returncode, 7)
        self.assertEqual(log.read_text(encoding="utf-8"), "child-output\n")

    def test_terminating_launcher_terminates_its_direct_child(self) -> None:
        child_pid = self.root / "child.pid"
        log = self.root / "child.log"
        child_code = (
            "import os,time,pathlib; "
            f"pathlib.Path({str(child_pid)!r}).write_text(str(os.getpid())); "
            "time.sleep(60)"
        )
        helper_code = (
            "import os,sys,pathlib; "
            "from tools.gearsue3_bootstrap.process import run_logged_child; "
            "raise SystemExit(run_logged_child("
            f"[sys.executable,'-c',{child_code!r}],"
            f"cwd=pathlib.Path({str(REPO_ROOT)!r}),environ=dict(os.environ),"
            f"log_path=pathlib.Path({str(log)!r})))"
        )
        helper = subprocess.Popen([sys.executable, "-c", helper_code], cwd=REPO_ROOT)
        deadline = time.monotonic() + 5
        while not child_pid.exists() and time.monotonic() < deadline:
            time.sleep(0.02)
        self.assertTrue(child_pid.exists(), "direct child never started")
        pid = int(child_pid.read_text())
        helper.send_signal(signal.SIGTERM)
        helper.wait(timeout=5)
        with self.assertRaises(ProcessLookupError):
            os.kill(pid, 0)

    def test_run_sh_is_only_the_frozen_uv_shim(self) -> None:
        lines = [
            line
            for line in (REPO_ROOT / "run.sh").read_text(encoding="utf-8").splitlines()
            if line and not line.startswith("#!")
        ]
        self.assertEqual(lines[0], "set -eu")
        self.assertEqual(lines[-1], 'exec uv run --frozen python bootstrap.py "$@"')
        self.assertLessEqual(len(lines), 4)


if __name__ == "__main__":
    unittest.main()
