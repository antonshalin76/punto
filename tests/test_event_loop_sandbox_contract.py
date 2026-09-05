#!/usr/bin/env python3
"""Run the real GTK fixture under clean and preconfigured read-only hosts."""

import importlib.util
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest


RUNNER = pathlib.Path(__file__).with_name("test_event_loop_gtk_e2e.py")


def check_inner_mounts() -> None:
    assert os.statvfs("/etc").f_flag & os.ST_RDONLY
    assert os.statvfs("/etc/passwd").f_flag & os.ST_RDONLY
    assert not os.statvfs("/etc/punto").f_flag & os.ST_RDONLY
    assert os.readlink("/etc/punto-contract-link") == "passwd"
    assert not pathlib.Path("/etc/punto/host-marker").exists()
    probe = pathlib.Path("/etc/punto/private-probe")
    probe.write_bytes(b"private fixture")
    assert probe.read_bytes() == b"private fixture"
    probe.unlink()
    os.execv(sys.executable, [sys.executable, str(RUNNER), sys.argv[1]])


def load_runner():
    spec = importlib.util.spec_from_file_location("gtk_fixture", RUNNER)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def run_fixture(driver: pathlib.Path) -> int:
    module = load_runner()
    # Bootstrap mount assertions inside the helper's actual sandbox, then exec
    # the unchanged GTK runner and real driver with the same private mounts.
    module.__file__ = __file__
    return module.run_in_sandbox(driver)


class SandboxContract(unittest.TestCase):
    def test_missing_xprop_is_reported_before_gui_start(self) -> None:
        result = subprocess.run([
            "bwrap", "--unshare-all", "--die-with-parent", "--ro-bind", "/", "/",
            "--tmpfs", "/tmp",
            "--ro-bind", "/dev/null", "/usr/bin/xprop", "--setenv", "PATH", "/usr/bin:/bin",
            sys.executable, str(pathlib.Path(__file__).resolve()), "--missing-xprop",
        ], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("xprop", result.stdout.splitlines())

    def assert_host_layout(self, existing_punto: bool) -> None:
        with tempfile.TemporaryDirectory(prefix="punto-sandbox-contract-") as temporary:
            fixture = pathlib.Path(temporary)
            punto = fixture / "punto"
            punto.mkdir()
            marker = punto / "host-marker"
            marker.write_bytes(b"host config must not change")
            marker.chmod(0o640)
            before = (marker.read_bytes(), marker.stat().st_mode)
            command = [
                "bwrap", "--unshare-all", "--die-with-parent", "--ro-bind", "/", "/",
                "--proc", "/proc", "--dev", "/dev", "--tmpfs", "/tmp", "--tmpfs", "/etc",
            ]
            for name in ("fonts", "X11", "ld.so.cache", "ld.so.conf", "ld.so.conf.d",
                         "alternatives", "passwd", "group"):
                command.extend(("--ro-bind", "/etc/" + name, "/etc/" + name))
            command.extend(("--symlink", "passwd", "/etc/punto-contract-link"))
            if existing_punto:
                command.extend(("--ro-bind", str(punto), "/etc/punto"))
            command.extend((
                "--remount-ro", "/etc", "--setenv", "PUNTO_REQUIRE_EVENT_LOOP_E2E", "1",
                "--setenv", "PUNTO_EVENT_LOOP_E2E_TEST", "test_initial_user_config_sets_auto_default",
                sys.executable, str(pathlib.Path(__file__).resolve()), "--outer", str(DRIVER),
            ))
            result = subprocess.run(command, capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("Ran 1 test", result.stderr)
            self.assertEqual((marker.read_bytes(), marker.stat().st_mode), before)
            self.assertEqual(list(punto.iterdir()), [marker])

    def test_host_without_punto_directory(self) -> None:
        self.assert_host_layout(False)

    def test_existing_host_configuration_is_untouched(self) -> None:
        self.assert_host_layout(True)


if __name__ == "__main__":
    if sys.argv[1:] == ["--missing-xprop"]:
        assert shutil.which("xprop") is None
        print("\n".join(load_runner().missing_runtime()))
    elif os.environ.get("PUNTO_EVENT_LOOP_E2E_INNER") == "1":
        check_inner_mounts()
    elif len(sys.argv) == 3 and sys.argv[1] == "--outer":
        raise SystemExit(run_fixture(pathlib.Path(sys.argv[2])))
    else:
        if len(sys.argv) != 2:
            raise SystemExit("usage: test_event_loop_sandbox_contract.py DRIVER")
        DRIVER = pathlib.Path(sys.argv[1]).resolve()
        missing = load_runner().missing_runtime()
        if missing:
            print("Missing sandbox runtime: " + ", ".join(missing), file=sys.stderr)
            raise SystemExit(1 if os.environ.get("PUNTO_REQUIRE_EVENT_LOOP_E2E") == "1" else 77)
        unittest.main(argv=[sys.argv[0]], verbosity=2)
