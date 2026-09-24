"""Backtraces of the CTest tests that crashed in one run, taken under the host debugger.

A test that dies on a signal before it logs anything, as the A64 runtime tests
did on macOS CI, leaves CTest nothing to show. Rerunning each such test under
the host's debugger prints where it stopped. The crashed set comes from the
JUnit report of the same CTest run, never from CTest's LastTestsFailed.log,
which a later passing run leaves in place.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import xml.etree.ElementTree as ElementTree
from collections.abc import Callable, Sequence
from dataclasses import dataclass
from pathlib import Path


class TriageError(RuntimeError):
    """The crashed tests cannot be named or rerun."""


@dataclass(frozen=True)
class TestCommand:
    name: str
    command: tuple[str, ...]
    working_directory: str | None


# CTest's failure messages for a test that exited on its own or ran out of
# time; any other (SIGTRAP, SIGSEGV, Exception) ended abnormally.
ORDINARY_FAILURES = frozenset({"Failed", "Timeout"})


def crashed_tests(junit_report: Path) -> list[str]:
    """Names of the tests the JUnit report says ended abnormally; refuses a missing report."""

    if not junit_report.is_file():
        raise TriageError(f"no JUnit report at {junit_report}; the failed tests are unknown")
    suite = ElementTree.parse(junit_report).getroot()
    cases = suite.findall("testcase")
    if not cases:
        raise TriageError(f"{junit_report} lists no test cases")
    crashed = []
    for case in cases:
        failure = case.find("failure")
        if failure is not None and failure.get("message") not in ORDINARY_FAILURES:
            crashed.append(case.get("name", ""))
    return crashed


def test_commands(show_only_json: str) -> dict[str, TestCommand]:
    """Each registered test's command line and working directory, by name."""

    commands = {}
    for test in json.loads(show_only_json)["tests"]:
        properties = {item["name"]: item["value"] for item in test.get("properties", [])}
        commands[test["name"]] = TestCommand(test["name"], tuple(test["command"]),
                                             properties.get("WORKING_DIRECTORY"))
    return commands


def debugger_command(system: str, find: Callable[[str], str | None],
                     command: Sequence[str]) -> list[str]:
    """The command that runs command under the host's debugger and prints every thread's stack."""

    if system == "Darwin":
        lldb = find("lldb")
        if lldb is None:
            raise TriageError("lldb is not on PATH")
        return [lldb, "--batch", "-o", "run", "-k", "thread backtrace all", "-k", "quit",
                "--", *command]
    if system == "Linux":
        gdb = find("gdb")
        if gdb is None:
            raise TriageError("gdb is not on PATH")
        return [gdb, "-batch", "-ex", "run", "-ex", "thread apply all bt", "--args", *command]
    raise TriageError(f"no debugger is known for {system}")


def report_backtraces(junit_report: Path, show_only_json: str, system: str,
                      find: Callable[[str], str | None],
                      run: Callable[[list[str], str | None], None]) -> int:
    """Rerun every test that ended abnormally under the debugger; returns how many.

    Refuses, naming what is missing, rather than reporting nothing.
    """

    crashed = crashed_tests(junit_report)
    commands = test_commands(show_only_json)
    unknown = [name for name in crashed if name not in commands]
    if unknown:
        raise TriageError(f"CTest registers no command for {', '.join(unknown)}")
    for name in crashed:
        test = commands[name]
        print(f"crash triage: {name} under the debugger", file=sys.stderr, flush=True)
        run(debugger_command(system, find, test.command), test.working_directory)
    print(f"crash triage: reran {len(crashed)} test(s) that ended abnormally, of "
          f"{junit_report.name}", file=sys.stderr, flush=True)
    return len(crashed)


def run_under_debugger(command: list[str], working_directory: str | None) -> None:
    """Run a debugger command, whose own exit status says nothing about the test."""

    subprocess.run(command, cwd=working_directory, check=False,
                   env={**os.environ, "NO_COLOR": "1"})
