#!/usr/bin/env python3
"""Keep default builds extensionless; validate the explicitly selected SDK."""

from __future__ import annotations

import os
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
NATIVE_PC_PATH = os.environ.get("PUNTO_TEST_NATIVE_PKG_CONFIG_PATH")


class GtkAdapterBuildContract(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="punto-gtk-build-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.build = self.directory / "build"
        self.env = os.environ.copy()
        for name in ("PKG_CONFIG_PATH", "PKG_CONFIG_LIBDIR", "LD_LIBRARY_PATH",
                     "LD_PRELOAD", "CMAKE_PREFIX_PATH", "CPLUS_INCLUDE_PATH",
                     "C_INCLUDE_PATH", "LIBRARY_PATH"):
            self.env.pop(name, None)

    def run_command(self, *args: str, env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
        result = subprocess.run(args, env=env or self.env, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                timeout=90)
        if result.returncode:
            print(result.stdout, file=sys.stderr)
        return result

    def configure(self, policy: int = 0, build_id: int = 0, *extra: str,
                  env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
        return self.run_command(
            "cmake", "-S", str(ROOT / "cpp"), "-B", str(self.build), "-G", "Ninja",
            "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_TRAY=OFF", "-DBUILD_TESTING=OFF",
            f"-DPUNTO_POLICY_HASH={policy}", f"-DPUNTO_BUILD_ID={build_id}",
            *extra, env=env)

    def build_module(self, target: str = "punto-gtk-editor-adapter",
                     env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
        return self.run_command("cmake", "--build", str(self.build), "--target", target,
                                "-j2", env=env)

    def load_native_module(self) -> None:
        module = self.build / "gtk-3.0/modules/libpunto-gtk-editor-adapter.so"
        result = self.run_command(
            sys.executable, "-c",
            "import ctypes, json, os, sys; ctypes.CDLL(sys.argv[1], mode=os.RTLD_NOW); "
            "print(json.dumps(sorted({line.split()[-1] for line in open('/proc/self/maps') "
            "if os.path.basename(line.split()[-1]).startswith("
            "('libgtk-3.so', 'libgdk-3.so', 'libX11.so', 'libX11-xcb.so', 'libxcb.so'))})))",
            str(module))
        self.assertEqual(result.returncode, 0, result.stdout)
        loaded = set(json.loads(result.stdout))
        symbols = self.run_command("nm", "-D", "--undefined-only", str(module))
        self.assertEqual(symbols.returncode, 0, symbols.stdout)
        bounded = "gdk_x11_display_bounded_progress_enabled_v3"
        self.assertIn(bounded, symbols.stdout)
        provider = {**self.env, "PKG_CONFIG_PATH": str(NATIVE_PC_PATH)}
        for package, soname in (("gtk+-3.0", "libgtk-3.so.0"),
                                ("gdk-3.0", "libgdk-3.so.0"),
                                ("x11", "libX11.so.6"),
                                ("x11-xcb", "libX11-xcb.so.1"),
                                ("xcb", "libxcb.so.1")):
            directory = self.run_command("pkg-config", "--variable=libdir", package,
                                         env=provider)
            self.assertEqual(directory.returncode, 0, directory.stdout)
            expected = str((Path(directory.stdout.strip()) / soname).resolve())
            self.assertIn(expected, loaded)

    def stock_native_metadata(self) -> dict[str, str]:
        # The host has the stock runtime but may lack its development .pc file.
        # This supplies metadata only, never native declarations or symbols.
        pc = self.directory / "stock-pc"
        pc.mkdir()
        (pc / "x11-xcb.pc").write_text(
            "Name: X11 XCB stock runtime\nDescription: stock negative control\n"
            "Version: 1.8.7\nRequires: x11 xcb\nLibs: -l:libX11-xcb.so.1\n")
        return {**self.env, "PKG_CONFIG_PATH": str(pc)}

    def assert_default_graph(self) -> None:
        targets = self.run_command("cmake", "--build", str(self.build), "--target", "help")
        self.assertEqual(targets.returncode, 0, targets.stdout)
        for target in ("punto-gtk-editor-adapter", "punto-delivered-trigger-gtk-adapter"):
            self.assertNotIn(target, targets.stdout)
            built = self.build_module(target)
            self.assertNotEqual(built.returncode, 0, built.stdout)
            self.assertIn("unknown target", built.stdout)
        database = json.loads((self.build / "compile_commands.json").read_text())
        self.assertFalse(any(entry["file"].endswith(("/adapter.cpp", "/xorg_recipient.cpp"))
                             for entry in database))
        tests = self.build / "CTestTestfile.cmake"
        if tests.exists():
            self.assertNotIn("GTK_MODULES", tests.read_text())
            self.assertNotIn("punto-editor-transaction-gtk-e2e", tests.read_text())

    def test_g0_default_graph_has_no_editor_adapter(self) -> None:
        configured = self.configure(0, 0, "-DBUILD_TESTING=ON",
                                    "-DPUNTO_BUILD_DELIVERED_TRIGGER_PROTOTYPE=ON")
        self.assertEqual(configured.returncode, 0, configured.stdout)
        self.assert_default_graph()

    @unittest.skipUnless(NATIVE_PC_PATH, "native-only link control requires the selected SDK")
    def test_g1_unresolved_symbol_fails_at_actual_module_link(self) -> None:
        source = self.directory / "missing.cpp"
        source.write_text('extern "C" void punto_missing_gtk_contract_symbol();\n'
                          'extern "C" void punto_contract_link_control() {\n'
                          '  punto_missing_gtk_contract_symbol();\n}\n')
        hook = self.directory / "inject.cmake"
        hook.write_text(
            'cmake_language(DEFER CALL target_sources punto-gtk-editor-adapter '
            f'PRIVATE "{source}")\n')
        native = {**self.env, "PKG_CONFIG_PATH": str(NATIVE_PC_PATH)}
        configured = self.configure(1, 1, f"-DCMAKE_PROJECT_INCLUDE={hook}", env=native)
        self.assertEqual(configured.returncode, 0, configured.stdout)
        built = self.build_module(env=native)
        self.assertNotEqual(built.returncode, 0, built.stdout)
        self.assertIn("Linking CXX shared module", built.stdout)
        self.assertIn("undefined reference to `punto_missing_gtk_contract_symbol'", built.stdout)

    def test_g2_mixed_identity_pair_rejected_both_directions(self) -> None:
        for policy, build_id in ((0, 1), (1, 0)):
            with self.subTest(policy=policy, build_id=build_id):
                configured = self.configure(policy, build_id)
                self.assertNotEqual(configured.returncode, 0, configured.stdout)
                self.assertIn("PUNTO_POLICY_HASH and PUNTO_BUILD_ID must both", configured.stdout)

    def test_g3_cached_native_success_rejected_on_stock_then_recovery(self) -> None:
        configured = self.configure()
        self.assertEqual(configured.returncode, 0, configured.stdout)
        stock = self.stock_native_metadata()
        rejected = self.configure(1, 1, "-DPUNTO_NATIVE_RECIPIENT_ABI:INTERNAL=1", env=stock)
        self.assertNotEqual(rejected.returncode, 0, rejected.stdout)
        self.assertIn("Native recipient requires matched patched GTK/Xlib/XCB", rejected.stdout)
        recovered = self.configure()
        self.assertEqual(recovered.returncode, 0, recovered.stdout)
        self.assert_default_graph()

    @unittest.skipUnless(NATIVE_PC_PATH, "selected native SDK is checked in a separate validation lane")
    def test_g3_g4_real_native_provider_transition_and_stock_recovery(self) -> None:
        native = {**self.env, "PKG_CONFIG_PATH": str(NATIVE_PC_PATH)}
        configured = self.configure(1, 1, "-DBUILD_TESTING=ON",
                                    "-DPUNTO_BUILD_DELIVERED_TRIGGER_PROTOTYPE=ON", env=native)
        self.assertEqual(configured.returncode, 0, configured.stdout)
        self.assertEqual(self.build_module(env=native).returncode, 0)
        self.load_native_module()
        prototype = self.build_module("punto-delivered-trigger-gtk-adapter", env=native)
        self.assertEqual(prototype.returncode, 0, prototype.stdout)
        prototype_path = self.build / "prototype/gtk-3.0/modules/libpunto-delivered-trigger-gtk-adapter.so"
        self.assertEqual(self.run_command(
            sys.executable, "-c", "import ctypes, os, sys; ctypes.CDLL(sys.argv[1], mode=os.RTLD_NOW)",
            str(prototype_path)).returncode, 0)
        stock = self.stock_native_metadata()
        rejected = self.configure(1, 1, env=stock)
        self.assertNotEqual(rejected.returncode, 0, rejected.stdout)
        self.assertIn("Native recipient requires matched patched GTK/Xlib/XCB", rejected.stdout)
        recovered = self.configure(0, 0, "-DBUILD_TESTING=ON",
                                   "-DPUNTO_BUILD_DELIVERED_TRIGGER_PROTOTYPE=ON")
        self.assertEqual(recovered.returncode, 0, recovered.stdout)
        self.assertTrue(prototype_path.is_file(), "stale native artifact control")
        self.assert_default_graph()


if __name__ == "__main__":
    unittest.main()
