"""Authoritative repository build-path policy."""

from __future__ import annotations

from pathlib import Path


class BuildPathError(RuntimeError):
    """A configured build path escapes the repository build root."""


def build_directory(repo_root: Path, configured: str | None, default: Path) -> Path:
    selected = Path(configured) if configured else default
    if not selected.is_absolute():
        selected = repo_root / selected
    build_root = (repo_root / "build").resolve()
    resolved = selected.resolve()
    if resolved == build_root or build_root not in resolved.parents:
        raise BuildPathError(
            f"build directory must be a child of {build_root}, not {resolved}"
        )
    if resolved.is_symlink():
        raise BuildPathError(f"build directory cannot be a symlink: {resolved}")
    return resolved
