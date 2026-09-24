"""Backtraces of the CTest tests that crashed in one run.

A test that dies on a signal before it logs anything, as the A64 runtime tests
did on macOS CI, leaves CTest nothing to show. The crashed set comes from the
JUnit report of the same CTest run, never from CTest's LastTestsFailed.log,
which a later passing run leaves in place. Each host has its own source of the
stack: Linux reruns the test under gdb; macOS reads the crash report
ReportCrash wrote for the test's process, since lldb cannot launch a process
on a hosted runner (a `run` there waited until the job's time limit).
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import time
import xml.etree.ElementTree as ElementTree
from collections.abc import Callable, Sequence
from dataclasses import dataclass
from pathlib import Path


class TriageError(RuntimeError):
    """The crashed tests cannot be named or their stacks cannot be shown."""


@dataclass(frozen=True)
class TestCommand:
    name: str
    command: tuple[str, ...]
    working_directory: str | None


# CTest's failure messages for a test that exited on its own or ran out of
# time; any other (SIGTRAP, SIGSEGV, Exception) ended abnormally.
ORDINARY_FAILURES = frozenset({"Failed", "Timeout"})
# A rerun under gdb that has not finished by then is hung, not crashing.
DEBUGGER_SECONDS = 300
# ReportCrash writes its report some seconds after the process dies.
CRASH_REPORT_WAIT_SECONDS = 60.0
CRASH_REPORT_POLL_SECONDS = 2.0
# Frames shown for each thread other than the one that crashed.
OTHER_THREAD_FRAMES = 6


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


def report_backtraces(junit_report: Path, show_only_json: str,
                      backtrace: Callable[[TestCommand], None]) -> int:
    """Show the stack of every test that ended abnormally; returns how many.

    Refuses, naming what is missing, rather than reporting nothing.
    """

    crashed = crashed_tests(junit_report)
    commands = test_commands(show_only_json)
    unknown = [name for name in crashed if name not in commands]
    if unknown:
        raise TriageError(f"CTest registers no command for {', '.join(unknown)}")
    for name in crashed:
        print(f"crash triage: the stack of {name}", file=sys.stderr, flush=True)
        backtrace(commands[name])
    print(f"crash triage: showed {len(crashed)} test(s) that ended abnormally, of "
          f"{junit_report.name}", file=sys.stderr, flush=True)
    return len(crashed)


def gdb_command(gdb: str, command: Sequence[str]) -> list[str]:
    """The command that runs command under gdb and prints every thread's stack."""

    return [gdb, "-batch", "-ex", "run", "-ex", "thread apply all bt", "--args", *command]


def gdb_backtrace(find: Callable[[str], str | None],
                  run: Callable[[list[str], str | None], None]) -> Callable[[TestCommand], None]:
    """Rerun each test under gdb; refuses when gdb is not on PATH."""

    gdb = find("gdb")
    if gdb is None:
        raise TriageError("gdb is not on PATH")
    return lambda test: run(gdb_command(gdb, test.command), test.working_directory)


def run_under_debugger(command: list[str], working_directory: str | None) -> None:
    """Run a debugger command, whose own exit status says nothing about the test.

    Refuses a debugger still running after DEBUGGER_SECONDS.
    """

    try:
        subprocess.run(command, cwd=working_directory, check=False,
                       env={**os.environ, "NO_COLOR": "1"}, timeout=DEBUGGER_SECONDS)
    except subprocess.TimeoutExpired as error:
        raise TriageError(f"{command[0]} was still running after {DEBUGGER_SECONDS} s") from error


def parse_crash_report(text: str) -> tuple[dict[str, object], dict[str, object]]:
    """The header and body of a ReportCrash .ips report: two JSON documents, one per part."""

    header, separator, body = text.partition("\n")
    if not separator:
        raise TriageError("the crash report has no body after its header line")
    return json.loads(header), json.loads(body)


def format_crash_report(text: str) -> str:
    """The exception, the crashed thread's frames, and the head of every other thread's."""

    header, body = parse_crash_report(text)
    images = body.get("usedImages", [])
    threads = body.get("threads", [])
    if not threads:
        raise TriageError(f"the crash report for {header.get('name', '?')} lists no threads")
    exception = body.get("exception", {})
    lines = [f"{header.get('name', '?')}: {exception.get('type', '?')} "
             f"({exception.get('signal', '?')}) {exception.get('subtype', '')}".rstrip()]
    termination = body.get("termination", {})
    if termination.get("indicator"):
        lines.append(f"termination: {termination['indicator']}")
    for message in body.get("asi", {}).values():
        lines.append(f"message: {' '.join(message)}")
    for index, thread in enumerate(threads):
        crashed = bool(thread.get("triggered"))
        frames = thread.get("frames", [])
        shown = frames if crashed else frames[:OTHER_THREAD_FRAMES]
        lines.append(f"thread {index}{' crashed' if crashed else ''}"
                     f"{': ' + thread['name'] if thread.get('name') else ''}"
                     f" ({len(frames)} frames{'' if shown is frames else f', first {len(shown)}'})")
        for number, frame in enumerate(shown):
            image_index = frame.get("imageIndex")
            image = (images[image_index].get("name", "?")
                     if isinstance(image_index, int) and image_index < len(images) else "?")
            symbol = frame.get("symbol")
            where = (f"{symbol} + {frame.get('symbolLocation', 0)}" if symbol
                     else f"0x{frame.get('imageOffset', 0):x}")
            lines.append(f"  #{number} {image}: {where}")
    return "\n".join(lines)


def crash_reports(directory: Path, executable: str, since: float) -> list[Path]:
    """ReportCrash's reports for executable written at or after since, oldest first."""

    if not directory.is_dir():
        return []
    reports = [path for path in directory.glob(f"{executable}-*.ips")
               if path.stat().st_mtime >= since]
    return sorted(reports, key=lambda path: path.stat().st_mtime)


def crash_report_backtrace(directory: Path, since: float,
                           clock: Callable[[], float] = time.monotonic,
                           sleep: Callable[[float], None] = time.sleep
                           ) -> Callable[[TestCommand], None]:
    """Print the newest crash report of each test's executable written since the run began.

    Waits up to CRASH_REPORT_WAIT_SECONDS for ReportCrash; refuses, naming the
    directory, when no report appears.
    """

    def show(test: TestCommand) -> None:
        executable = Path(test.command[0]).name
        deadline = clock() + CRASH_REPORT_WAIT_SECONDS
        while not (reports := crash_reports(directory, executable, since)):
            if clock() >= deadline:
                raise TriageError(f"no crash report of {executable} appeared in {directory} "
                                  f"within {CRASH_REPORT_WAIT_SECONDS:.0f} s")
            sleep(CRASH_REPORT_POLL_SECONDS)
        print(format_crash_report(reports[-1].read_text()), file=sys.stderr, flush=True)

    return show


def host_backtrace(system: str, since: float) -> Callable[[TestCommand], None]:
    """The host's source of a crashed test's stack; refuses a host with none."""

    if system == "Linux":
        return gdb_backtrace(shutil.which, run_under_debugger)
    if system == "Darwin":
        return crash_report_backtrace(Path.home() / "Library/Logs/DiagnosticReports", since)
    raise TriageError(f"no source of crash stacks is known for {system}")
