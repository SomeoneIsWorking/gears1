"""Shipping bootstrap CLI, prerequisite, logging, and lifecycle tests."""

from __future__ import annotations

import contextlib
import hashlib
import io
import json
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
sys.path.insert(0, str(REPO_ROOT / "tests"))

import test_gdf_extract as gdf_fixture

from tools import clean_build, run_offscreen
from tools.gearsue3_bootstrap import (
    crash_triage,
    environment,
    launcher,
    paths,
    process,
    profile,
    provision,
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

    def test_shipping_arguments_select_only_the_disc_and_preparation(self) -> None:
        options = launcher.parse_arguments(["--iso", "disc.iso", "--prepare"])
        self.assertEqual(options.image, "disc.iso")
        self.assertTrue(options.prepare_only)
        self.assertEqual(launcher.parse_arguments([]), launcher.LaunchOptions())

    def test_shipping_arguments_refuse_maintainer_options_and_missing_values(self) -> None:
        with self.assertRaisesRegex(launcher.CliError, "requires a value"):
            launcher.parse_arguments(["--iso"])
        for maintainer_only in ("--headless", "--script", "--menu-walk", "--http-port"):
            with (
                self.subTest(option=maintainer_only),
                self.assertRaisesRegex(launcher.CliError, "unknown option"),
            ):
                launcher.parse_arguments([maintainer_only])

    def test_launcher_executes_the_prepared_product_command(self) -> None:
        prepared = provision.PreparedTitle(
            self.root / "gears1", self.root / "disc.iso", "4d5307d5"
        )
        executed: list[list[str]] = []

        def execute(command: list[str]) -> None:
            executed.append(command)
            raise SystemExit(0)

        with (
            patch.object(launcher, "load_profile"),
            patch.object(launcher, "load_environment", return_value={}),
            patch.object(launcher, "prepare_title", return_value=prepared),
            self.assertRaises(SystemExit),
        ):
            launcher.main([], self.root, execute)
        self.assertEqual(
            executed,
            [[str(self.root / "gears1"), "--image", str(self.root / "disc.iso"),
              "--title-id", "4d5307d5"]],
        )

    def test_disc_is_identified_by_its_default_xex(self) -> None:
        executable = b"XEX2" + bytes(range(256)) * 20
        disc = gdf_fixture.image(
            gdf_fixture.table({0: gdf_fixture.entry("default.xex", start=48, size=len(executable))}),
            {48: executable},
        )
        expected = hashlib.sha256(executable).hexdigest()
        self.assertEqual(provision.disc_executable_digest(disc), expected)
        image = self.root / "disc.iso"
        image.write_bytes(disc.getvalue())
        provision.authenticate_image(image, self._profile(expected))
        with self.assertRaisesRegex(provision.ProvisionError, "not the supported"):
            provision.authenticate_image(image, self._profile("0" * 64))

    def test_disc_without_one_default_xex_is_refused(self) -> None:
        disc = gdf_fixture.image(
            gdf_fixture.table({0: gdf_fixture.entry("other.xex", start=48, size=4)}),
            {48: b"XEX2"},
        )
        with self.assertRaisesRegex(provision.ProvisionError, "0 root default.xex"):
            provision.disc_executable_digest(disc)
        image = self.root / "not-a-disc.iso"
        image.write_bytes(bytes(4096))
        with self.assertRaisesRegex(provision.ProvisionError, "not a readable"):
            provision.authenticate_image(image, self._profile("0" * 64))

    @staticmethod
    def _profile(xex_sha256: str) -> SimpleNamespace:
        return SimpleNamespace(
            display_name="Gears of War", identity=SimpleNamespace(xex_sha256=xex_sha256)
        )

    def test_missing_pkg_config_modules_are_refused_by_name(self) -> None:
        with self.assertRaises(requirements.RequirementError) as caught:
            requirements.require_pkg_config_modules(
                ("gtk+-3.0", "sdl2", "liblz4"), lambda module: module == "sdl2"
            )
        # The refusal names exactly the missing modules; the install hint that
        # follows may name every product module on a host with no known distribution.
        refusal = str(caught.exception).splitlines()[0]
        self.assertEqual(refusal, "missing development files for: gtk+-3.0, liblz4")
        requirements.require_pkg_config_modules(("sdl2",), lambda module: True)

    def test_timed_walks_accept_chords_and_refuse_malformed_steps(self) -> None:
        profile._validate_timed_walk("1000:START,1300:,2000:LY+&RX-,2500:,3000:LT&RT,3300:", "a walk")
        profile._validate_timed_walk("1000:DOWN&RB,1300:,2000:BACK&LTHUMB,2300:,3000:UP,3300:", "a walk")
        for invalid in ("1000:LY+&", "1000:ZZ", "1000:LT+", "1000:DWN", "1000:MB", "2000:A,1000:", "1000:A,1000:"):
            with self.subTest(invalid=invalid), self.assertRaises(profile.ProfileError):
                profile._validate_timed_walk(invalid, "a walk")
        self.assertIn("&", profile.load_profile(REPO_ROOT).navigation.gameplay_walk)

    def test_offscreen_walks_come_from_the_profile(self) -> None:
        navigation = SimpleNamespace(
            start_walk="start",
            menu_walk="menu",
            checkpoint_walk="checkpoint",
            gameplay_walk="gameplay",
        )
        self.assertEqual(run_offscreen.walk_script(navigation, "menu"), "menu")
        self.assertEqual(run_offscreen.walk_script(navigation, "gameplay"), "gameplay")
        self.assertEqual(run_offscreen.walk_script(navigation, "none"), "")
        with self.assertRaisesRegex(ValueError, "unknown walk"):
            run_offscreen.walk_script(navigation, "boss-fight")

    def test_missing_tools_name_every_missing_command_and_package_action(self) -> None:
        available = {"git", "cc", "c++"}
        with (
            patch.object(requirements.platform, "system", return_value="Linux"),
            self.assertRaises(requirements.RequirementError) as caught,
        ):
            requirements.require_commands(
                {}, lambda name: name if name in available else None
            )
        message = str(caught.exception)
        for name in ("cmake", "ninja", "pkg-config"):
            self.assertIn(name, message)
        self.assertIn("Install them with", message)

    def test_platform_package_commands_are_exact(self) -> None:
        self.assertEqual(
            requirements.package_command("Linux", "fedora"),
            "sudo dnf install cmake ninja-build pkgconf-pkg-config gcc gcc-c++ "
            "gtk3-devel SDL2-devel lz4-devel libX11-devel fontconfig-devel",
        )
        self.assertEqual(
            requirements.package_command("Linux", "ubuntu"),
            "sudo apt install cmake ninja-build pkg-config g++ libgtk-3-dev libsdl2-dev "
            "liblz4-dev libx11-xcb-dev libfontconfig-dev",
        )
        self.assertIn("using your package manager", requirements.package_command("Darwin"))
        requirements.require_supported_host("Linux")
        for host in ("Darwin", "Windows"):
            with (
                self.subTest(host=host),
                self.assertRaisesRegex(requirements.RequirementError, f"no {host} host yet"),
            ):
                requirements.require_supported_host(host)

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

    @unittest.skipUnless(
        os.name == "posix",
        "forwarding a termination signal is a POSIX contract; the product refuses other hosts",
    )
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



# CTest 4.3's JUnit report of one test that raised SIGTRAP and one that exited 1.
CTEST_JUNIT = """<?xml version="1.0" encoding="UTF-8"?>
<testsuite name="(empty)" tests="3" failures="2" disabled="0" skipped="0">
	<testcase name="traps" classname="traps" time="0.1" status="fail">
		<failure message="SIGTRAP"/>
		<properties/>
		<system-out></system-out>
	</testcase>
	<testcase name="fails" classname="fails" time="0.01" status="fail">
		<failure message="Failed"/>
		<properties/>
		<system-out></system-out>
	</testcase>
	<testcase name="passes" classname="passes" time="0.01" status="run">
		<properties/>
		<system-out></system-out>
	</testcase>
</testsuite>
"""
CTEST_COMMANDS = (
    '{"tests": [{"name": "traps", "command": ["/b/trap", "--x"], '
    '"properties": [{"name": "WORKING_DIRECTORY", "value": "/b"}]}, '
    '{"name": "fails", "command": ["false"]}, {"name": "passes", "command": ["true"]}]}'
)


# A ReportCrash .ips report: a header line, then the body, each one JSON document.
CRASH_REPORT = (
    '{"app_name":"test_trap","name":"test_trap","bug_type":"309"}\n'
    + json.dumps({
        "exception": {"type": "EXC_BREAKPOINT", "signal": "SIGTRAP"},
        "termination": {"indicator": "Trace/BPT trap: 5"},
        "usedImages": [{"name": "test_trap"}, {"name": "libsystem_c.dylib"}],
        "threads": [
            {"name": "main", "frames": [{"imageIndex": 1, "imageOffset": 4096}] * 8},
            {"triggered": True, "frames": [
                {"imageIndex": 0, "imageOffset": 256, "symbol": "xe::Emit", "symbolLocation": 12},
                {"imageIndex": 0, "imageOffset": 512},
            ]},
        ],
    })
)


class CrashTriageTests(unittest.TestCase):
    def _report(self, text: str) -> Path:
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        report = Path(directory.name) / "ctest-results.xml"
        report.write_text(text)
        return report

    def test_only_a_signal_death_is_rerun_under_gdb(self) -> None:
        runs: list[tuple[list[str], str | None]] = []
        backtrace = crash_triage.gdb_backtrace(lambda name: f"/usr/bin/{name}",
                                               lambda command, cwd: runs.append((command, cwd)))
        count = crash_triage.report_backtraces(self._report(CTEST_JUNIT), CTEST_COMMANDS,
                                               backtrace)
        self.assertEqual(count, 1)
        self.assertEqual(runs, [(["/usr/bin/gdb", "-batch", "-ex", "run", "-ex",
                                  "thread apply all bt", "--args", "/b/trap", "--x"], "/b")])

    def test_a_crash_report_shows_the_crashed_thread_whole(self) -> None:
        text = crash_triage.format_crash_report(CRASH_REPORT)
        self.assertIn("test_trap: EXC_BREAKPOINT (SIGTRAP)", text)
        self.assertIn("termination: Trace/BPT trap: 5", text)
        self.assertIn("thread 1 crashed (2 frames)", text)
        self.assertIn("#0 test_trap: xe::Emit + 12", text)
        self.assertIn("#1 test_trap: 0x200", text)
        self.assertIn("thread 0: main (8 frames, first 6)", text)

    def test_macos_shows_the_report_written_since_the_run_began(self) -> None:
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        reports = Path(directory.name)
        (reports / "trap-2026-09-24-old.ips").write_text("stale")
        os.utime(reports / "trap-2026-09-24-old.ips", (100.0, 100.0))
        (reports / "trap-2026-09-24-new.ips").write_text(CRASH_REPORT)
        (reports / "other-2026-09-24-new.ips").write_text("another test's")
        show = crash_triage.crash_report_backtrace(reports, since=1000.0)
        output = io.StringIO()
        with contextlib.redirect_stderr(output):
            show(crash_triage.TestCommand("traps", ("/b/trap",), None))
        self.assertIn("#0 test_trap: xe::Emit + 12", output.getvalue())

    def test_refusals_name_what_is_missing(self) -> None:
        with self.assertRaisesRegex(crash_triage.TriageError, "no JUnit report"):
            crash_triage.crashed_tests(Path("/nonexistent/ctest-results.xml"))
        with self.assertRaisesRegex(crash_triage.TriageError, "lists no test cases"):
            crash_triage.crashed_tests(self._report("<testsuite/>"))
        with self.assertRaisesRegex(crash_triage.TriageError, "gdb is not on PATH"):
            crash_triage.gdb_backtrace(lambda name: None, lambda command, cwd: None)
        with self.assertRaisesRegex(crash_triage.TriageError,
                                    "no source of crash stacks is known for Windows"):
            crash_triage.host_backtrace("Windows", 0.0)
        with self.assertRaisesRegex(crash_triage.TriageError, "no command for traps"):
            crash_triage.report_backtraces(self._report(CTEST_JUNIT), '{"tests": []}',
                                           lambda test: None)
        with self.assertRaisesRegex(crash_triage.TriageError, "lists no threads"):
            crash_triage.format_crash_report('{"name":"t"}\n{"threads":[]}')
        with self.assertRaisesRegex(crash_triage.TriageError, "no body"):
            crash_triage.format_crash_report('{"name":"t"}')
        ticks = iter(range(0, 1000, 10))
        show = crash_triage.crash_report_backtrace(Path("/nonexistent"), 0.0,
                                                   clock=lambda: float(next(ticks)),
                                                   sleep=lambda seconds: None)
        with self.assertRaisesRegex(crash_triage.TriageError,
                                    "no crash report of trap appeared in /nonexistent within 60 s"):
            show(crash_triage.TestCommand("traps", ("/b/trap",), None))

    def test_a_hung_debugger_is_refused(self) -> None:
        with patch.object(crash_triage.subprocess, "run",
                               side_effect=subprocess.TimeoutExpired("gdb", 300)):
            with self.assertRaisesRegex(crash_triage.TriageError,
                                        "gdb was still running after 300 s"):
                crash_triage.run_under_debugger(["gdb"], None)

if __name__ == "__main__":
    unittest.main()
