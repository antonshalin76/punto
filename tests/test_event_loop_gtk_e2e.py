#!/usr/bin/env python3

import ctypes
import ctypes.util
import os
import pathlib
import queue
import select
import signal
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest


SKIP_EXIT = 77
START_TIMEOUT = 5.0
EVENT_TIMEOUT = 4.0

EV_SYN = 0
EV_KEY = 1
SYN_REPORT = 0
KEY_BACKSPACE = 14
KEY_LEFTCTRL = 29
KEY_LEFTSHIFT = 42
KEY_C = 46
KEY_V = 47
KEY_DOT = 52
KEY_LEFTALT = 56
KEY_SPACE = 57
KEY_RIGHTCTRL = 97
KEY_LEFT = 105
KEY_RIGHT = 106
KEY_PAUSE = 119

LETTER_CODES = {
    "a": 30,
    "b": 48,
    "c": 46,
    "d": 32,
    "e": 18,
    "f": 33,
    "g": 34,
    "h": 35,
    "i": 23,
    "j": 36,
    "k": 37,
    "l": 38,
    "m": 50,
    "n": 49,
    "o": 24,
    "p": 25,
    "q": 16,
    "r": 19,
    "s": 31,
    "t": 20,
    "u": 22,
    "v": 47,
    "w": 17,
    "x": 45,
    "y": 21,
    "z": 44,
}


class InputEvent(ctypes.Structure):
    _fields_ = [
        ("tv_sec", ctypes.c_long),
        ("tv_usec", ctypes.c_long),
        ("type", ctypes.c_ushort),
        ("code", ctypes.c_ushort),
        ("value", ctypes.c_int),
    ]


def missing_runtime() -> list[str]:
    missing = [
        name
        for name in ("bwrap", "Xvfb", "xdotool", "xclip")
        if shutil.which(name) is None
    ]
    if ctypes.util.find_library("X11") is None:
        missing.append("libX11")
    if ctypes.util.find_library("Xtst") is None:
        missing.append("libXtst")
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
        missing.append("Python gi Gtk3")
    return missing


def run_in_sandbox(driver: pathlib.Path) -> int:
    absent = missing_runtime()
    if absent:
        required = os.environ.get("PUNTO_REQUIRE_EVENT_LOOP_E2E") == "1"
        prefix = "FAIL" if required else "SKIP"
        print(prefix + ": missing runtime: " + ", ".join(absent), file=sys.stderr)
        return 1 if required else SKIP_EXIT

    command = [
        shutil.which("bwrap") or "bwrap",
        "--unshare-all",
        "--uid",
        "0",
        "--gid",
        "0",
        "--die-with-parent",
        "--new-session",
        "--clearenv",
        "--ro-bind",
        "/",
        "/",
        "--proc",
        "/proc",
        "--dev",
        "/dev",
        "--tmpfs",
        "/run",
        "--tmpfs",
        "/tmp",
        "--ro-bind",
        str(driver.resolve()),
        "/tmp/punto-event-loop-e2e-driver",
        "--setenv",
        "PATH",
        "/usr/bin:/bin",
        "--setenv",
        "HOME",
        "/tmp/punto-home",
        "--setenv",
        "XDG_CONFIG_HOME",
        "/tmp/punto-home/.config",
        "--setenv",
        "XDG_RUNTIME_DIR",
        "/run/user/0",
        "--setenv",
        "GDK_BACKEND",
        "x11",
        "--setenv",
        "NO_AT_BRIDGE",
        "1",
        "--setenv",
        "LANG",
        "C.UTF-8",
        "--setenv",
        "PUNTO_EVENT_LOOP_E2E_INNER",
        "1",
        "--setenv",
        "PUNTO_RUNTIME_GID",
        "0",
    ]
    for variable in (
        "ASAN_OPTIONS",
        "UBSAN_OPTIONS",
        "LSAN_OPTIONS",
        "PUNTO_EVENT_LOOP_E2E_TEST",
        "PUNTO_E2E_CONFIG_SESSION_RACE",
    ):
        value = os.environ.get(variable)
        if value:
            command.extend(("--setenv", variable, value))
    command.extend(
        (
            sys.executable,
            str(pathlib.Path(__file__).resolve()),
            "/tmp/punto-event-loop-e2e-driver",
        )
    )
    try:
        completed = subprocess.run(command, timeout=80, check=False)
    except subprocess.TimeoutExpired:
        print("FAIL: sandboxed EventLoop GTK e2e timed out", file=sys.stderr)
        return 1
    return completed.returncode


class NestedX11:
    def __init__(self) -> None:
        self.process: subprocess.Popen[bytes] | None = None
        self.display = ""

    def start(self) -> None:
        read_fd, write_fd = os.pipe()
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
                    "-extension",
                    "GLX",
                ],
                pass_fds=(write_fd,),
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
            )
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
            detail = ""
            if self.process.poll() is not None and self.process.stderr is not None:
                detail = self.process.stderr.read().decode("utf-8", errors="replace")
            self.stop()
            raise RuntimeError(
                "nested Xvfb did not publish a display"
                + (f": {detail.strip()}" if detail else "")
            )
        self.display = f":{number}"

    def stop(self) -> None:
        if self.process is None:
            return
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=2)
        if self.process.stderr is not None and not self.process.stderr.closed:
            self.process.stderr.close()
        self.process = None


class BlackholeX11:
    """Accept local X11 sockets without completing the handshake."""

    def __init__(self) -> None:
        self.listener: socket.socket | None = None
        self.connections: list[socket.socket] = []
        self.stop_requested = threading.Event()
        self.thread: threading.Thread | None = None
        self.path: pathlib.Path | None = None
        self.display = ""

    def start(self) -> None:
        socket_dir = pathlib.Path("/tmp/.X11-unix")
        socket_dir.mkdir(parents=True, exist_ok=True)
        for number in range(200, 256):
            path = socket_dir / f"X{number}"
            listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                listener.bind(str(path))
            except OSError:
                listener.close()
                continue
            self.listener = listener
            self.path = path
            self.display = f":{number}"
            break
        if self.listener is None:
            raise RuntimeError("no private X11 socket number available")

        self.listener.listen(8)
        self.listener.settimeout(0.05)
        self.thread = threading.Thread(target=self._accept, daemon=True)
        self.thread.start()

    def _accept(self) -> None:
        assert self.listener is not None
        while not self.stop_requested.is_set():
            try:
                connection, _ = self.listener.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            self.connections.append(connection)

    def stop(self) -> None:
        self.stop_requested.set()
        if self.listener is not None:
            self.listener.close()
        if self.thread is not None:
            self.thread.join(timeout=1)
        for connection in self.connections:
            connection.close()
        if self.path is not None:
            self.path.unlink(missing_ok=True)


class XTestRelay:
    def __init__(self, stream, display: str) -> None:
        self.stream = stream
        self.display = display
        self.events: list[tuple[float, int, int, int]] = []
        self.events_lock = threading.Lock()
        self.output_queue: queue.Queue[tuple[int, int, int] | None] = queue.Queue()
        self.stop_requested = threading.Event()
        self.delay_next_paste = threading.Event()
        self.paste_blocked = threading.Event()
        self.permit_paste = threading.Event()
        self.blocked_at = 0.0
        self.error: BaseException | None = None
        self.stopped = False
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.injector = threading.Thread(target=self._inject, daemon=True)

    def start(self) -> None:
        self.reader.start()
        self.injector.start()

    def arm_delayed_paste(self) -> None:
        self.delay_next_paste.set()
        self.paste_blocked.clear()
        self.permit_paste.clear()
        self.blocked_at = 0.0

    def snapshot(self) -> list[tuple[float, int, int, int]]:
        with self.events_lock:
            return list(self.events)

    def _read_exact(self, size: int) -> bytes | None:
        data = bytearray()
        descriptor = self.stream.fileno()
        while len(data) < size and not self.stop_requested.is_set():
            ready, _, _ = select.select([descriptor], [], [], 0.05)
            if not ready:
                continue
            chunk = os.read(descriptor, size - len(data))
            if not chunk:
                return None if not data else bytes(data)
            data.extend(chunk)
        return bytes(data) if len(data) == size else None

    def _read(self) -> None:
        try:
            size = ctypes.sizeof(InputEvent)
            while not self.stop_requested.is_set():
                raw = self._read_exact(size)
                if raw is None:
                    break
                if len(raw) != size:
                    raise RuntimeError("truncated input_event on EventLoop stdout")
                event = InputEvent.from_buffer_copy(raw)
                observed = (event.type, event.code, event.value)
                with self.events_lock:
                    self.events.append((time.monotonic(), *observed))
                self.output_queue.put(observed)
        except BaseException as error:
            if not self.stop_requested.is_set():
                self.error = error
        finally:
            self.output_queue.put(None)

    def _inject(self) -> None:
        x11 = ctypes.CDLL(ctypes.util.find_library("X11"))
        xtst = ctypes.CDLL(ctypes.util.find_library("Xtst"))
        x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
        x11.XOpenDisplay.restype = ctypes.c_void_p
        x11.XFlush.argtypes = [ctypes.c_void_p]
        x11.XCloseDisplay.argtypes = [ctypes.c_void_p]
        xtst.XTestFakeKeyEvent.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint,
            ctypes.c_int,
            ctypes.c_ulong,
        ]
        xtst.XTestFakeKeyEvent.restype = ctypes.c_int

        connection = x11.XOpenDisplay(self.display.encode("ascii"))
        if not connection:
            self.error = RuntimeError("XTest relay could not open Xvfb")
            return
        ctrl_down = False
        keys_down: set[int] = set()
        try:
            while True:
                event = self.output_queue.get()
                if event is None:
                    return
                event_type, code, value = event
                if event_type != EV_KEY:
                    continue
                if (
                    code == KEY_V
                    and value == 1
                    and ctrl_down
                    and self.delay_next_paste.is_set()
                ):
                    self.delay_next_paste.clear()
                    self.blocked_at = time.monotonic()
                    self.paste_blocked.set()
                    if not self.permit_paste.wait(timeout=EVENT_TIMEOUT):
                        raise RuntimeError("delayed Ctrl+V was never released")

                pressed = value != 0
                if xtst.XTestFakeKeyEvent(connection, code + 8, pressed, 0) == 0:
                    raise RuntimeError(f"XTest rejected evdev code {code}")
                x11.XFlush(connection)
                if pressed:
                    keys_down.add(code)
                else:
                    keys_down.discard(code)
                if code in (KEY_LEFTCTRL, KEY_RIGHTCTRL):
                    ctrl_down = pressed
        except BaseException as error:
            if not self.stop_requested.is_set():
                self.error = error
        finally:
            for code in keys_down:
                xtst.XTestFakeKeyEvent(connection, code + 8, False, 0)
            x11.XFlush(connection)
            x11.XCloseDisplay(connection)

    def stop(self, break_pipe: bool = False) -> None:
        if self.stopped:
            if break_pipe and not self.stream.closed:
                self.stream.close()
            return
        self.stopped = True
        self.stop_requested.set()
        self.permit_paste.set()
        self.reader.join(timeout=1)
        if break_pipe and not self.stream.closed:
            self.stream.close()
        self.injector.join(timeout=1)


class EventLoopHarness:
    def __init__(
        self,
        driver: pathlib.Path,
        display: str,
        extra_environment: dict[str, str] | None = None,
    ) -> None:
        self.stopped = False
        environment = os.environ.copy()
        if extra_environment:
            environment.update(extra_environment)
        self.process = subprocess.Popen(
            [str(driver)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=environment,
        )
        assert self.process.stdout is not None
        assert self.process.stderr is not None
        self.relay = XTestRelay(self.process.stdout, display)
        self.relay.start()
        self.stderr = bytearray()
        self.stderr_thread = threading.Thread(target=self._drain_stderr, daemon=True)
        self.stderr_thread.start()

    def _drain_stderr(self) -> None:
        assert self.process.stderr is not None
        descriptor = self.process.stderr.fileno()
        while True:
            chunk = os.read(descriptor, 4096)
            if not chunk:
                return
            self.stderr.extend(chunk)

    def diagnostic(self) -> str:
        return self.stderr.decode("utf-8", errors="replace")[-4000:]

    def send_events(self, events: list[tuple[int, int, int]]) -> None:
        if self.process.stdin is None:
            raise RuntimeError("EventLoop stdin is closed")
        payload = bytearray()
        for event_type, code, value in events:
            event = InputEvent(0, 0, event_type, code, value)
            payload.extend(bytes(event))
        view = memoryview(payload)
        while view:
            written = os.write(self.process.stdin.fileno(), view)
            if written <= 0:
                raise RuntimeError("EventLoop stdin write made no progress")
            view = view[written:]

    def send_key(self, code: int, shifted: bool = False) -> None:
        events: list[tuple[int, int, int]] = []
        if shifted:
            events.extend(((EV_KEY, KEY_LEFTSHIFT, 1), (EV_SYN, SYN_REPORT, 0)))
        events.extend(
            (
                (EV_KEY, code, 1),
                (EV_SYN, SYN_REPORT, 0),
                (EV_KEY, code, 0),
                (EV_SYN, SYN_REPORT, 0),
            )
        )
        if shifted:
            events.extend(((EV_KEY, KEY_LEFTSHIFT, 0), (EV_SYN, SYN_REPORT, 0)))
        self.send_events(events)

    def type_word(self, text: str) -> None:
        for character in text:
            self.send_key(LETTER_CODES[character.lower()], character.isupper())

    def hotkey(self, modifier: int, repeat: bool) -> None:
        events = [
            (EV_KEY, modifier, 1),
            (EV_SYN, SYN_REPORT, 0),
            (EV_KEY, KEY_PAUSE, 1),
            (EV_SYN, SYN_REPORT, 0),
        ]
        if repeat:
            events.extend(((EV_KEY, KEY_PAUSE, 2), (EV_SYN, SYN_REPORT, 0)))
        events.extend(
            (
                (EV_KEY, KEY_PAUSE, 0),
                (EV_SYN, SYN_REPORT, 0),
                (EV_KEY, modifier, 0),
                (EV_SYN, SYN_REPORT, 0),
            )
        )
        self.send_events(events)

    def wait_ready(self, pump) -> None:
        deadline = time.monotonic() + START_TIMEOUT
        last_error = "IPC socket absent"
        while time.monotonic() < deadline:
            pump()
            if self.process.poll() is not None:
                raise RuntimeError(
                    f"EventLoop exited during startup: {self.process.returncode}\n"
                    + self.diagnostic()
                )
            try:
                response = ipc_request(b"STATS\n", timeout=0.5)
                if b"x11_health=ready" in response:
                    return
                last_error = response.decode("ascii", errors="replace").strip()
            except (FileNotFoundError, ConnectionRefusedError, socket.timeout) as error:
                last_error = str(error)
            time.sleep(0.01)
        raise RuntimeError(
            f"EventLoop X11 readiness timed out: {last_error}\n" + self.diagnostic()
        )

    def break_output(self) -> None:
        self.relay.stop(break_pipe=True)

    def stop(self) -> None:
        if self.stopped:
            return
        self.stopped = True
        self.relay.permit_paste.set()
        if self.process.stdin is not None and not self.process.stdin.closed:
            self.process.stdin.close()
        try:
            self.process.wait(timeout=8)
        except subprocess.TimeoutExpired:
            self.process.terminate()
            try:
                self.process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=2)
        self.relay.stop()
        self.stderr_thread.join(timeout=1)
        if self.process.stdout is not None and not self.process.stdout.closed:
            self.process.stdout.close()
        if self.process.stderr is not None and not self.process.stderr.closed:
            self.process.stderr.close()


def ipc_request(command: bytes, timeout: float = EVENT_TIMEOUT) -> bytes:
    return ipc_request_at("/run/punto.sock", command, timeout)


def ipc_request_at(path: str, command: bytes, timeout: float = EVENT_TIMEOUT) -> bytes:
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(timeout)
        client.connect(path)
        client.sendall(command)
        response = bytearray()
        while b"\n" not in response:
            chunk = client.recv(512)
            if not chunk:
                break
            response.extend(chunk)
        return bytes(response)


Gtk = None
Gdk = None
Vte = None


class EventLoopGtkE2E(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if (
            pathlib.Path("/run/systemd/private").exists()
            or pathlib.Path("/run/dbus/system_bus_socket").exists()
        ):
            raise RuntimeError("test must run without host systemd/system bus")
        if pathlib.Path("/run/service").exists():
            raise RuntimeError("test must run without host service supervision tree")
        if pathlib.Path("/run/punto.sock").exists():
            raise RuntimeError("private /run unexpectedly contains punto.sock")

        pathlib.Path("/run/user/0").mkdir(parents=True, mode=0o700)
        pathlib.Path("/run/user/0").chmod(0o700)
        config_dir = pathlib.Path("/tmp/punto-home/.config/punto")
        config_dir.mkdir(parents=True, mode=0o700)
        pathlib.Path("/tmp/punto-home/.Xauthority").write_bytes(b"")
        pathlib.Path("/tmp/punto-home/.Xauthority").chmod(0o600)
        (config_dir / "config.yaml").write_text(
            "hotkey:\n"
            "  modifier: leftctrl\n"
            "  key: grave\n"
            "auto_switch:\n"
            "  enabled: true\n"
            "  threshold: 3.5\n"
            "  min_word_len: 2\n"
            "  min_score: 5.0\n"
            "  max_rollback_words: 5\n"
            "  typo_correction_enabled: false\n"
            "  max_typo_diff: 2\n"
            "  sticky_shift_correction_enabled: false\n"
            "sound:\n"
            "  enabled: false\n"
            "logging:\n"
            "  level: error\n"
            "runtime:\n"
            "  analysis_threads: 1\n"
            "  max_analysis_threads_per_daemon: 1\n",
            encoding="utf-8",
        )

        cls.x11 = NestedX11()
        cls.x11.start()
        cls.addClassCleanup(cls.x11.stop)
        os.environ["DISPLAY"] = cls.x11.display
        os.environ["XAUTHORITY"] = "/tmp/punto-home/.Xauthority"

        x11 = ctypes.CDLL(ctypes.util.find_library("X11"))
        x11.XInitThreads.restype = ctypes.c_int
        if x11.XInitThreads() == 0:
            raise RuntimeError("XInitThreads failed before GTK initialization")

        global Gtk, Gdk, Vte
        import gi

        gi.require_version("Gtk", "3.0")
        gi.require_version("Gdk", "3.0")
        gi.require_version("Vte", "2.91")
        from gi.repository import Gdk as imported_gdk
        from gi.repository import Gtk as imported_gtk
        from gi.repository import Vte as imported_vte

        Gtk = imported_gtk
        Gdk = imported_gdk
        Vte = imported_vte
        initialized, _ = Gtk.init_check([])
        if not initialized:
            raise RuntimeError("GTK could not open nested Xvfb")

    @classmethod
    def tearDownClass(cls) -> None:
        while Gtk.events_pending():
            Gtk.main_iteration_do(False)

    def setUp(self) -> None:
        # The outer bwrap gives this suite a private /run. Start each case with
        # a fresh control-plane epoch after the prior harness has been stopped.
        for runtime_name in ("punto-control.state", "punto-control.lock"):
            pathlib.Path("/run", runtime_name).unlink(missing_ok=True)
        self.window = Gtk.Window(title="punto-event-loop-e2e")
        self.addCleanup(self._destroy_window)
        self.window.set_wmclass("gedit", "Gedit")
        self.entry = Gtk.Entry()
        self.window.add(self.entry)
        self.key_events: list[tuple[str, str | None, float]] = []
        self.entry.connect("key-press-event", self._record_key, "press")
        self.entry.connect("key-release-event", self._record_key, "release")
        self.window.show_all()
        self.entry.grab_focus()
        self.pump_until(
            lambda: self.window.get_window() is not None,
            "GTK window realization",
        )
        xid = self.window.get_window().get_xid()
        self.xdo("windowfocus", "--sync", str(xid))
        self.publish_active_window(xid)
        self.pump_until(self.entry.has_focus, "GTK entry focus")

        # The production manager subscribes after desktop applications already
        # own selections. Keep both owners live before EventLoop starts so every
        # scenario exercises timestamp-fenced startup baselining.
        self.set_selection(Gdk.SELECTION_CLIPBOARD, "startup clipboard baseline")
        self.set_selection(Gdk.SELECTION_PRIMARY, "startup primary baseline")

        harness_environment: dict[str, str] = {}
        if self._testMethodName in {
            "test_auto_candidate_delimiter_bypasses_unresponsive_x11",
            "test_ru_layout_snapshot_drives_analysis_without_input_x11",
        }:
            blackhole = BlackholeX11()
            blackhole.start()
            self.addCleanup(blackhole.stop)
            harness_environment["PUNTO_E2E_PROBE_DISPLAY"] = blackhole.display
        if self._testMethodName == "test_blocking_config_io_keeps_input_responsive":
            pathlib.Path("/run/punto-e2e-stuck-config-ready").unlink(missing_ok=True)
            harness_environment["PUNTO_E2E_STUCK_CONFIG"] = "1"
        if self._testMethodName == (
            "test_session_config_reload_retries_after_obsolete_load"
        ):
            for marker_name in (
                "punto-e2e-switch-session",
                "punto-e2e-new-session-observed",
                "punto-e2e-old-config-ready",
                "punto-e2e-release-old-config",
                "punto-e2e-new-config-loaded",
            ):
                pathlib.Path("/run", marker_name).unlink(missing_ok=True)
            release = pathlib.Path("/run/punto-e2e-release-old-config")
            self.addCleanup(release.touch, exist_ok=True)
            harness_environment["PUNTO_E2E_CONFIG_SESSION_RACE"] = "1"
        if self._testMethodName == (
            "test_ru_layout_snapshot_drives_analysis_without_input_x11"
        ):
            harness_environment["PUNTO_E2E_OBSERVED_LAYOUT"] = "1"

        self.harness = EventLoopHarness(
            DRIVER, self.x11.display, harness_environment or None
        )
        self.addCleanup(self.harness.stop)
        self.harness.wait_ready(self.pump_once)

    def _destroy_window(self) -> None:
        if self.window is None:
            return
        self.window.destroy()
        self.window = None
        self.pump_for(0.02)

    def _record_key(self, _widget, event, phase: str) -> bool:
        self.key_events.append((phase, Gdk.keyval_name(event.keyval), time.monotonic()))
        return False

    def pump_once(self) -> None:
        while Gtk.events_pending():
            Gtk.main_iteration_do(False)

    def pump_for(self, duration: float) -> None:
        deadline = time.monotonic() + duration
        while time.monotonic() < deadline:
            self.pump_once()
            time.sleep(0.002)

    def pump_until(
        self, predicate, description: str, timeout: float = EVENT_TIMEOUT
    ) -> None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.pump_once()
            if predicate():
                return
            if hasattr(self, "harness") and self.harness.relay.error is not None:
                raise self.harness.relay.error
            time.sleep(0.002)
        diagnostic = ""
        if hasattr(self, "harness"):
            diagnostic = self.harness.diagnostic()
            diagnostic += f"\nprocess={self.harness.process.poll()}"
            diagnostic += f"\nrelay={self.harness.relay.snapshot()[-40:]}"
        self.fail(f"timed out waiting for {description}\n{diagnostic}")

    def xdo(self, *arguments: str) -> str:
        result = subprocess.run(
            [shutil.which("xdotool") or "xdotool", *arguments],
            env={"DISPLAY": self.x11.display, "PATH": "/usr/bin:/bin"},
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=EVENT_TIMEOUT,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr.strip())
        return result.stdout.strip()

    def publish_active_window(self, xid: int) -> None:
        x11 = ctypes.CDLL(ctypes.util.find_library("X11"))
        x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
        x11.XOpenDisplay.restype = ctypes.c_void_p
        x11.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
        x11.XDefaultRootWindow.restype = ctypes.c_ulong
        x11.XInternAtom.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
        x11.XInternAtom.restype = ctypes.c_ulong
        x11.XChangeProperty.argtypes = [
            ctypes.c_void_p,
            ctypes.c_ulong,
            ctypes.c_ulong,
            ctypes.c_ulong,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_void_p,
            ctypes.c_int,
        ]
        x11.XSync.argtypes = [ctypes.c_void_p, ctypes.c_int]
        x11.XCloseDisplay.argtypes = [ctypes.c_void_p]

        connection = x11.XOpenDisplay(self.x11.display.encode("ascii"))
        self.assertTrue(connection)
        root = x11.XDefaultRootWindow(connection)
        prop = x11.XInternAtom(connection, b"_NET_ACTIVE_WINDOW", 0)
        window_type = x11.XInternAtom(connection, b"WINDOW", 0)
        value = (ctypes.c_ulong * 1)(xid)
        x11.XChangeProperty(
            connection,
            root,
            prop,
            window_type,
            32,
            0,
            ctypes.cast(value, ctypes.c_void_p),
            1,
        )
        x11.XSync(connection, 0)
        x11.XCloseDisplay(connection)
        self.assertEqual(self.xdo("getwindowfocus"), str(xid))

    def set_selection(self, atom, text: str) -> None:
        Gtk.Clipboard.get(atom).set_text(text, -1)
        Gdk.flush()
        self.pump_for(0.02)
        selection = "primary" if atom == Gdk.SELECTION_PRIMARY else "clipboard"
        reader = subprocess.Popen(
            ["xclip", "-selection", selection, "-out"],
            env={"DISPLAY": self.x11.display, "PATH": "/usr/bin:/bin"},
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        self.pump_until(
            lambda: reader.poll() is not None,
            f"external {selection} fixture read",
        )
        stdout, stderr = reader.communicate(timeout=1)
        self.assertEqual(reader.returncode, 0, stderr.decode(errors="replace"))
        self.assertEqual(stdout.decode("utf-8"), text)

    def selection_text(self, atom) -> str | None:
        values: list[str | None] = []
        Gtk.Clipboard.get(atom).request_text(
            lambda _clipboard, text, _data: values.append(text), None
        )
        self.pump_until(lambda: bool(values), "selection text")
        return values[0]

    def output_count(self, code: int, value: int = 1) -> int:
        return sum(
            event_type == EV_KEY and event_code == code and event_value == value
            for _, event_type, event_code, event_value in self.harness.relay.snapshot()
        )

    def wait_event_loop_idle(self, description: str) -> None:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
            client.settimeout(EVENT_TIMEOUT)
            client.connect("/run/punto.sock")
            client.sendall(b"GET_STATUS\n")
            self.pump_until(
                lambda: bool(select.select([client], [], [], 0)[0]), description
            )
            self.assertEqual(client.recv(512), b"OK DISABLED\n")

    def stats_fields(self) -> tuple[bytes, dict[str, str]]:
        response = ipc_request(b"STATS\n", timeout=0.5)
        parts = response.decode("ascii", errors="strict").strip().split()
        self.assertGreater(len(parts), 1)
        self.assertEqual(parts[0], "OK")
        return response, dict(part.split("=", 1) for part in parts[1:])

    def test_auto_candidate_delimiter_bypasses_unresponsive_x11(self) -> None:
        clipboard_before = "auto clipboard sentinel"
        primary_before = "auto primary sentinel"
        self.set_selection(Gdk.SELECTION_CLIPBOARD, clipboard_before)
        self.set_selection(Gdk.SELECTION_PRIMARY, primary_before)

        self.assertEqual(ipc_request(b"SET_STATUS 0\n"), b"OK DISABLED\n")
        _, initial_fields = self.stats_fields()
        self.assertEqual(initial_fields["configured_enabled"], "1")

        self.harness.type_word("ghbdtn")
        self.pump_until(
            lambda: self.entry.get_text() == "ghbdtn", "auto candidate source word"
        )
        before = self.harness.relay.snapshot()

        started = time.monotonic()
        self.harness.send_key(KEY_SPACE)
        self.pump_until(
            lambda: self.entry.get_text() == "ghbdtn ",
            "delimiter passthrough with unresponsive X11",
            timeout=0.2,
        )
        self.assertLess(time.monotonic() - started, 0.2)

        observed = [event[1:] for event in self.harness.relay.snapshot()[len(before) :]]
        self.assertEqual(
            observed,
            [
                (EV_KEY, KEY_SPACE, 1),
                (EV_SYN, SYN_REPORT, 0),
                (EV_KEY, KEY_SPACE, 0),
                (EV_SYN, SYN_REPORT, 0),
            ],
        )

        before_punctuation = self.harness.relay.snapshot()
        started = time.monotonic()
        self.harness.send_key(KEY_DOT)
        self.pump_until(
            lambda: self.entry.get_text() == "ghbdtn .",
            "punctuation passthrough with unresponsive X11",
            timeout=0.2,
        )
        self.assertLess(time.monotonic() - started, 0.2)
        self.assertEqual(
            [
                event[1:]
                for event in self.harness.relay.snapshot()[len(before_punctuation) :]
            ],
            [
                (EV_KEY, KEY_DOT, 1),
                (EV_SYN, SYN_REPORT, 0),
                (EV_KEY, KEY_DOT, 0),
                (EV_SYN, SYN_REPORT, 0),
            ],
        )

        latest: tuple[bytes, dict[str, str]] | None = None

        def candidate_was_analyzed() -> bool:
            nonlocal latest
            latest = self.stats_fields()
            return int(latest[1].get("need_switch", "0")) >= 1

        self.pump_until(candidate_was_analyzed, "positive auto-switch candidate")
        assert latest is not None
        response, fields = latest
        status_keys = [part.split(b"=", 1)[0] for part in response.split()[1:]]
        capability_index = status_keys.index(b"text_mutation")
        self.assertEqual(
            status_keys[capability_index - 2 : capability_index + 7],
            [
                b"input_in_flight",
                b"log_dropped",
                b"text_mutation",
                b"enabled",
                b"configured_enabled",
                b"config_pending",
                b"config_generation",
                b"config_result",
                b"analyzed",
            ],
        )
        self.assertEqual(fields["corrections"], "0")
        self.assertEqual(ipc_request(b"GET_STATUS\n"), b"OK DISABLED\n")
        self.assertEqual(
            ipc_request(b"SET_STATUS 1\n"), b"ERROR Text mutation disabled\n"
        )
        self.assertEqual(self.entry.get_text(), "ghbdtn .")
        self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD), clipboard_before)
        self.assertEqual(self.selection_text(Gdk.SELECTION_PRIMARY), primary_before)

    def test_secondary_status_sync_cannot_override_configured_analysis(self) -> None:
        self.assertEqual(ipc_request(b"SET_STATUS 0\n"), b"OK DISABLED\n")
        environment = os.environ.copy()
        secondary = subprocess.Popen(
            [str(DRIVER)],
            stdin=subprocess.PIPE,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            env=environment,
        )
        socket_path = f"/run/punto-{secondary.pid}.sock"

        def stop_secondary() -> None:
            if secondary.stdin is not None and not secondary.stdin.closed:
                secondary.stdin.close()
            if secondary.poll() is None:
                try:
                    secondary.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    secondary.kill()
                    secondary.wait(timeout=2)

        self.addCleanup(stop_secondary)

        latest: dict[str, str] = {}

        def secondary_config_committed() -> bool:
            if secondary.poll() is not None:
                self.fail(f"secondary exited unexpectedly: {secondary.returncode}")
            if not pathlib.Path(socket_path).exists():
                return False
            try:
                response = ipc_request_at(socket_path, b"STATS\n", timeout=0.2)
            except (FileNotFoundError, ConnectionRefusedError, socket.timeout):
                return False
            parts = response.decode("ascii", errors="strict").strip().split()
            latest.clear()
            latest.update(part.split("=", 1) for part in parts[1:])
            return latest.get("config_pending") == "0"

        self.pump_until(
            secondary_config_committed,
            "secondary immutable config commit after shared status sync",
        )
        self.assertEqual(latest["control_plane"], "secondary")
        self.assertEqual(latest["enabled"], "0")
        self.assertEqual(latest["configured_enabled"], "1")
        self.assertEqual(ipc_request(b"GET_STATUS\n"), b"OK DISABLED\n")

    def test_dictionary_oversize_fails_once_without_ipc_readiness(self) -> None:
        sockets_before = {
            str(path) for path in pathlib.Path("/run").glob("punto*.sock")
        }
        environment = os.environ.copy()
        environment["PUNTO_E2E_OVERSIZE_DICTIONARY"] = "1"
        started = time.monotonic()
        process = subprocess.Popen(
            [str(DRIVER)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=environment,
        )

        def stop_oversize_driver() -> None:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=2)
            if process.stdin is not None and not process.stdin.closed:
                process.stdin.close()
            if process.stdout is not None and not process.stdout.closed:
                process.stdout.close()
            if process.stderr is not None and not process.stderr.closed:
                process.stderr.close()

        self.addCleanup(stop_oversize_driver)
        process.wait(timeout=4)
        assert process.stderr is not None
        diagnostic = process.stderr.read().decode("utf-8", errors="replace")
        if process.stdin is not None:
            process.stdin.close()
        if process.stdout is not None:
            process.stdout.close()
        process.stderr.close()
        self.assertEqual(process.returncode, 2)
        self.assertLess(time.monotonic() - started, 4)
        exact = "[punto] FATAL: dictionary initialization failed: oversize"
        self.assertEqual(diagnostic.count(exact), 1, diagnostic)
        self.assertEqual(
            {str(path) for path in pathlib.Path("/run").glob("punto*.sock")},
            sockets_before,
        )

    def test_blocking_dictionary_keeps_passthrough_live_and_shutdown_bounded(
        self,
    ) -> None:
        marker = pathlib.Path("/run/punto-e2e-stuck-dictionary-ready")
        marker.unlink(missing_ok=True)
        sockets_before = {
            str(path) for path in pathlib.Path("/run").glob("punto*.sock")
        }
        environment = os.environ.copy()
        environment["PUNTO_E2E_STUCK_DICTIONARY"] = "1"
        process = subprocess.Popen(
            [str(DRIVER)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            env=environment,
        )

        def stop_stuck_dictionary() -> None:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=2)
            if process.stdin is not None and not process.stdin.closed:
                process.stdin.close()
            if process.stdout is not None and not process.stdout.closed:
                process.stdout.close()
            marker.unlink(missing_ok=True)

        self.addCleanup(stop_stuck_dictionary)
        self.pump_until(marker.exists, "blocking dictionary loader entry")
        socket_path = f"/run/punto-{process.pid}.sock"

        pending: dict[str, str] = {}

        def pending_stats_are_reachable() -> bool:
            if not pathlib.Path(socket_path).exists():
                return False
            try:
                response = ipc_request_at(socket_path, b"STATS\n", timeout=0.2)
            except (FileNotFoundError, ConnectionRefusedError, socket.timeout):
                return False
            pending.clear()
            pending.update(
                part.split("=", 1)
                for part in response.decode("ascii", errors="strict")
                .strip()
                .split()[1:]
            )
            return True

        self.pump_until(
            pending_stats_are_reachable,
            "diagnostic IPC while dictionary loading is pending",
        )
        self.assertEqual(pending["analysis_health"], "degraded")
        self.assertEqual(pending["analysis_outstanding"], "0")
        self.assertEqual(pending["worker_threads"], "0")
        self.assertEqual(
            {str(path) for path in pathlib.Path("/run").glob("punto*.sock")},
            sockets_before | {socket_path},
        )
        assert process.stdin is not None
        assert process.stdout is not None
        events = [
            (EV_KEY, KEY_SPACE, 1),
            (EV_SYN, SYN_REPORT, 0),
            (EV_KEY, KEY_SPACE, 0),
            (EV_SYN, SYN_REPORT, 0),
        ]
        payload = b"".join(bytes(InputEvent(0, 0, *event)) for event in events)
        started = time.monotonic()
        os.write(process.stdin.fileno(), payload)
        observed = bytearray()
        deadline = started + 0.2
        while len(observed) < len(payload) and time.monotonic() < deadline:
            ready, _, _ = select.select([process.stdout], [], [], 0.01)
            if ready:
                observed.extend(
                    os.read(process.stdout.fileno(), len(payload) - len(observed))
                )
        self.assertEqual(bytes(observed), payload)
        self.assertLess(time.monotonic() - started, 0.2)
        self.assertFalse(process.stdin.closed)
        shutdown_started = time.monotonic()
        process.send_signal(signal.SIGTERM)
        self.pump_until(
            lambda: process.poll() is not None,
            "bounded SIGTERM fail-fast shutdown for blocking dictionary I/O",
            timeout=4.5,
        )
        self.assertEqual(process.returncode, 3)
        self.assertLess(time.monotonic() - shutdown_started, 4.0)
        self.assertEqual(
            {str(path) for path in pathlib.Path("/run").glob("punto*.sock")},
            sockets_before,
        )

    def test_proc_scan_candidate_cap_uses_conservative_fallback(self) -> None:
        with tempfile.TemporaryDirectory(
            prefix="punto-proc-contract-", dir="/tmp"
        ) as root:
            proc_root = pathlib.Path(root)
            (proc_root / "self").mkdir()
            (proc_root / "self" / "comm").write_text("punto-daemon\n", encoding="ascii")
            for pid in ("100", "101"):
                (proc_root / pid).mkdir()
                (proc_root / pid / "comm").write_text("other\n", encoding="ascii")
            environment = os.environ.copy()
            environment["PUNTO_E2E_PROC_SCAN_ROOT"] = str(proc_root)
            started = time.monotonic()
            completed = subprocess.run(
                [str(DRIVER)],
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
                env=environment,
                timeout=0.5,
                check=False,
            )
            self.assertEqual(completed.returncode, 0, completed.stderr.decode())
            self.assertLess(time.monotonic() - started, 0.5)

    def test_ru_layout_snapshot_drives_analysis_without_input_x11(self) -> None:
        self.harness.type_word("hello")
        self.pump_until(
            lambda: self.entry.get_text() == "hello", "RU-layout physical word"
        )
        before = self.harness.relay.snapshot()
        started = time.monotonic()
        self.harness.send_key(KEY_SPACE)
        self.pump_until(
            lambda: self.entry.get_text() == "hello ",
            "RU-layout delimiter passthrough",
            timeout=0.2,
        )
        self.assertLess(time.monotonic() - started, 0.2)
        self.assertEqual(
            [event[1:] for event in self.harness.relay.snapshot()[len(before) :]],
            [
                (EV_KEY, KEY_SPACE, 1),
                (EV_SYN, SYN_REPORT, 0),
                (EV_KEY, KEY_SPACE, 0),
                (EV_SYN, SYN_REPORT, 0),
            ],
        )

        latest: dict[str, str] = {}

        def ru_candidate_was_analyzed() -> bool:
            _, fields = self.stats_fields()
            latest.update(fields)
            return int(fields.get("need_switch", "0")) >= 1

        self.pump_until(
            ru_candidate_was_analyzed,
            "RU snapshot interpreted physical hello as wrong-layout text",
        )
        self.assertEqual(latest["corrections"], "0")
        self.assertEqual(self.entry.get_text(), "hello ")

    def test_blocking_config_io_keeps_input_responsive(self) -> None:
        marker = pathlib.Path("/run/punto-e2e-stuck-config-ready")
        self.pump_until(marker.exists, "blocking config loader entry")
        before = self.harness.relay.snapshot()
        started = time.monotonic()
        self.harness.send_key(KEY_SPACE)
        self.pump_until(
            lambda: self.entry.get_text() == " ",
            "input passthrough while config I/O is blocked",
            timeout=0.2,
        )
        self.assertLess(time.monotonic() - started, 0.2)
        self.assertEqual(
            [event[1:] for event in self.harness.relay.snapshot()[len(before) :]],
            [
                (EV_KEY, KEY_SPACE, 1),
                (EV_SYN, SYN_REPORT, 0),
                (EV_KEY, KEY_SPACE, 0),
                (EV_SYN, SYN_REPORT, 0),
            ],
        )

        _, fields = self.stats_fields()
        self.assertEqual(fields["config_pending"], "1")
        self.assertEqual(fields["config_result"], "none")
        self.assertEqual(
            ipc_request(b"RELOAD /etc/punto/config.yaml\n"),
            b"ERROR Config reload in progress\n",
        )

        assert self.harness.process.stdin is not None
        shutdown_started = time.monotonic()
        self.harness.process.stdin.close()
        self.pump_until(
            lambda: self.harness.process.poll() is not None,
            "bounded fail-fast shutdown for blocking config I/O",
            timeout=4.5,
        )
        self.assertEqual(self.harness.process.returncode, 3)
        self.assertLess(time.monotonic() - shutdown_started, 4.0)
        marker.unlink(missing_ok=True)

    def test_session_config_reload_retries_after_obsolete_load(self) -> None:
        old_ready = pathlib.Path("/run/punto-e2e-old-config-ready")
        self.pump_until(old_ready.exists, "old-session config loader entry")

        _, initial = self.stats_fields()
        self.assertEqual(initial["config_pending"], "1")
        self.assertEqual(initial["config_generation"], "1")

        pathlib.Path("/run/punto-e2e-switch-session").touch()
        self.pump_until(
            pathlib.Path("/run/punto-e2e-new-session-observed").exists,
            "new session probe",
            timeout=5.0,
        )
        self.pump_until(
            lambda: "id=punto-event-loop-e2e-new" in self.harness.diagnostic(),
            "new session commit and deferred reload",
        )

        _, blocked = self.stats_fields()
        self.assertEqual(blocked["config_pending"], "1")
        self.assertEqual(blocked["config_generation"], "1")

        pathlib.Path("/run/punto-e2e-release-old-config").touch()
        self.pump_until(
            pathlib.Path("/run/punto-e2e-new-config-loaded").exists,
            "latest-session config load",
        )

        latest: dict[str, str] = {}

        def latest_config_committed() -> bool:
            _, fields = self.stats_fields()
            latest.update(fields)
            return (
                fields["config_pending"] == "0"
                and fields["config_result"] == "ok"
                and fields["config_generation"] == "2"
            )

        self.pump_until(latest_config_committed, "latest-session config commit")
        self.assertEqual(latest["configured_enabled"], "0")
        diagnostic = self.harness.diagnostic()
        self.assertIn("Config reload superseded by newer session", diagnostic)
        self.assertIn(
            'Configuration reloaded: "/tmp/punto-new-config/punto/config.yaml"',
            diagnostic,
        )

    def test_reload_rejects_paths_outside_authorized_roots_synchronously(
        self,
    ) -> None:
        latest: dict[str, str] = {}

        def initial_reload_completed() -> bool:
            _, fields = self.stats_fields()
            latest.update(fields)
            return fields["config_pending"] == "0"

        self.pump_until(initial_reload_completed, "initial config reload completion")
        generation = latest["config_generation"]

        self.assertEqual(
            ipc_request(b"RELOAD ../outside.yaml\n"), b"ERROR Invalid path\n"
        )
        self.assertEqual(
            ipc_request(b"RELOAD /tmp/outside.yaml\n"), b"ERROR Invalid path\n"
        )
        _, fields = self.stats_fields()
        self.assertEqual(fields["config_generation"], generation)
        self.assertEqual(fields["config_pending"], "0")

    def test_selection_gui_hotkey_is_swallowed_before_dispatch(self) -> None:
        original = "before sEleCt after"
        self.entry.set_text(original)
        start = original.index("sEleCt")
        self.entry.select_region(start, start + len("sEleCt"))
        self.pump_until(
            lambda: self.selection_text(Gdk.SELECTION_PRIMARY) == "sEleCt",
            "GTK PRIMARY selection",
        )
        self.set_selection(Gdk.SELECTION_CLIPBOARD, "plain clipboard sentinel")
        before = self.harness.relay.snapshot()

        self.harness.hotkey(KEY_LEFTALT, repeat=True)
        self.wait_event_loop_idle("GUI selection pre-dispatch skip")

        self.assertEqual(self.entry.get_text(), original)
        self.assertEqual(self.entry.get_selection_bounds(), (start, start + 6))
        self.assertEqual(
            self.selection_text(Gdk.SELECTION_CLIPBOARD),
            "plain clipboard sentinel",
        )
        self.assertEqual(self.selection_text(Gdk.SELECTION_PRIMARY), "sEleCt")
        mutating_codes = {
            KEY_C,
            KEY_V,
            KEY_LEFT,
            KEY_RIGHT,
            KEY_BACKSPACE,
            KEY_PAUSE,
        }
        self.assertFalse(
            any(
                event_type == EV_KEY and event_code in mutating_codes
                for _, event_type, event_code, _ in self.harness.relay.snapshot()[
                    len(before) :
                ]
            )
        )

        self.harness.hotkey(KEY_LEFTALT, repeat=False)
        self.wait_event_loop_idle("second GUI selection pre-dispatch skip")
        self.assertEqual(self.entry.get_text(), original)
        self.assertEqual(self.output_count(KEY_PAUSE), 0)
        self.assertEqual(self.output_count(KEY_C), 0)
        self.assertEqual(self.output_count(KEY_V), 0)

    def test_pause_repeat_and_release_only_are_swallowed(self) -> None:
        original = "release-only selection"
        self.entry.set_text(original)
        self.entry.select_region(0, len(original))
        self.pump_until(
            lambda: self.selection_text(Gdk.SELECTION_PRIMARY) == original,
            "PRIMARY before standalone Pause events",
        )
        self.set_selection(Gdk.SELECTION_CLIPBOARD, "pause clipboard sentinel")
        before = self.harness.relay.snapshot()

        self.harness.send_events(
            [
                (EV_KEY, KEY_PAUSE, 2),
                (EV_SYN, SYN_REPORT, 0),
                (EV_KEY, KEY_PAUSE, 0),
                (EV_SYN, SYN_REPORT, 0),
            ]
        )
        self.wait_event_loop_idle("standalone Pause repeat/release")

        after = self.harness.relay.snapshot()[len(before) :]
        self.assertFalse(
            any(
                event_type == EV_KEY and event_code == KEY_PAUSE
                for _, event_type, event_code, _ in after
            )
        )
        self.assertEqual(self.entry.get_text(), original)
        self.assertEqual(self.entry.get_selection_bounds(), (0, len(original)))
        self.assertEqual(
            self.selection_text(Gdk.SELECTION_CLIPBOARD),
            "pause clipboard sentinel",
        )
        self.assertEqual(self.selection_text(Gdk.SELECTION_PRIMARY), original)

    def test_combined_same_client_race_is_closed_before_paste_dispatch(self) -> None:
        original = "before sEleCt after"
        self.entry.set_text(original)
        start = original.index("sEleCt")
        self.entry.select_region(start, start + len("sEleCt"))
        self.pump_until(
            lambda: self.selection_text(Gdk.SELECTION_PRIMARY) == "sEleCt",
            "GTK source selection before combined race",
        )
        self.set_selection(Gdk.SELECTION_CLIPBOARD, "combined-race clipboard")
        before = self.harness.relay.snapshot()
        self.harness.relay.arm_delayed_paste()

        self.harness.hotkey(KEY_LEFTALT, repeat=False)
        early: list[str | None] = []
        Gtk.Clipboard.get(Gdk.SELECTION_CLIPBOARD).request_text(
            lambda _clipboard, text, _data: early.append(text), None
        )
        self.set_selection(Gdk.SELECTION_PRIMARY, "unrelated same-client primary")
        self.pump_until(lambda: bool(early), "same-client background request")
        self.wait_event_loop_idle("combined race pre-dispatch skip")

        self.assertEqual(early[0], "combined-race clipboard")
        self.assertFalse(self.harness.relay.paste_blocked.is_set())
        self.assertEqual(self.entry.get_text(), original)
        self.assertEqual(
            self.selection_text(Gdk.SELECTION_CLIPBOARD),
            "combined-race clipboard",
        )
        self.assertEqual(
            self.selection_text(Gdk.SELECTION_PRIMARY),
            "unrelated same-client primary",
        )
        mutating_codes = {
            KEY_C,
            KEY_V,
            KEY_LEFT,
            KEY_RIGHT,
            KEY_BACKSPACE,
            KEY_PAUSE,
        }
        self.assertFalse(
            any(
                event_type == EV_KEY and event_code in mutating_codes
                for _, event_type, event_code, _ in self.harness.relay.snapshot()[
                    len(before) :
                ]
            )
        )

    def test_word_gui_hotkey_is_swallowed_before_dispatch(self) -> None:
        self.set_selection(Gdk.SELECTION_CLIPBOARD, "word clipboard sentinel")
        self.set_selection(Gdk.SELECTION_PRIMARY, "word primary sentinel")
        self.harness.type_word("hELLo")
        self.pump_until(lambda: self.entry.get_text() == "hELLo", "typed source word")
        before = self.harness.relay.snapshot()

        self.harness.hotkey(KEY_LEFTCTRL, repeat=True)
        self.wait_event_loop_idle("GUI word pre-dispatch skip")

        self.assertEqual(self.entry.get_text(), "hELLo")
        self.assertEqual(
            self.selection_text(Gdk.SELECTION_CLIPBOARD),
            "word clipboard sentinel",
        )
        self.assertEqual(
            self.selection_text(Gdk.SELECTION_PRIMARY), "word primary sentinel"
        )
        mutating_codes = {
            KEY_C,
            KEY_V,
            KEY_LEFT,
            KEY_RIGHT,
            KEY_BACKSPACE,
            KEY_PAUSE,
        }
        self.assertFalse(
            any(
                event_type == EV_KEY and event_code in mutating_codes
                for _, event_type, event_code, _ in self.harness.relay.snapshot()[
                    len(before) :
                ]
            )
        )
        self.assertFalse(any(name == "Pause" for _, name, _ in self.key_events))

    def test_stale_word_gui_hotkey_does_not_select_or_mutate(self) -> None:
        self.set_selection(Gdk.SELECTION_CLIPBOARD, "abort clipboard sentinel")
        self.set_selection(Gdk.SELECTION_PRIMARY, "abort primary sentinel")
        self.harness.type_word("abc")
        self.pump_until(lambda: self.entry.get_text() == "abc", "tracked source word")
        self.entry.set_text("xyz")
        self.entry.set_position(3)
        before = self.harness.relay.snapshot()

        self.harness.hotkey(KEY_LEFTCTRL, repeat=True)
        self.wait_event_loop_idle("stale GUI word pre-dispatch skip")

        self.assertEqual(self.entry.get_text(), "xyz")
        self.assertEqual(self.entry.get_position(), 3)
        self.assertEqual(self.entry.get_selection_bounds(), ())
        self.assertEqual(
            self.selection_text(Gdk.SELECTION_PRIMARY), "abort primary sentinel"
        )
        self.assertEqual(
            self.selection_text(Gdk.SELECTION_CLIPBOARD),
            "abort clipboard sentinel",
        )
        mutating_codes = {
            KEY_C,
            KEY_V,
            KEY_LEFT,
            KEY_RIGHT,
            KEY_BACKSPACE,
            KEY_PAUSE,
        }
        self.assertFalse(
            any(
                event_type == EV_KEY and event_code in mutating_codes
                for _, event_type, event_code, _ in self.harness.relay.snapshot()[
                    len(before) :
                ]
            )
        )

    def assert_vte_selection_transform_is_rejected(
        self, instance: str, klass: str, clipboard_sentinel: str
    ) -> None:
        self.window.destroy()
        self.window = Gtk.Window(title="punto-event-loop-vte-e2e")
        self.window.set_wmclass(instance, klass)
        terminal = Vte.Terminal()
        self.window.add(terminal)
        self.window.show_all()
        terminal.grab_focus()
        terminal.feed(b"AbC")
        self.pump_until(
            lambda: self.window.get_window() is not None,
            "VTE window realization",
        )
        xid = self.window.get_window().get_xid()
        self.xdo("windowfocus", "--sync", str(xid))
        self.publish_active_window(xid)
        terminal.select_all()
        self.pump_for(0.1)
        primary_before = self.selection_text(Gdk.SELECTION_PRIMARY)
        self.assertIsNotNone(primary_before, "VTE select_all must own PRIMARY")
        self.set_selection(Gdk.SELECTION_CLIPBOARD, clipboard_sentinel)

        before = self.harness.relay.snapshot()
        self.harness.hotkey(KEY_LEFTALT, repeat=True)
        completed = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.addCleanup(completed.close)
        completed.settimeout(EVENT_TIMEOUT)
        completed.connect("/run/punto.sock")
        completed.sendall(b"GET_STATUS\n")
        self.pump_until(
            lambda: bool(select.select([completed], [], [], 0)[0]),
            "terminal pre-dispatch rejection",
        )
        self.assertEqual(completed.recv(512), b"OK DISABLED\n")

        self.assertEqual(self.output_count(KEY_C), 0)
        self.assertEqual(self.output_count(KEY_V), 0)
        self.assertEqual(self.output_count(KEY_PAUSE), 0)
        self.assertEqual(self.selection_text(Gdk.SELECTION_PRIMARY), primary_before)
        self.assertEqual(
            self.selection_text(Gdk.SELECTION_CLIPBOARD),
            clipboard_sentinel,
        )
        after = self.harness.relay.snapshot()
        self.assertGreater(len(after), len(before))
        mutating_codes = {KEY_C, KEY_V, KEY_LEFT, KEY_RIGHT, KEY_BACKSPACE, KEY_PAUSE}
        self.assertFalse(
            any(
                event_type == EV_KEY and event_code in mutating_codes
                for _, event_type, event_code, _ in after[len(before) :]
            ),
            "terminal/unknown pre-dispatch skip emitted a mutating key",
        )

    def test_vte_selection_transform_is_rejected_before_dispatch(self) -> None:
        self.assert_vte_selection_transform_is_rejected(
            "xterm", "XTerm", "vte clipboard sentinel"
        )

    def test_custom_vte_unknown_wm_class_is_rejected_before_dispatch(self) -> None:
        self.assert_vte_selection_transform_is_rejected(
            "nebula-shell", "NebulaShell", "custom vte clipboard sentinel"
        )

    def test_output_epipe_before_action_leaves_gtk_state_untouched(self) -> None:
        self.entry.set_text("untouched")
        self.entry.set_position(len("untouched"))
        self.entry.select_region(0, len("untouched"))
        self.set_selection(Gdk.SELECTION_CLIPBOARD, "epipe clipboard sentinel")
        self.pump_until(
            lambda: self.selection_text(Gdk.SELECTION_PRIMARY) == "untouched",
            "native PRIMARY before output failure",
        )

        self.harness.send_events([(EV_KEY, KEY_LEFTALT, 1), (EV_SYN, SYN_REPORT, 0)])
        self.pump_until(
            lambda: any(
                phase == "press" and name == "Alt_L"
                for phase, name, _ in self.key_events
            ),
            "modifier delivery before output failure",
        )
        self.harness.break_output()

        self.harness.send_events(
            [
                (EV_KEY, KEY_PAUSE, 1),
                (EV_SYN, SYN_REPORT, 0),
                (EV_KEY, KEY_PAUSE, 2),
                (EV_SYN, SYN_REPORT, 0),
                (EV_KEY, KEY_PAUSE, 0),
                (EV_SYN, SYN_REPORT, 0),
                (EV_KEY, KEY_LEFTALT, 0),
                (EV_SYN, SYN_REPORT, 0),
            ]
        )
        self.pump_until(
            lambda: self.harness.process.poll() is not None,
            "EventLoop fail-closed exit after EPIPE",
        )
        self.assertNotEqual(self.harness.process.returncode, 0)
        self.assertEqual(self.entry.get_text(), "untouched")
        self.assertEqual(self.entry.get_position(), len("untouched"))
        self.assertEqual(self.entry.get_selection_bounds(), (0, len("untouched")))
        self.assertEqual(
            self.selection_text(Gdk.SELECTION_CLIPBOARD),
            "epipe clipboard sentinel",
        )
        self.assertEqual(self.selection_text(Gdk.SELECTION_PRIMARY), "untouched")
        self.assertEqual(self.output_count(KEY_C), 0)
        self.assertEqual(self.output_count(KEY_V), 0)

    def test_stuck_x11_probe_shutdown_exits_without_static_teardown(self) -> None:
        marker = pathlib.Path("/run/punto-e2e-stuck-probe-ready")
        marker.unlink(missing_ok=True)
        environment = os.environ.copy()
        environment["PUNTO_E2E_STUCK_PROBE"] = "1"
        process = subprocess.Popen(
            [str(DRIVER)],
            stdin=subprocess.PIPE,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            env=environment,
        )

        def stop_stuck_driver() -> None:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=2)
            if process.stdin is not None and not process.stdin.closed:
                process.stdin.close()
            marker.unlink(missing_ok=True)

        self.addCleanup(stop_stuck_driver)
        self.pump_until(marker.exists, "stuck X11 probe entry")
        assert process.stdin is not None
        shutdown_started = time.monotonic()
        process.stdin.close()
        self.pump_until(
            lambda: process.poll() is not None,
            "bounded fail-fast shutdown for stuck X11 probe",
            timeout=4.5,
        )
        self.assertEqual(process.returncode, 3)
        self.assertLess(time.monotonic() - shutdown_started, 4.0)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("usage: test_event_loop_gtk_e2e.py DRIVER", file=sys.stderr)
        raise SystemExit(2)
    DRIVER = pathlib.Path(sys.argv[1])
    if not DRIVER.is_file():
        print(f"missing EventLoop e2e driver: {DRIVER}", file=sys.stderr)
        raise SystemExit(2)
    if os.environ.get("PUNTO_EVENT_LOOP_E2E_INNER") != "1":
        raise SystemExit(run_in_sandbox(DRIVER))
    test_name = os.environ.get("PUNTO_EVENT_LOOP_E2E_TEST")
    arguments = [sys.argv[0]]
    if test_name:
        arguments.append(f"EventLoopGtkE2E.{test_name}")
    unittest.main(argv=arguments, verbosity=2)
