#!/usr/bin/env python3
"""Reports which services the authenticated image imports and which have a recovered handler.

The image's manifest names a service only by library and ordinal. This joins it
to Xenia's ordinal tables, so the migration work list is the set of exports the
title actually reaches rather than the set of handlers that happen to exist.
"""

from __future__ import annotations

import argparse
import sys
from collections import Counter
from pathlib import Path

from gearsue3_imports.inventory import Inventory, InventoryError, build

ROOT = Path(__file__).resolve().parents[1]


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--manifest",
        type=Path,
        default=ROOT / "scratch/gears1-manifest.json",
        help="x360-xex-inspect JSON for the user-supplied image",
    )
    parser.add_argument("--xenia-root", type=Path, default=ROOT / "extern/xenia")
    parser.add_argument("--runtime-root", type=Path, default=ROOT / "runtime")
    parser.add_argument(
        "--x360port-root",
        type=Path,
        default=ROOT.parents[1] / "shared/x360port",
        help="checkout whose host services claim kernel/XAM exports by name",
    )
    parser.add_argument(
        "--list",
        choices=("uncovered", "covered", "unresolved", "unused", "bound", "unbound"),
        help="print one name per line instead of the summary",
    )
    return parser.parse_args()


def report(inventory: Inventory) -> None:
    functions = inventory.functions
    variables = len(inventory.entries) - len(functions)
    print(
        f"imports: {len(inventory.entries)} "
        f"({len(functions)} function, {variables} variable)"
    )
    print(f"unresolved ordinals: {len(inventory.unresolved)} of {len(inventory.entries)}")
    print(
        f"function imports with a recovered handler: "
        f"{len(inventory.covered)} of {len(functions)}"
    )
    uncovered = Counter(entry.library for entry in inventory.uncovered)
    print(
        f"function imports with no recovered handler: {len(inventory.uncovered)} "
        f"({', '.join(f'{count} {library}' for library, count in sorted(uncovered.items())) or 'none'})"
    )
    print(
        f"recovered handlers this image never imports: {len(inventory.unused_handlers)} "
        f"of {len(inventory.recovered_handlers)}"
    )
    bound = Counter(entry.library for entry in inventory.bound)
    print(
        f"function imports a host service binds: {len(inventory.bound)} of {len(functions)} "
        f"({', '.join(f'{count} {library}' for library, count in sorted(bound.items())) or 'none'})"
    )
    print(
        f"function imports that still take the typed refusal: {len(inventory.unbound)}"
    )
    print(
        "note: a recovered handler is source the migration preserved, not a binding; "
        "a service implemented in x360port carries no recovered handler and is counted "
        "as bound, not covered"
    )


def names(inventory: Inventory, selection: str) -> list[str]:
    if selection == "unused":
        return list(inventory.unused_handlers)
    chosen = {
        "uncovered": inventory.uncovered,
        "covered": inventory.covered,
        "unresolved": inventory.unresolved,
        "bound": inventory.bound,
        "unbound": inventory.unbound,
    }[selection]
    return sorted(
        f"{entry.library} {entry.ordinal} {entry.name or '<unresolved>'}"
        for entry in chosen
    )


def main() -> int:
    selected = arguments()
    try:
        inventory = build(
            selected.manifest.resolve(),
            selected.xenia_root.resolve(),
            selected.runtime_root.resolve(),
            (selected.x360port_root.resolve() / "src", ROOT / "runtime/titles"),
        )
    except InventoryError as error:
        print(f"import inventory refusing: {error}", file=sys.stderr)
        return 2
    if selected.list is None:
        report(inventory)
        return 0
    chosen = names(inventory, selected.list)
    print(f"# {len(chosen)} {selected.list}")
    for name in chosen:
        print(name)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
