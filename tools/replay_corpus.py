"""Shared, fail-closed access to frame-replay captures and outputs."""

from __future__ import annotations

import os
from pathlib import Path
import shutil

from gearsue3_bootstrap.paths import BuildPathError, build_directory

REPO_ROOT = Path(__file__).resolve().parents[1]


class ReplayCorpusError(RuntimeError):
    """A replay diagnostic has no executable, corpus, or fresh output."""


def environment_integer(
    environment: dict[str, str],
    name: str,
    default: int,
    *,
    minimum: int = 0,
    maximum: int | None = None,
) -> int:
    raw = environment.get(name, str(default))
    try:
        value = int(raw)
    except ValueError as error:
        raise ReplayCorpusError(f"{name} must be an integer, got {raw!r}") from error
    if value < minimum or (maximum is not None and value > maximum):
        bounds = f"{minimum}..{maximum}" if maximum is not None else f">= {minimum}"
        raise ReplayCorpusError(f"{name} must be {bounds}, got {value}")
    return value


def reset_scratch_directory(path: Path) -> Path:
    scratch = (REPO_ROOT / "scratch").resolve()
    target = path.resolve()
    if target == scratch or scratch not in target.parents:
        raise ReplayCorpusError(f"cleanup target escapes a scoped scratch child: {path}")
    if target.is_symlink():
        raise ReplayCorpusError(f"cleanup target is a symlink: {path}")
    if target.exists():
        shutil.rmtree(target)
    target.mkdir(parents=True)
    return target


def replay_executable(environ: dict[str, str] | None = None) -> Path:
    environment = os.environ if environ is None else environ
    try:
        build_dir = build_directory(
            REPO_ROOT,
            environment.get("GEARS_BUILD_DIR"),
            REPO_ROOT / "build/release",
        )
    except BuildPathError as error:
        raise ReplayCorpusError(str(error)) from error
    replay = build_dir / "runtime/frame_replay"
    if not replay.is_file() or not os.access(replay, os.X_OK):
        raise ReplayCorpusError(
            f"frame replay is not built: {replay}; build target frame_replay first"
        )
    return replay


def captures(root: Path | None = None) -> tuple[Path, ...]:
    directory = REPO_ROOT / "scratch/frames" if root is None else root
    selected = tuple(sorted(directory.glob("*.gfr")))
    if not selected:
        raise ReplayCorpusError(
            f"no captures in {directory}; this run would examine nothing"
        )
    return selected


def clear_frame_outputs(directory: Path | None = None) -> None:
    screenshots = REPO_ROOT / "scratch/screenshots" if directory is None else directory
    resolved_scratch = (REPO_ROOT / "scratch").resolve()
    resolved = screenshots.resolve()
    if resolved != resolved_scratch and resolved_scratch not in resolved.parents:
        raise ReplayCorpusError(f"screenshot output escapes scratch: {screenshots}")
    screenshots.mkdir(parents=True, exist_ok=True)
    for output in screenshots.glob("frame*.ppm"):
        if output.is_symlink() or not output.is_file():
            raise ReplayCorpusError(f"unexpected frame output type: {output}")
        output.unlink()


def one_frame_output(directory: Path | None = None) -> Path:
    screenshots = REPO_ROOT / "scratch/screenshots" if directory is None else directory
    outputs = tuple(sorted(screenshots.glob("frame*.ppm")))
    if len(outputs) != 1:
        raise ReplayCorpusError(
            f"replay emitted {len(outputs)} frame screenshots in {screenshots}; expected exactly one"
        )
    return outputs[0]


def ppm_pixels(path: Path) -> tuple[int, int, bytes]:
    data = path.read_bytes()
    parts = data.split(b"\n", 3)
    if len(parts) != 4 or parts[0] != b"P6":
        raise ReplayCorpusError(f"invalid binary PPM header: {path}")
    dimensions = parts[1].split()
    if len(dimensions) != 2 or parts[2] != b"255":
        raise ReplayCorpusError(f"unsupported binary PPM layout: {path}")
    width, height = (int(value) for value in dimensions)
    pixels = parts[3]
    expected = width * height * 3
    if len(pixels) != expected:
        raise ReplayCorpusError(
            f"truncated PPM {path}: expected {expected} pixel bytes, got {len(pixels)}"
        )
    return width, height, pixels
