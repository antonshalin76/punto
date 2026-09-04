#!/usr/bin/env python3

import os
import pathlib
import select
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest
import warnings


SKIP_EXIT = 77
START_TIMEOUT = 3.0
EVENT_TIMEOUT = 2.0

Gtk = None
Gdk = None
GLib = None
Vte = None


def missing_runtime() -> list[str]:
    missing = [name for name in ("Xvfb", "xdotool") if shutil.which(name) is None]
    probe = (
        "import gi; "
        "gi.require_version('Gtk', '3.0'); "
        "gi.require_version('Gdk', '3.0'); "
        "gi.require_version('Vte', '2.91'); "
        "from gi.repository import Gtk, Gdk, Vte"
    )
    try:
        result = subprocess.run(
            [sys.executable, "-c", probe],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            timeout=5,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired):
        result = None
    if result is None or result.returncode != 0:
        missing.append("Python gi Gtk3/Vte 2.91")
    return missing


class NestedX11:
    def __init__(self) -> None:
        self.process: subprocess.Popen[bytes] | None = None
        self.display = ""

    def start(self) -> None:
        read_fd, write_fd = os.pipe()
        try:
            try:
                self.process = subprocess.Popen(
                    [
                        shutil.which("Xvfb") or "Xvfb",
                        "-displayfd",
                        str(write_fd),
                        "-screen",
                        "0",
                        "1024x768x24",
                        "-ac",
                    "-nolisten",
                    "tcp",
                    "-terminate",
                    ],
                    pass_fds=(write_fd,),
                    stdin=subprocess.DEVNULL,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                )
            except Exception:
                os.close(read_fd)
                raise
        finally:
            os.close(write_fd)

        ready, _, _ = select.select([read_fd], [], [], START_TIMEOUT)
        if not ready:
            os.close(read_fd)
            self.stop()
            raise RuntimeError("nested Xvfb startup timed out")
        number = os.read(read_fd, 32).decode("ascii", errors="strict").strip()
        os.close(read_fd)
        if not number.isdecimal() or self.process.poll() is not None:
            self.stop()
            raise RuntimeError("nested Xvfb did not publish a display")
        self.display = f":{number}"

    def stop(self) -> None:
        if self.process is None or self.process.poll() is not None:
            return
        self.process.terminate()
        try:
            self.process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=2)


class DesktopEditorE2E(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        absent = missing_runtime()
        if absent:
            raise unittest.SkipTest("missing runtime: " + ", ".join(absent))

        cls.saved_environment = {
            name: os.environ.get(name)
            for name in ("DISPLAY", "GDK_BACKEND", "NO_AT_BRIDGE")
        }
        cls.x11 = NestedX11()
        cls.x11.start()
        os.environ["DISPLAY"] = cls.x11.display
        os.environ["GDK_BACKEND"] = "x11"
        os.environ["NO_AT_BRIDGE"] = "1"

        global Gtk, Gdk, GLib, Vte
        import gi

        gi.require_version("Gtk", "3.0")
        gi.require_version("Gdk", "3.0")
        gi.require_version("Vte", "2.91")
        from gi.repository import Gdk as imported_gdk
        from gi.repository import GLib as imported_glib
        from gi.repository import Gtk as imported_gtk
        from gi.repository import Vte as imported_vte

        Gtk = imported_gtk
        Gdk = imported_gdk
        GLib = imported_glib
        Vte = imported_vte
        initialized, _ = Gtk.init_check([])
        if not initialized:
            cls.x11.stop()
            cls.restore_environment()
            raise RuntimeError("GTK could not open the nested X display")

    @classmethod
    def tearDownClass(cls) -> None:
        while Gtk is not None and Gtk.events_pending():
            Gtk.main_iteration_do(False)
        cls.x11.stop()
        cls.restore_environment()

    @classmethod
    def restore_environment(cls) -> None:
        for name, value in cls.saved_environment.items():
            if value is None:
                os.environ.pop(name, None)
            else:
                os.environ[name] = value

    def pump_until(
        self, predicate, description: str, timeout: float = EVENT_TIMEOUT
    ) -> None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            while Gtk.events_pending():
                Gtk.main_iteration_do(False)
            if predicate():
                return
            time.sleep(0.005)
        self.fail(f"timed out waiting for {description}")

    def pump_for(self, duration: float) -> None:
        deadline = time.monotonic() + duration
        while time.monotonic() < deadline:
            while Gtk.events_pending():
                Gtk.main_iteration_do(False)
            time.sleep(0.005)

    def clipboard_text(self, selection) -> str | None:
        values: list[str | None] = []

        def received(_clipboard, text, _data) -> None:
            values.append(text)

        Gtk.Clipboard.get(selection).request_text(received, None)
        self.pump_until(lambda: bool(values), "X11 selection response")
        return values[0]

    def set_selection(self, selection, text: str) -> None:
        Gtk.Clipboard.get(selection).set_text(text, -1)
        Gdk.flush()
        self.pump_for(0.02)

    def xdo(self, *arguments: str) -> None:
        environment = {
            "DISPLAY": self.x11.display,
            "PATH": os.environ.get("PATH", "/usr/bin:/bin"),
        }
        result = subprocess.run(
            [shutil.which("xdotool") or "xdotool", *arguments],
            env=environment,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=EVENT_TIMEOUT,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr.strip())

    def focus(self, window) -> None:
        self.pump_until(
            lambda: window.get_window() is not None,
            "GTK window realization",
        )
        self.xdo("windowfocus", "--sync", str(window.get_window().get_xid()))
        self.pump_for(0.02)

    def destroy_window(self, window) -> None:
        window.destroy()
        self.pump_for(0.02)

    def test_gtk_editable_native_ctrl_v_replacement_and_edges(self) -> None:
        window = Gtk.Window(title="punto-gtk-editable-e2e")
        entry = Gtk.Entry()
        window.add(entry)
        window.show_all()
        entry.grab_focus()
        self.addCleanup(self.destroy_window, window)
        self.focus(window)

        self.set_selection(Gdk.SELECTION_CLIPBOARD, "saved clipboard")
        self.set_selection(Gdk.SELECTION_PRIMARY, "saved primary")
        original = "head source source tail"
        source_start = original.rindex("source")
        entry.set_text(original)
        entry.select_region(source_start, source_start + len("source"))
        self.pump_until(
            lambda: self.clipboard_text(Gdk.SELECTION_PRIMARY) == "source",
            "GTK source selection ownership",
        )
        self.assertEqual(
            entry.get_selection_bounds(), (source_start, source_start + 6)
        )

        replacement = "target target"
        self.set_selection(Gdk.SELECTION_CLIPBOARD, replacement)
        self.assertEqual(self.clipboard_text(Gdk.SELECTION_PRIMARY), "source")
        self.xdo("key", "--clearmodifiers", "ctrl+v")
        self.pump_until(
            lambda: entry.get_text() == "head source target target tail",
            "native GTK selection replacement",
        )
        self.assertEqual(entry.get_selection_bounds(), ())
        # Punto can now restore PRIMARY without disturbing the source range,
        # because the native paste has already consumed and collapsed it.
        self.set_selection(Gdk.SELECTION_PRIMARY, "saved primary")
        self.assertEqual(self.clipboard_text(Gdk.SELECTION_CLIPBOARD), replacement)
        self.assertEqual(
            self.clipboard_text(Gdk.SELECTION_PRIMARY), "saved primary"
        )

        entry.set_text("headtail")
        entry.set_position(4)
        no_selection_payload = "source source"
        self.set_selection(Gdk.SELECTION_CLIPBOARD, no_selection_payload)
        self.xdo("key", "--clearmodifiers", "ctrl+v")
        self.pump_until(
            lambda: entry.get_text() == "headsource sourcetail",
            "GTK insertion without a selection",
        )
        self.assertEqual(entry.get_selection_bounds(), ())
        self.assertEqual(
            self.clipboard_text(Gdk.SELECTION_CLIPBOARD), no_selection_payload
        )

        entry.set_text("left DELETE right")
        entry.select_region(5, 11)
        self.pump_until(
            lambda: self.clipboard_text(Gdk.SELECTION_PRIMARY) == "DELETE",
            "GTK empty-paste source selection",
        )
        self.set_selection(Gdk.SELECTION_CLIPBOARD, "")
        self.xdo("key", "--clearmodifiers", "ctrl+v")
        self.pump_until(
            lambda: entry.get_text() == "left  right",
            "GTK empty clipboard replacement",
        )
        self.assertEqual(self.clipboard_text(Gdk.SELECTION_CLIPBOARD), "")
        self.assertIsNone(self.clipboard_text(Gdk.SELECTION_PRIMARY))

    def test_vte_ctrl_shift_v_inserts_clipboard_and_preserves_primary(self) -> None:
        temporary = tempfile.TemporaryDirectory(prefix="punto-vte-e2e-")
        self.addCleanup(temporary.cleanup)
        received = pathlib.Path(temporary.name) / "received"
        reader = (
            "import pathlib, sys\n"
            "destination = pathlib.Path(sys.argv[1])\n"
            "for line in sys.stdin:\n"
            "    with destination.open('a', encoding='utf-8') as stream:\n"
            "        stream.write(line)\n"
            "        stream.flush()\n"
        )

        window = Gtk.Window(title="punto-vte-terminal-e2e")
        terminal = Vte.Terminal()
        window.add(terminal)
        window.set_default_size(640, 320)
        accelerator = Gtk.AccelGroup()
        window.add_accel_group(accelerator)

        def paste_clipboard(*_arguments) -> bool:
            # VTE exposes the operation; terminal windows own this accelerator.
            terminal.paste_clipboard()
            return True

        accelerator.connect(
            Gdk.KEY_v,
            Gdk.ModifierType.CONTROL_MASK | Gdk.ModifierType.SHIFT_MASK,
            Gtk.AccelFlags.VISIBLE,
            paste_clipboard,
        )
        window.show_all()
        self.addCleanup(self.destroy_window, window)
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", DeprecationWarning)
            spawned, child_pid = terminal.spawn_sync(
                Vte.PtyFlags.DEFAULT,
                None,
                [sys.executable, "-c", reader, str(received)],
                ["PATH=/usr/bin:/bin", "LANG=C.UTF-8", "LC_ALL=C.UTF-8"],
                GLib.SpawnFlags.DEFAULT,
                None,
                None,
                None,
            )
        if child_pid > 1:
            self.addCleanup(self.terminate_child, child_pid)
        self.assertTrue(spawned)
        self.assertGreater(child_pid, 1)
        terminal.grab_focus()
        self.focus(window)

        self.set_selection(Gdk.SELECTION_PRIMARY, "primary sentinel")
        payload = "term term"
        self.set_selection(Gdk.SELECTION_CLIPBOARD, payload)
        self.assertFalse(terminal.get_has_selection())
        self.xdo("type", "--clearmodifiers", "--delay", "1", "prefix-")
        self.xdo("key", "--clearmodifiers", "ctrl+shift+v")
        self.pump_until(
            lambda: "prefix-term term" in terminal.get_text_format(Vte.Format.TEXT),
            "VTE clipboard insertion",
        )
        self.xdo("type", "--clearmodifiers", "--delay", "1", "suffix")
        self.xdo("key", "--clearmodifiers", "Return")
        self.pump_until(
            lambda: received.exists()
            and received.read_text(encoding="utf-8") == "prefix-term termsuffix\n",
            "first PTY line",
        )
        self.assertEqual(self.clipboard_text(Gdk.SELECTION_CLIPBOARD), payload)
        self.assertEqual(
            self.clipboard_text(Gdk.SELECTION_PRIMARY), "primary sentinel"
        )
        self.assertFalse(terminal.get_has_selection())

        self.set_selection(Gdk.SELECTION_CLIPBOARD, "")
        self.xdo("type", "--clearmodifiers", "--delay", "1", "empty-guard")
        self.xdo("key", "--clearmodifiers", "ctrl+shift+v")
        self.pump_for(0.05)
        self.xdo("type", "--clearmodifiers", "--delay", "1", "tail")
        self.xdo("key", "--clearmodifiers", "Return")
        expected = "prefix-term termsuffix\nempty-guardtail\n"
        self.pump_until(
            lambda: received.exists()
            and received.read_text(encoding="utf-8") == expected,
            "empty-paste PTY line",
        )
        self.assertEqual(self.clipboard_text(Gdk.SELECTION_CLIPBOARD), "")
        self.assertEqual(
            self.clipboard_text(Gdk.SELECTION_PRIMARY), "primary sentinel"
        )

    def terminate_child(self, child_pid: int) -> None:
        try:
            os.kill(child_pid, signal.SIGTERM)
        except ProcessLookupError:
            return
        deadline = time.monotonic() + 1
        while time.monotonic() < deadline:
            try:
                waited, _ = os.waitpid(child_pid, os.WNOHANG)
            except ChildProcessError:
                return
            if waited == child_pid:
                return
            self.pump_for(0.01)
        try:
            os.kill(child_pid, signal.SIGKILL)
        except ProcessLookupError:
            return
        try:
            os.waitpid(child_pid, 0)
        except ChildProcessError:
            pass


def main() -> int:
    absent = missing_runtime()
    if absent:
        print("SKIP: missing runtime: " + ", ".join(absent), file=sys.stderr)
        return SKIP_EXIT
    program = unittest.main(module=__name__, verbosity=2, exit=False)
    return 0 if program.result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
