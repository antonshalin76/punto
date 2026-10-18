#!/usr/bin/env python3
"""Native-only omission must not hide missing ordinary source coverage."""

from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


SELECTOR = Path(__file__).with_name("select_clang_tidy_commands.py")


class ClangTidyInventoryContract(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory(prefix="punto-tidy-inventory-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.sources = self.root / "src"
        self.sources.mkdir()
        for name in ("main.cpp", "xorg_recipient.cpp"):
            (self.sources / name).touch()
        self.product = self.root / "product.json"
        self.contract = self.root / "contract.json"
        self.output = self.root / "selected.json"
        self.contract.write_text("[]\n")
        self.write_database(native=False)

    def write_database(self, *, native: bool) -> None:
        entries = []
        for name, target in (("main.cpp", "punto"),
                             ("xorg_recipient.cpp", "punto-gtk-editor-adapter")):
            if name == "xorg_recipient.cpp" and not native:
                continue
            entries.append({"directory": str(self.root),
                            "file": str(self.sources / name),
                            "output": f"CMakeFiles/{target}.dir/{name}.o",
                            "command": "c++ -c " + str(self.sources / name)})
        self.product.write_text(json.dumps(entries))

    def select(self, profile: str = "ordinary") -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(SELECTOR), "--source-root", str(self.sources),
             "--product-db", str(self.product), "--contract-db", str(self.contract),
             "--output", str(self.output), "--profile", profile],
            text=True, capture_output=True, timeout=5)

    def test_ordinary_omits_only_exact_native_source(self) -> None:
        result = self.select()
        self.assertEqual(result.returncode, 0, result.stderr)
        selected = json.loads(self.output.read_text())
        self.assertEqual([Path(entry["file"]).name for entry in selected], ["main.cpp"])

    def test_other_missing_source_is_not_hidden(self) -> None:
        (self.sources / "ordinary_missing.cpp").touch()
        result = self.select()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("ordinary_missing.cpp", result.stderr)

    def test_nested_same_name_is_not_a_native_exception(self) -> None:
        nested = self.sources / "other"
        nested.mkdir()
        (nested / "xorg_recipient.cpp").touch()
        result = self.select()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(str(nested / "xorg_recipient.cpp"), result.stderr)

    def test_native_requires_the_real_compile_command(self) -> None:
        result = self.select("native")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("xorg_recipient.cpp", result.stderr)
        self.write_database(native=True)
        result = self.select("native")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(json.loads(self.output.read_text())), 2)

    def test_missing_ordinary_command_fails_even_in_native_profile(self) -> None:
        self.write_database(native=True)
        entries = json.loads(self.product.read_text())
        self.product.write_text(json.dumps(entries[1:]))
        result = self.select("native")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("main.cpp", result.stderr)


if __name__ == "__main__":
    unittest.main()
