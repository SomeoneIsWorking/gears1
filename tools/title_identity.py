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
3. exactly one supported image or 7z archive in the ignored ``roms/`` drop-in directory.

A selected but missing higher-priority path is an error. It never falls back to
a different image, because silently recompiling another title or revision would
associate generated code with the wrong executable.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
import tempfile
from collections.abc import Callable, Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path

ENV_IMAGE = "GEARS_ISO"
DROP_IN_DIRECTORY = "roms"
SUPPORTED_DROP_IN_SUFFIXES = frozenset({".7z", ".img", ".iso", ".xgd", ".xiso"})
HASH_CHUNK_SIZE = 1024 * 1024
IDENTITY_SCHEMA = 1
XEX_INSPECT_SCHEMA = 1
XEX_INSPECT_ENV = "XEX_INSPECT"
XEX_INSPECT_DEFAULT = Path("build/deps/xenonrecomp/XexInspect/xex-inspect")

XGD_SECTOR_SIZE = 2048
XGD_VOLUME_DESCRIPTOR_SECTOR = 32
XGD_VOLUME_MAGIC = b"MICROSOFT*XBOX*MEDIA"
XGD_CANDIDATE_PARTITION_OFFSETS = (0, 0x0FD90000, 0x02080000, 0x18300000)

XEX_INSPECT_HELPERS = frozenset(
    {
        "restgprlr_14",
        "savegprlr_14",
        "restfpr_14",
        "savefpr_14",
        "restvmx_14",
        "savevmx_14",
        "restvmx_64",
        "savevmx_64",
    }
)


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
        return ResolvedImage(
            _require_regular_file(path, "explicit disc image"), "explicit"
        )

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


def resolve_xex_inspect(
    repo_root: Path,
    explicit: str | os.PathLike[str] | None = None,
    environ: Mapping[str, str] | None = None,
) -> Path:
    """Resolve the checked XEX inspector without searching machine-specific paths."""

    process_environment = os.environ if environ is None else environ
    configured = explicit
    description = "explicit xex-inspect executable"
    if configured is None:
        configured = process_environment.get(XEX_INSPECT_ENV, XEX_INSPECT_DEFAULT)
        description = f"{XEX_INSPECT_ENV} executable"
    path = _absolute(Path(configured), repo_root.resolve())
    _require_regular_file(path, description)
    if not os.access(path, os.X_OK):
        raise IdentityError(f"{description} is not executable: {path}")
    return path


def _strict_object(
    value: object, expected_keys: set[str] | frozenset[str], description: str
) -> dict[str, object]:
    if type(value) is not dict:
        raise IdentityError(f"xex-inspect {description} is not a JSON object")
    result = value
    actual_keys = set(result)
    if actual_keys != set(expected_keys):
        missing = sorted(set(expected_keys) - actual_keys)
        unexpected = sorted(actual_keys - set(expected_keys))
        details = []
        if missing:
            details.append(f"missing {', '.join(missing)}")
        if unexpected:
            details.append(f"unexpected {', '.join(unexpected)}")
        raise IdentityError(
            f"xex-inspect {description} schema is invalid: {'; '.join(details)}"
        )
    return result


def _strict_list(value: object, description: str) -> list[object]:
    if type(value) is not list:
        raise IdentityError(f"xex-inspect {description} is not a JSON array")
    return value


def _strict_uint(value: object, maximum: int, description: str) -> int:
    if type(value) is not int or not 0 <= value <= maximum:
        raise IdentityError(
            f"xex-inspect {description} is not a valid unsigned integer"
        )
    return value


def _strict_hex32(value: object, description: str) -> int:
    if (
        type(value) is not str
        or len(value) != 8
        or any(character not in "0123456789abcdef" for character in value)
    ):
        raise IdentityError(
            f"xex-inspect {description} is not an eight-digit lowercase hex value"
        )
    return int(value, 16)


def _strict_sha256(value: object, description: str) -> str:
    if (
        type(value) is not str
        or len(value) != 64
        or any(character not in "0123456789abcdef" for character in value)
    ):
        raise IdentityError(f"xex-inspect {description} is not a lowercase SHA-256")
    return value


def _reject_duplicate_json_keys(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise IdentityError(
                f"xex-inspect JSON contains ambiguous duplicate key {key!r}"
            )
        result[key] = value
    return result


def _validate_inspection(
    value: object,
    xex_identity: dict[str, int | str],
    image_identity: dict[str, int | str],
) -> dict[str, object]:
    inspection = _strict_object(
        value,
        {
            "schema",
            "format",
            "xex",
            "execution",
            "image",
            "sections",
            "imports",
            "helpers",
        },
        "document",
    )
    if inspection["schema"] != XEX_INSPECT_SCHEMA:
        raise IdentityError(f"unsupported xex-inspect schema: {inspection['schema']!r}")
    if inspection["format"] != "XEX2":
        raise IdentityError(
            f"xex-inspect returned unexpected format: {inspection['format']!r}"
        )

    inspected_xex = _strict_object(
        inspection["xex"], {"sha256", "size"}, "xex identity"
    )
    _strict_sha256(inspected_xex["sha256"], "xex SHA-256")
    _strict_uint(inspected_xex["size"], sys.maxsize, "xex size")
    if inspected_xex != xex_identity:
        raise IdentityError(
            "xex-inspect metadata does not identify the selected default.xex"
        )

    execution = _strict_object(
        inspection["execution"],
        {
            "title_id",
            "media_id",
            "version",
            "base_version",
            "platform",
            "executable_table",
            "disc_number",
            "disc_count",
            "savegame_id",
        },
        "execution metadata",
    )
    for key in ("title_id", "media_id", "version", "base_version", "savegame_id"):
        _strict_hex32(execution[key], f"execution {key}")
    for key in ("platform", "executable_table", "disc_number", "disc_count"):
        _strict_uint(execution[key], 0xFF, f"execution {key}")

    image = _strict_object(
        inspection["image"], {"sha256", "base", "size", "entry"}, "image"
    )
    _strict_sha256(image["sha256"], "image SHA-256")
    _strict_hex32(image["base"], "image base")
    image_size = _strict_uint(image["size"], 0xFFFFFFFF, "image size")
    _strict_hex32(image["entry"], "image entry")
    if {"sha256": image["sha256"], "size": image_size} != image_identity:
        raise IdentityError(
            "xex-inspect image metadata does not identify its emitted image"
        )

    sections = _strict_list(inspection["sections"], "sections")
    for index, raw_section in enumerate(sections):
        section = _strict_object(
            raw_section, {"name", "base", "size", "code"}, f"section {index}"
        )
        if type(section["name"]) is not str:
            raise IdentityError(f"xex-inspect section {index} name is invalid")
        _strict_hex32(section["base"], f"section {index} base")
        _strict_uint(section["size"], 0xFFFFFFFF, f"section {index} size")
        if type(section["code"]) is not bool:
            raise IdentityError(f"xex-inspect section {index} code flag is invalid")

    imports = _strict_list(inspection["imports"], "imports")
    for index, raw_import in enumerate(imports):
        imported = _strict_object(
            raw_import,
            {"kind", "library", "ordinal", "name", "address", "record_address"},
            f"import {index}",
        )
        if imported["kind"] not in {"function", "variable"}:
            raise IdentityError(f"xex-inspect import {index} kind is invalid")
        if type(imported["library"]) is not str:
            raise IdentityError(f"xex-inspect import {index} library is invalid")
        if type(imported["name"]) is not str:
            raise IdentityError(f"xex-inspect import {index} name is invalid")
        _strict_uint(imported["ordinal"], 0xFFFFFFFF, f"import {index} ordinal")
        for key in ("address", "record_address"):
            _strict_hex32(imported[key], f"import {index} {key}")

    helpers = _strict_object(inspection["helpers"], XEX_INSPECT_HELPERS, "helpers")
    for name, raw_addresses in helpers.items():
        addresses = _strict_list(raw_addresses, f"helper {name}")
        for raw_address in addresses:
            _strict_hex32(raw_address, f"helper {name} address")

    return inspection


def parse_xex_metadata(
    path: Path,
    repo_root: Path | None = None,
    xex_inspect: str | os.PathLike[str] | None = None,
    environ: Mapping[str, str] | None = None,
    runner: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
) -> dict[str, object]:
    """Inspect ``default.xex`` through the single checked XenonUtils authority."""

    _require_regular_file(path, "extracted default.xex")
    if path.name.lower() != "default.xex":
        raise IdentityError(f"XEX input must be named default.xex, not {path.name!r}")
    root = (
        Path(__file__).resolve().parents[1]
        if repo_root is None
        else repo_root.resolve()
    )
    executable = resolve_xex_inspect(root, xex_inspect, environ)
    scratch = _require_ignored_scratch(root).parent
    scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(
        prefix=".title-identity-", dir=scratch
    ) as temporary:
        image_path = Path(temporary) / "image.bin"
        try:
            completed = runner(
                [executable, path.resolve(), "--image-out", image_path],
                capture_output=True,
                check=False,
                text=True,
            )
        except OSError as error:
            raise IdentityError(f"could not execute xex-inspect: {error}") from error
        if completed.returncode != 0:
            detail = completed.stderr.strip()
            suffix = f": {detail}" if detail else ""
            raise IdentityError(f"xex-inspect refused the selected default.xex{suffix}")
        try:
            document = json.loads(
                completed.stdout, object_pairs_hook=_reject_duplicate_json_keys
            )
        except json.JSONDecodeError as error:
            raise IdentityError(
                f"xex-inspect returned invalid JSON: {error}"
            ) from error
        xex_identity = fingerprint(path)
        if image_path.is_symlink():
            raise IdentityError("xex-inspect emitted image is an unexpected symlink")
        image_identity = fingerprint(image_path)
        inspection = _validate_inspection(document, xex_identity, image_identity)

    return {
        key: value for key, value in inspection.items() if key not in {"schema", "xex"}
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
            raise IdentityError(
                f"refusing derived-output symlink outside repository: {path}"
            )
    return titles


def build_identity(
    disc_path: Path,
    xex_path: Path | None = None,
    *,
    repo_root: Path | None = None,
    xex_inspect: str | os.PathLike[str] | None = None,
    environ: Mapping[str, str] | None = None,
    runner: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
) -> dict[str, object]:
    disc = fingerprint(disc_path)
    disc.update(inspect_disc(disc_path))
    identity: dict[str, object] = {
        "schema": IDENTITY_SCHEMA,
        "disc": disc,
    }
    if xex_path is not None:
        xex = fingerprint(xex_path)
        xex["metadata"] = parse_xex_metadata(
            xex_path,
            repo_root=repo_root,
            xex_inspect=xex_inspect,
            environ=environ,
            runner=runner,
        )
        identity["xex"] = xex
    return identity


def _merge_existing(
    existing: dict[str, object], requested: dict[str, object]
) -> dict[str, object]:
    if (
        existing.get("schema") != IDENTITY_SCHEMA
        or existing.get("disc") != requested["disc"]
    ):
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
        raise IdentityError(
            f"refusing derived-output symlink outside repository: {directory}"
        )
    directory.mkdir(parents=True, exist_ok=True)
    destination = directory / "identity.json"

    selected = identity
    if destination.exists():
        try:
            existing = json.loads(destination.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as error:
            raise IdentityError(
                f"existing identity manifest is unreadable: {error}"
            ) from error
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
        "--xex-inspect",
        type=Path,
        help=(
            "xex-inspect executable (default: XEX_INSPECT or "
            "build/deps/xenonrecomp/XexInspect/xex-inspect)"
        ),
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
        identity = build_identity(
            resolved.path,
            xex_path,
            repo_root=repo_root,
            xex_inspect=arguments.xex_inspect,
        )
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
