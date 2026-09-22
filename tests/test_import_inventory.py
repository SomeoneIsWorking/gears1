"""Import-service inventory contract tests.

Every case is synthetic: restricted title bytes never become a test input, and a
fixture the test writes itself is the only way to assert what a MISS prints.
"""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "tools"))

from gearsue3_imports.inventory import (
    InventoryError,
    build,
    read_claimed_exports,
    read_recovered_handlers,
)
from gearsue3_imports.ordinal_tables import OrdinalTableError, load_tables

KERNEL_TABLE = Path("src/xenia/kernel/xboxkrnl/xboxkrnl_table.inc")
XAM_TABLE = Path("src/xenia/kernel/xam/xam_table.inc")


class ImportInventoryTests(unittest.TestCase):
    def setUp(self) -> None:
        scratch = REPO_ROOT / "scratch"
        scratch.mkdir(exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(
            prefix="import-inventory-test-", dir=scratch
        )
        self.root = Path(self.temporary.name)
        self.xenia = self.root / "xenia"
        self.runtime = self.root / "runtime"
        self.runtime.mkdir(parents=True)
        self.services = self.root / "services"
        self.services.mkdir(parents=True)
        self.write_claims(["XGetAVPack"])
        self.write_tables(
            kernel=[(0xCC, "NtAllocateVirtualMemory", "Function"),
                    (0x0E, "ExEventObjectType", "Variable")],
            xam=[(971, "XGetAVPack", "Function")],
        )
        self.write_handlers("KernelStub", ["NtAllocateVirtualMemory", "NeverImported"])

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def write_tables(self, kernel: list[tuple[int, str, str]],
                     xam: list[tuple[int, str, str]]) -> None:
        for relative, rows in ((KERNEL_TABLE, kernel), (XAM_TABLE, xam)):
            path = self.xenia / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(
                "".join(
                    f"XE_EXPORT(module, {ordinal:#010x}, {name}, k{kind}),\n"
                    for ordinal, name, kind in rows
                ),
                encoding="utf-8",
            )

    def write_handlers(self, stem: str, names: list[str]) -> None:
        (self.runtime / f"{stem}.cpp").write_text(
            "".join(f"void __imp__{name}(PPCContext &ctx, uint8_t *base) {{}}\n"
                    for name in names),
            encoding="utf-8",
        )

    def write_claims(self, names: list[str]) -> None:
        (self.services / "claims.cpp").write_text(
            "".join(
                f'ImportClaim{{.export_name = "{name}", .handler = Handler}},\n'
                for name in names
            ),
            encoding="utf-8",
        )

    def write_manifest(self, imports: list[dict[str, object]]) -> Path:
        path = self.root / "manifest.json"
        path.write_text(json.dumps({"imports": imports}), encoding="utf-8")
        return path

    def test_joins_manifest_to_export_names_and_recovered_handlers(self) -> None:
        manifest = self.write_manifest(
            [
                {"kind": "function", "library": "xboxkrnl.exe", "ordinal": 0xCC},
                {"kind": "function", "library": "xam.xex", "ordinal": 971},
                {"kind": "variable", "library": "xboxkrnl.exe", "ordinal": 0x0E},
            ]
        )
        inventory = build(manifest, self.xenia, self.runtime, (self.services,))
        self.assertEqual(len(inventory.entries), 3)
        self.assertEqual(len(inventory.functions), 2)
        self.assertEqual(inventory.unresolved, ())
        self.assertEqual(
            [entry.name for entry in inventory.covered], ["NtAllocateVirtualMemory"]
        )
        self.assertEqual([entry.name for entry in inventory.uncovered], ["XGetAVPack"])
        self.assertEqual(inventory.unused_handlers, ("NeverImported",))
        # A service claims the export the recovered corpus never covered, so the
        # two answers must not be read off each other.
        self.assertEqual([entry.name for entry in inventory.bound], ["XGetAVPack"])
        self.assertEqual(
            [entry.name for entry in inventory.unbound], ["NtAllocateVirtualMemory"]
        )

    def test_unknown_ordinal_is_reported_not_silently_dropped(self) -> None:
        manifest = self.write_manifest(
            [{"kind": "function", "library": "xboxkrnl.exe", "ordinal": 0x7777}]
        )
        inventory = build(manifest, self.xenia, self.runtime, (self.services,))
        self.assertEqual(len(inventory.unresolved), 1)
        self.assertIsNone(inventory.unresolved[0].name)
        self.assertEqual(inventory.covered, ())

    def test_missing_manifest_refuses(self) -> None:
        with self.assertRaises(InventoryError) as raised:
            build(self.root / "absent.json", self.xenia, self.runtime, (self.services,))
        self.assertIn("no import manifest", str(raised.exception))

    def test_manifest_without_imports_refuses(self) -> None:
        with self.assertRaises(InventoryError) as raised:
            build(self.write_manifest([]), self.xenia, self.runtime, (self.services,))
        self.assertIn("declares no imports", str(raised.exception))

    def test_missing_ordinal_table_refuses(self) -> None:
        (self.xenia / KERNEL_TABLE).unlink()
        with self.assertRaises(OrdinalTableError) as raised:
            load_tables(self.xenia)
        self.assertIn("no ordinal table", str(raised.exception))

    def test_ordinal_table_that_parses_to_nothing_refuses(self) -> None:
        (self.xenia / XAM_TABLE).write_text("// only a comment\n", encoding="utf-8")
        with self.assertRaises(OrdinalTableError) as raised:
            load_tables(self.xenia)
        self.assertIn("declared no exports", str(raised.exception))

    def test_runtime_root_without_sources_refuses(self) -> None:
        empty = self.root / "empty"
        empty.mkdir()
        with self.assertRaises(InventoryError) as raised:
            read_recovered_handlers(empty)
        self.assertIn("no C++ sources", str(raised.exception))

    def test_service_root_that_claims_nothing_refuses(self) -> None:
        self.write_claims([])
        with self.assertRaises(InventoryError) as raised:
            read_claimed_exports((self.services,))
        self.assertIn("no claimed export names", str(raised.exception))

    def test_absent_service_root_refuses(self) -> None:
        with self.assertRaises(InventoryError) as raised:
            read_claimed_exports((self.root / "absent",))
        self.assertIn("no host-service source directory", str(raised.exception))

    def test_absent_runtime_root_refuses(self) -> None:
        with self.assertRaises(InventoryError) as raised:
            read_recovered_handlers(self.root / "absent")
        self.assertIn("no runtime source directory", str(raised.exception))


if __name__ == "__main__":
    unittest.main()
