"""Joins an image's import manifest to export names and recovered handlers."""

from __future__ import annotations

import json
import re
from dataclasses import dataclass
from pathlib import Path

from .ordinal_tables import Export, OrdinalTableError, load_tables

_HANDLER = re.compile(r"^void __imp__([A-Za-z0-9_]+)", re.MULTILINE)
_CLAIMED_EXPORT = re.compile(r"\.export_name\s*=\s*\"([A-Za-z0-9_]+)\"")


class InventoryError(RuntimeError):
    """The manifest or the recovered corpus could not be read."""


@dataclass(frozen=True)
class ImportEntry:
    library: str
    ordinal: int
    kind: str
    name: str | None
    recovered: bool
    bound: bool


@dataclass(frozen=True)
class Inventory:
    entries: tuple[ImportEntry, ...]
    recovered_handlers: frozenset[str]
    claimed_exports: frozenset[str]

    @property
    def functions(self) -> tuple[ImportEntry, ...]:
        return tuple(entry for entry in self.entries if entry.kind == "function")

    @property
    def unresolved(self) -> tuple[ImportEntry, ...]:
        return tuple(entry for entry in self.entries if entry.name is None)

    @property
    def covered(self) -> tuple[ImportEntry, ...]:
        return tuple(entry for entry in self.functions if entry.recovered)

    @property
    def uncovered(self) -> tuple[ImportEntry, ...]:
        return tuple(entry for entry in self.functions if not entry.recovered)

    @property
    def bound(self) -> tuple[ImportEntry, ...]:
        """Function imports a host service claims by name, so they reach it."""
        return tuple(entry for entry in self.functions if entry.bound)

    @property
    def unbound(self) -> tuple[ImportEntry, ...]:
        return tuple(entry for entry in self.functions if not entry.bound)

    @property
    def unused_handlers(self) -> tuple[str, ...]:
        needed = {entry.name for entry in self.functions if entry.name is not None}
        return tuple(sorted(self.recovered_handlers - needed))


def read_recovered_handlers(runtime_root: Path) -> frozenset[str]:
    """Names the recovered service corpus implements.

    Refuses on an empty or missing directory: "no handlers" and "I looked in the
    wrong place" print the same way otherwise.
    """
    if not runtime_root.is_dir():
        raise InventoryError(f"no runtime source directory at {runtime_root}")
    sources = sorted(runtime_root.glob("*.cpp"))
    if not sources:
        raise InventoryError(f"no C++ sources under {runtime_root}")
    names: set[str] = set()
    for source in sources:
        names.update(_HANDLER.findall(source.read_text(encoding="utf-8")))
    return frozenset(names)


def read_claimed_exports(service_roots: tuple[Path, ...]) -> frozenset[str]:
    """Export names the host services claim, read from their own sources.

    Refuses on a missing directory or a tree that claims nothing: a service
    layer that binds no export and one this tool failed to find would otherwise
    print the same count.
    """
    names: set[str] = set()
    for root in service_roots:
        if not root.is_dir():
            raise InventoryError(f"no host-service source directory at {root}")
        for source in sorted(root.rglob("*.cpp")):
            names.update(_CLAIMED_EXPORT.findall(source.read_text(encoding="utf-8")))
    if not names:
        roots = ", ".join(str(root) for root in service_roots)
        raise InventoryError(f"no claimed export names found under {roots}")
    return frozenset(names)


def build(
    manifest_path: Path,
    xenia_root: Path,
    runtime_root: Path,
    service_roots: tuple[Path, ...],
) -> Inventory:
    if not manifest_path.is_file():
        raise InventoryError(
            f"no import manifest at {manifest_path}; produce one with x360-xex-inspect"
        )
    document = json.loads(manifest_path.read_text(encoding="utf-8"))
    imports = document.get("imports")
    if not imports:
        raise InventoryError(f"{manifest_path} declares no imports")
    try:
        tables = load_tables(xenia_root)
    except OrdinalTableError as error:
        raise InventoryError(str(error)) from error
    handlers = read_recovered_handlers(runtime_root)
    claimed = read_claimed_exports(service_roots)

    entries: list[ImportEntry] = []
    for entry in imports:
        library = entry["library"]
        ordinal = int(entry["ordinal"])
        export: Export | None = tables.get(library, {}).get(ordinal)
        name = export.name if export is not None else None
        entries.append(
            ImportEntry(
                library=library,
                ordinal=ordinal,
                kind=entry["kind"],
                name=name,
                recovered=name is not None and name in handlers,
                bound=name is not None and name in claimed,
            )
        )
    return Inventory(
        entries=tuple(entries), recovered_handlers=handlers, claimed_exports=claimed
    )
