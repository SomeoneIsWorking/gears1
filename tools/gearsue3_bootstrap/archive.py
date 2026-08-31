"""Bounded materialization of a user-owned 7z disc archive."""

from __future__ import annotations

import hashlib
import os
import shutil
from collections.abc import Mapping
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

from .process import CommandRunner

ARCHIVE_SUFFIX = ".7z"
IMAGE_SUFFIXES = frozenset({".img", ".iso", ".xgd", ".xiso"})
MAX_ARCHIVE_ENTRIES = 100_000
MAX_ARCHIVE_BYTES = 16 * 1024**3
MAX_EXPANDED_BYTES = 16 * 1024**3


class ArchiveError(RuntimeError):
    """A selected archive cannot be safely reduced to one disc image."""


@dataclass(frozen=True)
class ArchiveEntry:
    path: str
    size: int
    folder: bool
    encrypted: bool


def _hash_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def _records(listing: str) -> list[Mapping[str, str]]:
    result: list[Mapping[str, str]] = []
    current: dict[str, str] = {}
    for line in listing.splitlines() + [""]:
        if not line.strip():
            if current:
                result.append(current)
                current = {}
            continue
        name, separator, value = line.partition(" = ")
        if separator:
            current[name] = value
    return result


def _entry(record: Mapping[str, str]) -> ArchiveEntry | None:
    path = record.get("Path")
    if path is None or record.get("Type") is not None:
        return None
    raw_size = record.get("Size")
    if raw_size is None:
        raise ArchiveError(f"archive entry has no size: {path}")
    try:
        size = int(raw_size, 10)
    except ValueError as error:
        raise ArchiveError(f"archive entry has an invalid size: {path}") from error
    if size < 0:
        raise ArchiveError(f"archive entry has a negative size: {path}")
    return ArchiveEntry(
        path=path,
        size=size,
        folder=record.get("Folder") == "+",
        encrypted=record.get("Encrypted") == "+",
    )


def inspect_archive(
    archive: Path,
    *,
    runner: CommandRunner,
    cwd: Path,
) -> tuple[ArchiveEntry, ...]:
    """Validate archive metadata and return its bounded entries."""

    if not archive.is_file():
        raise ArchiveError(f"archive is not a regular file: {archive}")
    if archive.stat().st_size > MAX_ARCHIVE_BYTES:
        raise ArchiveError(
            f"archive exceeds the {MAX_ARCHIVE_BYTES} byte limit: {archive}"
        )
    listing = runner.capture(["7z", "l", "-slt", "-bd", "-spd", archive], cwd=cwd)
    entries = tuple(
        entry for record in _records(listing) if (entry := _entry(record)) is not None
    )
    if not entries:
        raise ArchiveError(f"archive contains no entries: {archive}")
    if len(entries) > MAX_ARCHIVE_ENTRIES:
        raise ArchiveError(f"archive contains too many entries: {len(entries)}")
    expanded_size = sum(entry.size for entry in entries)
    if expanded_size > MAX_EXPANDED_BYTES:
        raise ArchiveError(
            f"archive entries exceed the {MAX_EXPANDED_BYTES} byte expanded-size limit"
        )
    for entry in entries:
        member = PurePosixPath(entry.path)
        if (
            member.is_absolute()
            or not entry.path
            or "\\" in entry.path
            or ".." in member.parts
        ):
            raise ArchiveError(f"archive entry has an unsafe path: {entry.path!r}")
    return entries


def _image_entry(entries: tuple[ArchiveEntry, ...]) -> ArchiveEntry:
    candidates = tuple(
        entry
        for entry in entries
        if not entry.folder and Path(entry.path).suffix.lower() in IMAGE_SUFFIXES
    )
    if len(candidates) != 1:
        raise ArchiveError(
            f"archive must contain exactly one disc image; found {len(candidates)}"
        )
    selected = candidates[0]
    if selected.encrypted:
        raise ArchiveError(f"archive disc image is encrypted: {selected.path}")
    return selected


def materialize_disc_image(
    archive: Path,
    destination_root: Path,
    *,
    runner: CommandRunner,
    cwd: Path,
) -> Path:
    """Extract one validated image into stable ignored storage."""

    entries = inspect_archive(archive, runner=runner, cwd=cwd)
    selected = _image_entry(entries)
    archive_digest = _hash_file(archive)
    output_dir = destination_root / archive_digest
    output = output_dir / "disc.iso"
    if output_dir.exists() and (output_dir.is_symlink() or not output_dir.is_dir()):
        raise ArchiveError(f"archive output path is not a directory: {output_dir}")
    if output.is_symlink():
        raise ArchiveError(f"archive output image is a symlink: {output}")
    if output.is_file() and output.stat().st_size == selected.size:
        return output

    staging = output_dir / ".staging"
    if staging.exists():
        if staging.is_symlink() or not staging.is_dir():
            raise ArchiveError(f"archive staging path is not a directory: {staging}")
        shutil.rmtree(staging)
    staging.mkdir(parents=True, exist_ok=True)
    try:
        runner.run(
            ["7z", "e", "-y", "-spd", f"-o{staging}", archive, selected.path],
            cwd=cwd,
        )
        extracted = staging / Path(selected.path).name
        if not extracted.is_file() or extracted.stat().st_size != selected.size:
            raise ArchiveError(
                f"archive extraction did not produce the declared image: {selected.path}"
            )
        output_dir.mkdir(parents=True, exist_ok=True)
        os.replace(extracted, output)
    finally:
        if staging.exists():
            shutil.rmtree(staging)
    return output
