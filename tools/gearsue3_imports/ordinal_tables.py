"""Reads Xenia's kernel and XAM ordinal tables.

The tables are the same authority `x360port::ExportNames` compiles into the
runtime, so a name this tool prints is the name a claim must spell.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path

KERNEL_LIBRARY = "xboxkrnl.exe"
XAM_LIBRARY = "xam.xex"

_TABLES = {
    KERNEL_LIBRARY: Path("src/xenia/kernel/xboxkrnl/xboxkrnl_table.inc"),
    XAM_LIBRARY: Path("src/xenia/kernel/xam/xam_table.inc"),
}

_EXPORT = re.compile(r"XE_EXPORT\(\s*\w+,\s*(0x[0-9A-Fa-f]+),\s*(\w+),\s*k(\w+)\)")


class OrdinalTableError(RuntimeError):
    """A table could not be read, or declared nothing."""


@dataclass(frozen=True)
class Export:
    ordinal: int
    name: str
    kind: str


def load_tables(xenia_root: Path) -> dict[str, dict[int, Export]]:
    """Returns every library's ordinal-to-export map.

    Refuses rather than returning an empty map: a table whose path moved would
    otherwise make every import look unknown, which reads like a real finding.
    """
    tables: dict[str, dict[int, Export]] = {}
    for library, relative in _TABLES.items():
        path = xenia_root / relative
        if not path.is_file():
            raise OrdinalTableError(f"{library}: no ordinal table at {path}")
        exports: dict[int, Export] = {}
        for line in path.read_text(encoding="utf-8").splitlines():
            match = _EXPORT.match(line.strip())
            if match is None:
                continue
            ordinal = int(match.group(1), 16)
            exports[ordinal] = Export(ordinal, match.group(2), match.group(3).lower())
        if not exports:
            raise OrdinalTableError(
                f"{library}: {path} declared no exports; its format has changed"
            )
        tables[library] = exports
    return tables
