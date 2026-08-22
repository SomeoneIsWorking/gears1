#!/usr/bin/env python3
"""Create a local identity record from a user-owned Xbox 360 disc image.

The repository supplies code only. This tool resolves one image without ever
copying it into the repository, fingerprints it with streaming SHA-256, and
writes only a factual identity record below the ignored
``scratch/titles/<disc-sha256>/`` tree. If the caller also supplies the
extracted ``default.xex``, its generic XEX execution metadata is recorded.

Resolution order is deliberate and strict:

1. positional image path;
2. ``GEARS_ISO`` from the process environment, then the repository ``.env``;
3. exactly one supported image in the ignored ``roms/`` drop-in directory.

A selected but missing higher-priority path is an error. It never falls back to
a different image, because silently recompiling another title or revision would
associate generated code with the wrong executable.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Mapping, Sequence


ENV_IMAGE = "GEARS_ISO"
DROP_IN_DIRECTORY = "roms"
SUPPORTED_DROP_IN_SUFFIXES = frozenset({".img", ".iso", ".xgd", ".xiso"})
HASH_CHUNK_SIZE = 1024 * 1024
IDENTITY_SCHEMA = 1

XGD_SECTOR_SIZE = 2048
XGD_VOLUME_DESCRIPTOR_SECTOR = 32
XGD_VOLUME_MAGIC = b"MICROSOFT*XBOX*MEDIA"
XGD_CANDIDATE_PARTITION_OFFSETS = (0, 0x0FD90000, 0x02080000, 0x18300000)

XEX_MAGIC = b"XEX2"
XEX_HEADER_EXECUTION_INFO = 0x00040006
XEX_FIXED_HEADER_SIZE = 24
XEX_OPTIONAL_HEADER_SIZE = 8
XEX_EXECUTION_INFO_SIZE = 24


class IdentityError(RuntimeError):
    """An input could not be identified without guessing."""


@dataclass(frozen=True)
class ResolvedImage:
    path: Path
    source: str


def _absolute(path: Path, base: Path) -> Path:
    if path.is_absolute():
        return path.resolve()
    return (base / path).resolve()


def _require_regular_file(path: Path, description: str) -> Path:
    if not path.exists():
        raise IdentityError(f"{description} does not exist: {path}")
    if not path.is_file():
        raise IdentityError(f"{description} is not a regular file: {path}")
    return path


def _dotenv_image(env_file: Path) -> str | None:
    if not env_file.exists():
        return None
    if not env_file.is_file():
        raise IdentityError(f"environment file is not a regular file: {env_file}")

    values: list[str] = []
    with env_file.open(encoding="utf-8") as source:
        for number, raw_line in enumerate(source, 1):
            line = raw_line.strip()
            if not line or line.startswith("#"):
                continue
            name, separator, value = line.partition("=")
            if separator == "" or name.strip() != ENV_IMAGE:
                continue
            value = value.strip()
            if value[:1] in {"'", '"'}:
                quote = value[0]
                if len(value) < 2 or value[-1] != quote:
                    raise IdentityError(
                        f"{env_file}:{number}: unterminated quoted {ENV_IMAGE} value"
                    )
                value = value[1:-1]
            if not value:
                raise IdentityError(f"{env_file}:{number}: {ENV_IMAGE} is empty")
            values.append(value)

    if len(values) > 1:
        raise IdentityError(f"{env_file} defines {ENV_IMAGE} more than once")
    return values[0] if values else None


def _drop_in_image(repo_root: Path) -> Path:
    directory = repo_root / DROP_IN_DIRECTORY
    if not directory.exists():
        raise IdentityError(
            f"no disc image was selected and drop-in directory is missing: {directory}"
        )
    if not directory.is_dir():
        raise IdentityError(f"drop-in location is not a directory: {directory}")

    candidates = sorted(
        path.resolve()
        for path in directory.iterdir()
        if path.is_file() and path.suffix.lower() in SUPPORTED_DROP_IN_SUFFIXES
    )
    if not candidates:
        suffixes = ", ".join(sorted(SUPPORTED_DROP_IN_SUFFIXES))
        raise IdentityError(
            f"no supported disc image found in {directory}; expected one of: {suffixes}"
        )
    if len(candidates) != 1:
        names = ", ".join(path.name for path in candidates)
        raise IdentityError(
            f"ambiguous drop-in images in {directory}: {names}; select one explicitly"
        )
    return candidates[0]


def resolve_image(
    explicit: str | os.PathLike[str] | None,
    repo_root: Path,
    environ: Mapping[str, str] | None = None,
    env_file: Path | None = None,
    current_directory: Path | None = None,
) -> ResolvedImage:
    """Resolve one image with explicit > environment/.env > drop-in priority."""

    root = repo_root.resolve()
    process_environment = os.environ if environ is None else environ
    cwd = Path.cwd() if current_directory is None else current_directory

    if explicit is not None:
        path = _absolute(Path(explicit), cwd)
        return ResolvedImage(_require_regular_file(path, "explicit disc image"), "explicit")

    environment_value = process_environment.get(ENV_IMAGE)
    if environment_value:
        path = _absolute(Path(environment_value), root)
        return ResolvedImage(
            _require_regular_file(path, f"{ENV_IMAGE} disc image"), "environment"
        )

    dotenv_path = root / ".env" if env_file is None else _absolute(env_file, root)
    dotenv_value = _dotenv_image(dotenv_path)
    if dotenv_value is not None:
        path = _absolute(Path(dotenv_value), root)
        return ResolvedImage(
            _require_regular_file(path, f"{ENV_IMAGE} disc image from {dotenv_path}"),
            "dotenv",
        )

    return ResolvedImage(_drop_in_image(root), "drop-in")


def fingerprint(path: Path) -> dict[str, int | str]:
    """Return a path-free, streaming SHA-256 identity for one regular file."""

    _require_regular_file(path, "fingerprint input")
    digest = hashlib.sha256()
    size = 0
    with path.open("rb") as source:
        while True:
            chunk = source.read(HASH_CHUNK_SIZE)
            if not chunk:
                break
            digest.update(chunk)
            size += len(chunk)
    return {"sha256": digest.hexdigest(), "size": size}


def inspect_disc(path: Path) -> dict[str, int | str]:
    """Identify the XGD filesystem without extracting any user-owned bytes."""

    _require_regular_file(path, "disc image")
    matches = []
    with path.open("rb") as source:
        for partition_offset in XGD_CANDIDATE_PARTITION_OFFSETS:
            source.seek(
                partition_offset + XGD_VOLUME_DESCRIPTOR_SECTOR * XGD_SECTOR_SIZE
            )
            if source.read(len(XGD_VOLUME_MAGIC)) == XGD_VOLUME_MAGIC:
                matches.append(partition_offset)
    if not matches:
        raise IdentityError(f"unknown disc image format in {path}; expected Xbox XGD")
    if len(matches) != 1:
        offsets = ", ".join(f"0x{offset:X}" for offset in matches)
        raise IdentityError(f"ambiguous XGD partition offsets in {path}: {offsets}")
    return {"format": "XGD", "partition_offset": matches[0]}


def _hex32(value: int) -> str:
    return f"0x{value:08X}"


def parse_xex_metadata(path: Path) -> dict[str, object]:
    """Read format-level identity fields from an extracted ``default.xex``."""

    _require_regular_file(path, "extracted default.xex")
    if path.name.lower() != "default.xex":
        raise IdentityError(
            f"XEX input must be named default.xex, not {path.name!r}"
        )

    with path.open("rb") as source:
        fixed = source.read(XEX_FIXED_HEADER_SIZE)
        if len(fixed) != XEX_FIXED_HEADER_SIZE or fixed[:4] != XEX_MAGIC:
            raise IdentityError(f"unknown executable format in {path}; expected XEX2")

        module_flags, header_size, _reserved, _security_offset, header_count = (
            struct.unpack(">5I", fixed[4:])
        )
        table_size = header_count * XEX_OPTIONAL_HEADER_SIZE
        table_end = XEX_FIXED_HEADER_SIZE + table_size
        if header_size < table_end:
            raise IdentityError(
                f"invalid XEX2 header in {path}: optional-header table exceeds header size"
            )
        source.seek(0, os.SEEK_END)
        file_size = source.tell()
        if header_size > file_size or table_end > file_size:
            raise IdentityError(f"truncated XEX2 header in {path}")

        source.seek(XEX_FIXED_HEADER_SIZE)
        optional_headers = source.read(table_size)
        execution_offsets = []
        for offset in range(0, table_size, XEX_OPTIONAL_HEADER_SIZE):
            key, value = struct.unpack_from(">II", optional_headers, offset)
            if key == XEX_HEADER_EXECUTION_INFO:
                execution_offsets.append(value)

        if not execution_offsets:
            raise IdentityError(f"XEX2 execution metadata is missing from {path}")
        if len(execution_offsets) != 1:
            raise IdentityError(f"XEX2 execution metadata is ambiguous in {path}")

        execution_offset = execution_offsets[0]
        execution_end = execution_offset + XEX_EXECUTION_INFO_SIZE
        if execution_offset < table_end or execution_end > header_size:
            raise IdentityError(f"invalid XEX2 execution metadata offset in {path}")
        source.seek(execution_offset)
        execution = source.read(XEX_EXECUTION_INFO_SIZE)
        if len(execution) != XEX_EXECUTION_INFO_SIZE:
            raise IdentityError(f"truncated XEX2 execution metadata in {path}")

    media_id, version, base_version, title_id = struct.unpack(">4I", execution[:16])
    platform, executable_table, disc_number, disc_count = execution[16:20]
    (savegame_id,) = struct.unpack(">I", execution[20:24])
    return {
        "format": "XEX2",
        "module_flags": _hex32(module_flags),
        "header_size": header_size,
        "execution": {
            "media_id": _hex32(media_id),
            "version": _hex32(version),
            "base_version": _hex32(base_version),
            "title_id": _hex32(title_id),
            "platform": platform,
            "executable_table": executable_table,
            "disc_number": disc_number,
            "disc_count": disc_count,
            "savegame_id": _hex32(savegame_id),
        },
    }


def _require_ignored_scratch(repo_root: Path) -> Path:
    ignore_file = repo_root / ".gitignore"
    if not ignore_file.is_file():
        raise IdentityError(f"repository has no .gitignore: {ignore_file}")
    ignored = {
        line.strip()
        for line in ignore_file.read_text(encoding="utf-8").splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    }
    if "scratch/" not in ignored and "/scratch/" not in ignored:
        raise IdentityError(
            f"refusing to create derived identity data: {ignore_file} does not ignore scratch/"
        )
    scratch = repo_root / "scratch"
    titles = scratch / "titles"
    for path in (scratch, titles):
        if path.is_symlink():
            raise IdentityError(f"refusing derived-output symlink outside repository: {path}")
    return titles


def build_identity(disc_path: Path, xex_path: Path | None = None) -> dict[str, object]:
    disc = fingerprint(disc_path)
    disc.update(inspect_disc(disc_path))
    identity: dict[str, object] = {
        "schema": IDENTITY_SCHEMA,
        "disc": disc,
    }
    if xex_path is not None:
        xex = fingerprint(xex_path)
        xex["metadata"] = parse_xex_metadata(xex_path)
        identity["xex"] = xex
    return identity


def _merge_existing(
    existing: dict[str, object], requested: dict[str, object]
) -> dict[str, object]:
    if existing.get("schema") != IDENTITY_SCHEMA or existing.get("disc") != requested["disc"]:
        raise IdentityError("existing identity manifest does not match its cache key")
    old_xex = existing.get("xex")
    new_xex = requested.get("xex")
    if old_xex is not None and new_xex is not None and old_xex != new_xex:
        raise IdentityError(
            "existing identity manifest names a different default.xex for this disc"
        )
    if old_xex is not None and new_xex is None:
        return existing
    return requested


def write_identity(repo_root: Path, identity: dict[str, object]) -> Path:
    disc = identity.get("disc")
    if not isinstance(disc, dict) or not isinstance(disc.get("sha256"), str):
        raise IdentityError("identity has no disc SHA-256 cache key")
    cache_key = disc["sha256"]
    if len(cache_key) != 64 or any(ch not in "0123456789abcdef" for ch in cache_key):
        raise IdentityError("identity disc SHA-256 is malformed")

    cache_root = _require_ignored_scratch(repo_root.resolve())
    directory = cache_root / cache_key
    if directory.is_symlink():
        raise IdentityError(f"refusing derived-output symlink outside repository: {directory}")
    directory.mkdir(parents=True, exist_ok=True)
    destination = directory / "identity.json"

    selected = identity
    if destination.exists():
        try:
            existing = json.loads(destination.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as error:
            raise IdentityError(f"existing identity manifest is unreadable: {error}") from error
        if not isinstance(existing, dict):
            raise IdentityError("existing identity manifest is not a JSON object")
        selected = _merge_existing(existing, identity)

    encoded = json.dumps(selected, indent=2, sort_keys=True) + "\n"
    temporary = directory / f".identity.json.{os.getpid()}.tmp"
    temporary.write_text(encoded, encoding="utf-8")
    os.replace(temporary, destination)
    return destination


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", nargs="?", help="path to the user-owned disc image")
    parser.add_argument(
        "--xex",
        type=Path,
        help="optional extracted default.xex to fingerprint and inspect",
    )
    parser.add_argument(
        "--repo-root",
        type=Path,
        default=Path(__file__).resolve().parents[1],
        help=argparse.SUPPRESS,
    )
    parser.add_argument("--env-file", type=Path, help=argparse.SUPPRESS)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    arguments = _parser().parse_args(argv)
    repo_root = arguments.repo_root.resolve()
    try:
        resolved = resolve_image(
            arguments.image,
            repo_root,
            env_file=arguments.env_file,
        )
        xex_path = arguments.xex.resolve() if arguments.xex is not None else None
        identity = build_identity(resolved.path, xex_path)
        destination = write_identity(repo_root, identity)
        stored_identity = json.loads(destination.read_text(encoding="utf-8"))
    except (IdentityError, OSError) as error:
        print(f"title_identity: REFUSING: {error}", file=sys.stderr)
        return 2

    print(json.dumps(stored_identity, indent=2, sort_keys=True))
    print(f"identity: {destination.relative_to(repo_root)}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
