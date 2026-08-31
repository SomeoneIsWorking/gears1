"""Checked command execution and direct-child runtime lifecycle ownership."""

from __future__ import annotations

import os
import signal
import subprocess
import sys
import threading
import time
from collections.abc import Mapping, Sequence
from pathlib import Path
from typing import BinaryIO


class CommandError(RuntimeError):
    """A required provisioning command failed."""


class CommandRunner:
    def run(
        self,
        command: Sequence[str | os.PathLike[str]],
        *,
        cwd: Path,
        environ: Mapping[str, str] | None = None,
    ) -> None:
        rendered = [os.fspath(value) for value in command]
        print(f"bootstrap: {' '.join(rendered)}", file=sys.stderr)
        completed = subprocess.run(rendered, cwd=cwd, env=environ, check=False)
        if completed.returncode != 0:
            raise CommandError(
                f"command exited {completed.returncode}: {' '.join(rendered)}"
            )

    def capture(
        self,
        command: Sequence[str | os.PathLike[str]],
        *,
        cwd: Path,
    ) -> str:
        rendered = [os.fspath(value) for value in command]
        completed = subprocess.run(
            rendered, cwd=cwd, check=False, capture_output=True, text=True
        )
        if completed.returncode != 0:
            detail = completed.stderr.strip()
            suffix = f": {detail}" if detail else ""
            raise CommandError(
                f"command exited {completed.returncode}: {' '.join(rendered)}{suffix}"
            )
        return completed.stdout.strip()


def terminate_child(process: subprocess.Popen[object], grace_seconds: float = 10) -> None:
    """Terminate exactly one child, escalating only when its grace period expires."""

    if process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=grace_seconds)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()


def terminate_process_group(
    process: subprocess.Popen[object], grace_seconds: float = 10
) -> None:
    """Terminate one captured new-session process group and no sibling run."""

    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    deadline = time.monotonic() + grace_seconds
    while time.monotonic() < deadline:
        try:
            os.killpg(process.pid, 0)
        except ProcessLookupError:
            break
        time.sleep(0.05)
    else:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    if process.poll() is None:
        process.wait()


def run_for_duration(
    command: Sequence[str | os.PathLike[str]],
    *,
    cwd: Path,
    environ: Mapping[str, str],
    duration_seconds: float,
) -> int:
    """Run a diagnostic child for a bounded duration without killing by name."""

    process = subprocess.Popen(
        [os.fspath(value) for value in command], cwd=cwd, env=dict(environ)
    )
    deadline = time.monotonic() + duration_seconds
    try:
        while process.poll() is None and time.monotonic() < deadline:
            time.sleep(min(0.1, max(0.0, deadline - time.monotonic())))
        if process.poll() is None:
            terminate_child(process)
            return 0
        assert process.returncode is not None
        return process.returncode
    finally:
        terminate_child(process)


def _copy_output(source: BinaryIO, destinations: tuple[BinaryIO, ...]) -> None:
    while chunk := source.read(64 * 1024):
        for destination in destinations:
            destination.write(chunk)
            destination.flush()


def run_logged_child(
    command: Sequence[str | os.PathLike[str]],
    *,
    cwd: Path,
    environ: Mapping[str, str],
    log_path: Path,
) -> int:
    """Run one direct child, tee output, forward termination, and return its status."""

    log_path.parent.mkdir(parents=True, exist_ok=True)
    rendered = [os.fspath(value) for value in command]
    process: subprocess.Popen[bytes] | None = None
    previous_handlers: dict[int, signal.Handlers] = {}

    def forward(signum: int, _frame: object) -> None:
        if process is not None and process.poll() is None:
            process.send_signal(signum)

    handled = (signal.SIGINT, signal.SIGTERM, signal.SIGHUP)
    try:
        for signum in handled:
            previous_handlers[signum] = signal.signal(signum, forward)
        with log_path.open("wb") as log:
            process = subprocess.Popen(
                rendered,
                cwd=cwd,
                env=dict(environ),
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
            )
            assert process.stdout is not None
            copier = threading.Thread(
                target=_copy_output,
                args=(process.stdout, (sys.stdout.buffer, log)),
                daemon=False,
            )
            copier.start()
            returncode = process.wait()
            copier.join()
            process.stdout.close()
            return returncode
    finally:
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        for signum, handler in previous_handlers.items():
            signal.signal(signum, handler)
