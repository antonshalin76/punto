#!/usr/bin/env python3

import ctypes
import ctypes.util
import array
import fcntl
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
import termios
import threading
import time
import unittest
import warnings


SKIP_EXIT = 77
START_TIMEOUT = 5.0
EVENT_TIMEOUT = 4.0

EV_SYN = 0
EV_KEY = 1
SYN_REPORT = 0
KEY_BACKSPACE = 14
KEY_TAB = 15
KEY_ENTER = 28
KEY_LEFTCTRL = 29
KEY_LEFTSHIFT = 42
KEY_C = 46
KEY_V = 47
KEY_DOT = 52
KEY_LEFTALT = 56
KEY_SPACE = 57
KEY_NUMLOCK = 69
KEY_RIGHTCTRL = 97
KEY_LEFT = 105
KEY_RIGHT = 106
KEY_PAUSE = 119
KEY_Z = 44

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


class XkbState(ctypes.Structure):
    _fields_ = [
        ("group", ctypes.c_ubyte), ("locked_group", ctypes.c_ubyte),
        ("base_group", ctypes.c_ushort), ("latched_group", ctypes.c_ushort),
        ("mods", ctypes.c_ubyte), ("base_mods", ctypes.c_ubyte),
        ("latched_mods", ctypes.c_ubyte), ("locked_mods", ctypes.c_ubyte),
        ("compat_state", ctypes.c_ubyte), ("grab_mods", ctypes.c_ubyte),
        ("compat_grab_mods", ctypes.c_ubyte), ("lookup_mods", ctypes.c_ubyte),
        ("compat_lookup_mods", ctypes.c_ubyte), ("ptr_buttons", ctypes.c_ushort),
    ]


def missing_runtime() -> list[str]:
    missing = [
        name
        for name in ("bwrap", "Xvfb", "xdotool", "xclip", "setxkbmap", "xkbcomp", "xmodmap", "xprop")
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
        "--tmpfs",
        "/etc",
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
    # Build the private mount point even on hosts without Punto installed.
    for entry in sorted(pathlib.Path("/etc").iterdir()):
        if entry.name == "punto":
            continue
        if entry.is_symlink():
            command.extend(("--symlink", os.readlink(entry), str(entry)))
        else:
            command.extend(("--ro-bind", str(entry), str(entry)))
    command.extend(("--dir", "/etc/punto", "--remount-ro", "/etc",
                    "--tmpfs", "/etc/punto"))
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


def require_private_network_namespace() -> None:
    interfaces = {
        line.split(":", maxsplit=1)[0].strip()
        for line in pathlib.Path("/proc/net/dev")
        .read_text(encoding="ascii")
        .splitlines()[2:]
    }
    routes = (
        pathlib.Path("/proc/net/route")
        .read_text(encoding="ascii")
        .splitlines()[1:]
    )
    if interfaces != {"lo"} or routes:
        raise RuntimeError(
            "sandbox must expose only loopback and no IPv4 routes: "
            f"interfaces={sorted(interfaces)!r}, routes={len(routes)}"
        )


class NestedX11:
    def __init__(self) -> None:
        self.process: subprocess.Popen[bytes] | None = None
        self.display = ""
        self.stderr_file = tempfile.TemporaryFile()

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
                stderr=self.stderr_file,
            )
        except OSError:
            os.close(read_fd)
            self.stderr_file.close()
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
            detail = ""
            if self.process.poll() is not None:
                self.stderr_file.seek(0)
                detail = self.stderr_file.read(4096).decode("utf-8", errors="replace")
            self.stop()
            raise RuntimeError(
                "nested Xvfb did not publish a display"
                + (f": {detail.strip()}" if detail else "")
            )
        self.display = f":{number}"

    def stop(self) -> None:
        if self.process is None:
            self.stderr_file.close()
            return
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=2)
        self.stderr_file.close()
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
                # XTest ignores a second press of an already held key. Evdev
                # repeat is a delivered keystroke, represented by X11's pair.
                if value == 2 and code in keys_down:
                    if xtst.XTestFakeKeyEvent(connection, code + 8, False, 0) == 0:
                        raise RuntimeError(f"XTest rejected repeat release {code}")
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
GLib = None


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

        global Gtk, Gdk, Vte, GLib
        import gi

        gi.require_version("Gtk", "3.0")
        gi.require_version("Gdk", "3.0")
        gi.require_version("Vte", "2.91")
        from gi.repository import Gdk as imported_gdk
        from gi.repository import Gtk as imported_gtk
        from gi.repository import Vte as imported_vte
        from gi.repository import GLib as imported_glib

        Gtk = imported_gtk
        Gdk = imported_gdk
        Vte = imported_vte
        GLib = imported_glib
        initialized, _ = Gtk.init_check([])
        if not initialized:
            raise RuntimeError("GTK could not open nested Xvfb")

    @classmethod
    def tearDownClass(cls) -> None:
        while Gtk.events_pending():
            Gtk.main_iteration_do(False)

    def setUp(self) -> None:
        subprocess.run(
            ["setxkbmap", "-display", self.x11.display, "-option", "", "-layout", "us"],
            check=True,
            timeout=3,
        )
        # Replacing the map does not clear the locked group left by a previous
        # correction. Reset server state, not just the map, between scenarios.
        self.lock_keyboard_group(0)
        self.keyboard_locks(0)
        # The outer bwrap gives this suite a private /run. Start each case with
        # a fresh control-plane epoch after the prior harness has been stopped.
        for runtime_name in ("punto-control.state", "punto-control.lock"):
            pathlib.Path("/run", runtime_name).unlink(missing_ok=True)
        for exclusion_name in ("undo_exclusions.txt", ".undo_exclusions.txt.lock"):
            pathlib.Path("/etc/punto", exclusion_name).unlink(missing_ok=True)
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
        if self._testMethodName == "test_initial_user_config_sets_auto_default":
            path = pathlib.Path("/tmp/punto-home/.config/punto/config.yaml")
            original = path.read_text(encoding="utf-8")
            self.addCleanup(path.write_text, original, encoding="utf-8")
            path.write_text(
                original.replace("enabled: true", "enabled: false", 1),
                encoding="utf-8",
            )
        if self._testMethodName == "test_x11_refresh_preserves_runtime_disable":
            pathlib.Path("/run/punto-e2e-layout-ru").unlink(missing_ok=True)
            harness_environment["PUNTO_E2E_DYNAMIC_LAYOUT"] = "1"
        if self._testMethodName in {
            "test_auto_candidate_delimiter_bypasses_unresponsive_x11",
            "test_ru_layout_snapshot_drives_analysis_without_input_x11",
            "test_keyboard_observation_stall_keeps_input_live_and_recovers",
        }:
            blackhole = BlackholeX11()
            blackhole.start()
            self.addCleanup(blackhole.stop)
            self.blackhole = blackhole
            harness_environment["PUNTO_E2E_PROBE_DISPLAY"] = blackhole.display
        if self._testMethodName == "test_blocking_config_io_keeps_input_responsive":
            pathlib.Path("/run/punto-e2e-stuck-config-ready").unlink(missing_ok=True)
            harness_environment["PUNTO_E2E_STUCK_CONFIG"] = "1"
        if self._testMethodName in {
            "test_session_config_reload_retries_after_obsolete_load",
            "test_config_completion_does_not_override_newer_status",
        }:
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
        diagnostic += f"\neditor={self.entry.get_text()!r} keys={self.key_events[-20:]}"
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
            self.assertIn(client.recv(512), (b"OK ENABLED\n", b"OK DISABLED\n"))

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
        self.assertEqual(ipc_request(b"GET_STATUS\n"), b"OK ENABLED\n")
        self.assertEqual(
            ipc_request(b"SET_STATUS 1\n"), b"OK ENABLED\n"
        )
        self.assertEqual(self.entry.get_text(), "ghbdtn .")
        self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD), clipboard_before)
        self.assertEqual(self.selection_text(Gdk.SELECTION_PRIMARY), primary_before)

    def test_secondary_status_sync_cannot_override_configured_analysis(self) -> None:
        self.pump_until(
            lambda: self.stats_fields()[1]["config_pending"] == "0",
            "initial config commit",
        )
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

    def test_config_completion_does_not_override_newer_status(self) -> None:
        ready = pathlib.Path("/run/punto-e2e-old-config-ready")
        release = pathlib.Path("/run/punto-e2e-release-old-config")
        self.pump_until(ready.exists, "old config load paused")
        self.assertEqual(self.stats_fields()[1]["config_pending"], "1")
        self.assertEqual(ipc_request(b"SET_STATUS 0\n"), b"OK DISABLED\n")
        release.touch()
        self.pump_until(
            lambda: self.stats_fields()[1]["config_pending"] == "0",
            "older config completion",
        )
        fields = self.stats_fields()[1]
        self.assertEqual(fields["config_result"], "ok")
        self.assertEqual(fields["configured_enabled"], "1")
        self.assertEqual(fields["enabled"], "0")
        self.assertEqual(ipc_request(b"GET_STATUS\n"), b"OK DISABLED\n")
        self.prepare_word_editor()
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn", "source word")
        self.harness.send_key(KEY_SPACE)
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn ", "auto disabled")
        self.pump_for(0.2)
        self.assertEqual(self.entry.get_text(), "ghbdtn ")
        self.assertNotIn("Word edit dispatch status=", self.harness.diagnostic())

    def test_x11_refresh_preserves_runtime_disable(self) -> None:
        self.prepare_word_editor()
        self.assertEqual(ipc_request(b"SET_STATUS 0\n"), b"OK DISABLED\n")
        config_generation = self.stats_fields()[1]["config_generation"]
        self.pump_until(
            lambda: "Configuration reloaded:" in self.harness.diagnostic(),
            "initial config diagnostic delivered",
        )
        config_loads = self.harness.diagnostic().count("Configuration reloaded:")
        refresh_prefix = "X11 observation refreshed, layout:"
        refreshes = self.harness.diagnostic().count(refresh_prefix)
        self.lock_keyboard_group(1)
        pathlib.Path("/run/punto-e2e-layout-ru").touch()
        self.pump_until(
            lambda: self.harness.diagnostic().count(refresh_prefix) > refreshes
            and self.stats_fields()[1]["x11_health"] == "ready",
            "changed layout observed by session refresh",
        )
        self.pump_until(
            lambda: self.stats_fields()[1]["config_pending"] == "0",
            "session refresh config completion",
        )
        self.assertEqual(ipc_request(b"GET_STATUS\n"), b"OK DISABLED\n")
        self.assertEqual(self.stats_fields()[1]["config_generation"], config_generation)
        self.assertEqual(self.harness.diagnostic().count("Configuration reloaded:"), config_loads)
        self.harness.type_word("hello")
        self.pump_until(lambda: self.entry.get_text() == "руддщ", "Russian source")
        self.harness.send_key(KEY_SPACE)
        self.pump_until(lambda: self.entry.get_text() == "руддщ ", "auto stays off")
        self.pump_for(0.2)
        self.assertEqual(self.entry.get_text(), "руддщ ")
        self.assertNotIn("Word edit dispatch status=", self.harness.diagnostic())

    def test_config_publication_failure_preserves_runtime_and_recovers(self) -> None:
        self.prepare_word_editor()
        config_path = pathlib.Path("/tmp/punto-home/.config/punto/config.yaml")
        previous_config = config_path.read_text(encoding="utf-8")
        self.addCleanup(config_path.write_text, previous_config, encoding="utf-8")
        config_path.write_text(
            previous_config.replace("enabled: true", "enabled: false", 1),
            encoding="utf-8",
        )
        state_path = pathlib.Path("/run/punto-control.state")
        previous_state = state_path.read_bytes()
        self.addCleanup(state_path.chmod, 0o660)
        state_path.chmod(0o666)
        self.assertEqual(ipc_request(b"RELOAD\n"), b"OK Scheduled\n")
        self.pump_until(
            lambda: self.stats_fields()[1]["config_pending"] == "0",
            "failed config publication",
        )
        fields = self.stats_fields()[1]
        self.assertEqual(fields["config_result"], "error")
        self.assertEqual(fields["configured_enabled"], "1")
        self.assertEqual(fields["enabled"], "1")
        self.assertEqual(state_path.read_bytes(), previous_state)
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn", "old config source")
        self.harness.send_key(KEY_SPACE)
        self.pump_until(lambda: self.entry.get_text() == "привет ", "old auto still on")
        state_path.chmod(0o660)
        self.assertEqual(ipc_request(b"RELOAD\n"), b"OK Scheduled\n")
        self.pump_until(
            lambda: self.stats_fields()[1]["config_pending"] == "0",
            "config publication recovered",
        )
        fields = self.stats_fields()[1]
        self.assertEqual(fields["config_result"], "ok")
        self.assertEqual(fields["configured_enabled"], "0")
        self.assertEqual(ipc_request(b"GET_STATUS\n"), b"OK DISABLED\n")
        self.harness.type_word("hello")
        self.pump_until(lambda: self.entry.get_text() == "привет руддщ", "new source")
        self.harness.send_key(KEY_SPACE)
        self.pump_until(lambda: self.entry.get_text() == "привет руддщ ", "auto off")
        self.pump_for(0.2)
        self.assertEqual(self.entry.get_text(), "привет руддщ ")

    def test_initial_user_config_sets_auto_default(self) -> None:
        self.prepare_word_editor()
        fields = self.stats_fields()[1]
        self.assertEqual(fields["configured_enabled"], "0")
        self.assertEqual(fields["enabled"], "0")
        self.assertEqual(ipc_request(b"GET_STATUS\n"), b"OK DISABLED\n")
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn", "source word")
        self.harness.send_key(KEY_SPACE)
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn ", "auto default off")
        self.pump_for(0.2)
        self.assertEqual(self.entry.get_text(), "ghbdtn ")

    def test_xvfb_repeated_keymaps_do_not_block_diagnostic_output(self) -> None:
        for index in range(40):
            subprocess.run(
                ["setxkbmap", "-display", self.x11.display, "-layout",
                 "us,ru" if index % 2 else "us"],
                check=True,
                timeout=1,
            )
        self.lock_keyboard_group(0)
        self.harness.type_word("hello")
        self.pump_until(lambda: self.entry.get_text() == "hello", "Xvfb still responsive")

    def test_nondurable_status_and_config_keep_visible_state(self) -> None:
        self.prepare_word_editor()
        marker = pathlib.Path("/run/punto-e2e-fail-directory-fsync")
        self.addCleanup(marker.unlink, missing_ok=True)
        state_path = pathlib.Path("/run/punto-control.state")

        def state() -> dict[str, str]:
            return dict(
                line.split("=", 1)
                for line in state_path.read_text(encoding="utf-8").splitlines()
            )

        previous = state()
        marker.touch(mode=0o600)
        self.assertEqual(
            ipc_request(b"SET_STATUS 0\n"),
            b"ERROR Status published but durability not confirmed\n",
        )
        self.assertFalse(marker.exists(), "exact directory-fsync fault was consumed")
        visible = state()
        self.assertEqual(visible["enabled"], "0")
        self.assertEqual(
            int(visible["status_generation"]), int(previous["status_generation"]) + 1
        )
        self.assertEqual(visible["config_generation"], previous["config_generation"])
        self.assertEqual(visible["config_path"], previous["config_path"])
        self.assertEqual(ipc_request(b"GET_STATUS\n"), b"OK DISABLED\n")
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn", "source word")
        self.harness.send_key(KEY_SPACE)
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn ", "visible auto off")
        self.pump_for(0.2)
        self.assertEqual(self.entry.get_text(), "ghbdtn ")
        self.assertEqual(ipc_request(b"SET_STATUS 1\n"), b"OK ENABLED\n")

        config_path = pathlib.Path("/tmp/punto-home/.config/punto/nondurable.yaml")
        self.addCleanup(config_path.unlink, missing_ok=True)
        original = config_path.with_name("config.yaml").read_text(encoding="utf-8")
        config_path.write_text(
            original.replace("enabled: true", "enabled: false", 1),
            encoding="utf-8",
        )
        previous = state()
        marker.touch(mode=0o600)
        self.assertEqual(
            ipc_request(f"RELOAD {config_path}\n".encode("ascii")), b"OK Scheduled\n"
        )
        self.pump_until(
            lambda: self.stats_fields()[1]["config_pending"] == "0",
            "nondurable config completion",
        )
        self.assertFalse(marker.exists())
        visible = state()
        self.assertEqual(visible["enabled"], "0")
        self.assertEqual(visible["config_path"], str(config_path))
        self.assertEqual(
            int(visible["status_generation"]), int(previous["status_generation"]) + 1
        )
        self.assertEqual(
            int(visible["config_generation"]), int(previous["config_generation"]) + 1
        )
        fields = self.stats_fields()[1]
        self.assertEqual(fields["config_result"], "error")
        self.assertEqual(fields["configured_enabled"], "0")
        self.assertEqual(ipc_request(b"GET_STATUS\n"), b"OK DISABLED\n")

        self.assertEqual(ipc_request(b"RELOAD\n"), b"OK Scheduled\n")
        self.pump_until(
            lambda: self.stats_fields()[1]["config_pending"] == "0",
            "durable config retry",
        )
        fields = self.stats_fields()[1]
        self.assertEqual(fields["config_result"], "ok")
        self.assertEqual(fields["configured_enabled"], "1")
        self.assertEqual(ipc_request(b"GET_STATUS\n"), b"OK ENABLED\n")
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn ghbdtn", "retry source")
        self.harness.send_key(KEY_SPACE)
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn привет ", "auto recovered")

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
        def diagnostic_analysis_ready() -> bool:
            _, fields = self.stats_fields()
            return (
                fields["config_pending"] == "0"
                and fields["config_result"] == "ok"
                and fields["analysis_health"] == "ready"
                and fields["worker_threads"] == "1"
                and fields["x11_health"] == "ready"
            )

        self.pump_until(diagnostic_analysis_ready, "initialized RU diagnostic analysis")
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
        self.pump_until(
            lambda: 'Configuration reloaded: "/tmp/punto-new-config/punto/config.yaml"'
            in self.harness.diagnostic(),
            "latest-session configuration log delivery",
        )
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

    def test_selection_gui_rejects_unsupported_single_layout(self) -> None:
        layout = subprocess.run(
            ["setxkbmap", "-display", self.x11.display, "-query"],
            check=True, capture_output=True, text=True, timeout=3,
        ).stdout
        self.assertEqual(next(line.split(":", 1)[1].strip() for line in layout.splitlines()
                              if line.startswith("layout:")), "us")
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
        self.pump_until(lambda: "Word edit dispatch status=0" in self.harness.diagnostic(),
                        "unsupported single-layout selection rejection")
        diagnostic = self.harness.stderr.decode("utf-8", errors="replace")
        self.assertIn("Word edit dispatch status=0 rejection_stage=keymap", diagnostic)
        for private_text in (original, "sEleCt", "plain clipboard sentinel"):
            self.assertNotIn(private_text, diagnostic)

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

    def prepare_word_editor(self) -> None:
        self.pump_until(
            lambda: self.stats_fields()[1]["config_pending"] == "0",
            "initial config commit",
        )
        subprocess.run(
            ["setxkbmap", "-display", self.x11.display, "-layout", "us,ru"],
            check=True,
            timeout=3,
        )
        # An active PRIMARY owned by this application is deliberately excluded
        # by the first word-editor stage. Selection support has a separate gate.
        Gtk.Clipboard.get(Gdk.SELECTION_PRIMARY).clear()
        self.pump_until(
            lambda: self.selection_text(Gdk.SELECTION_PRIMARY) is None,
            "empty primary selection",
        )

    def lock_keyboard_group(self, group: int) -> None:
        x11 = ctypes.CDLL(ctypes.util.find_library("X11"))
        x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
        x11.XOpenDisplay.restype = ctypes.c_void_p
        x11.XkbLockGroup.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_uint]
        x11.XkbLockGroup.restype = ctypes.c_int
        x11.XSync.argtypes = [ctypes.c_void_p, ctypes.c_int]
        x11.XCloseDisplay.argtypes = [ctypes.c_void_p]
        connection = x11.XOpenDisplay(self.x11.display.encode("ascii"))
        self.assertTrue(connection)
        try:
            self.assertNotEqual(x11.XkbLockGroup(connection, 0x0100, group), 0)
            x11.XSync(connection, False)
        finally:
            x11.XCloseDisplay(connection)

    def keyboard_locks(self, mask: int | None = None) -> int:
        x11 = ctypes.CDLL(ctypes.util.find_library("X11"))
        x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
        x11.XOpenDisplay.restype = ctypes.c_void_p
        x11.XkbLockModifiers.argtypes = [
            ctypes.c_void_p, ctypes.c_uint, ctypes.c_uint, ctypes.c_uint,
        ]
        x11.XkbLockModifiers.restype = ctypes.c_int
        x11.XkbGetState.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.POINTER(XkbState)]
        x11.XkbGetState.restype = ctypes.c_int
        x11.XCloseDisplay.argtypes = [ctypes.c_void_p]
        connection = x11.XOpenDisplay(self.x11.display.encode("ascii"))
        self.assertTrue(connection)
        try:
            if mask is not None:
                self.assertNotEqual(x11.XkbLockModifiers(connection, 0x0100, 255, mask), 0)
            state = XkbState()
            self.assertEqual(x11.XkbGetState(connection, 0x0100, ctypes.byref(state)), 0)
            if mask is not None:
                self.assertEqual(state.locked_mods, mask)
            return state.locked_mods
        finally:
            x11.XCloseDisplay(connection)

    def install_word_test_map(self, symbols: str = "", types: str = "") -> None:
        # Compile only into this suite's private Xvfb; no host keymap is touched.
        source = (
            'xkb_keymap { xkb_keycodes { include "evdev+aliases(qwerty)" }; '
            'xkb_types { include "complete" ' + types + ' }; '
            'xkb_compatibility { include "complete" }; '
            'xkb_symbols { include "pc+us+ru:2+inet(evdev)" ' + symbols + ' }; };'
        )
        result = subprocess.run(
            ["xkbcomp", "-w", "0", "-", self.x11.display], input=source,
            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            timeout=3, check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.pump_for(0.02)

    def lookup_test_keysym(self, code: int, group: int, modifiers: int) -> str:
        x11 = ctypes.CDLL(ctypes.util.find_library("X11"))
        x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
        x11.XOpenDisplay.restype = ctypes.c_void_p
        x11.XkbLookupKeySym.argtypes = [
            ctypes.c_void_p, ctypes.c_ubyte, ctypes.c_uint,
            ctypes.POINTER(ctypes.c_uint), ctypes.POINTER(ctypes.c_ulong),
        ]
        x11.XkbLookupKeySym.restype = ctypes.c_int
        x11.XKeysymToString.argtypes = [ctypes.c_ulong]
        x11.XKeysymToString.restype = ctypes.c_char_p
        x11.XCloseDisplay.argtypes = [ctypes.c_void_p]
        connection = x11.XOpenDisplay(self.x11.display.encode("ascii"))
        self.assertTrue(connection)
        try:
            consumed, symbol = ctypes.c_uint(), ctypes.c_ulong()
            self.assertNotEqual(x11.XkbLookupKeySym(
                connection, code + 8, modifiers | (group << 13),
                ctypes.byref(consumed), ctypes.byref(symbol),
            ), 0)
            return x11.XKeysymToString(symbol.value).decode("ascii")
        finally:
            x11.XCloseDisplay(connection)

    def assert_numlock_word_correction(self, lock_mask: int) -> None:
        self.harness.send_key(KEY_NUMLOCK)
        self.pump_until(
            lambda: any(name == "Num_Lock" and kind == "release" for kind, name, _ in self.key_events),
            "NumLock key release",
        )
        self.assertEqual(self.keyboard_locks(), lock_mask)
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn", "NumLock source")
        self.harness.send_key(KEY_SPACE)
        self.pump_until(lambda: self.entry.get_text() == "привет ", "NumLock correction")
        self.assertEqual(self.keyboard_locks(), lock_mask)
        self.assertEqual(self.entry.get_selection_bounds(), ())
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "1")
        self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD), "startup clipboard baseline")
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.entry.get_text() == "привет привет", "target layout retained")
        self.assertEqual(self.keyboard_locks(), lock_mask)

    def test_numlock_allows_word_correction(self) -> None:
        self.prepare_word_editor()
        self.assert_numlock_word_correction(16)

    def test_remapped_numlock_allows_word_correction(self) -> None:
        self.prepare_word_editor()
        subprocess.run(
            ["xmodmap", "-display", self.x11.display, "-e", "clear Mod2",
             "-e", "clear Mod3", "-e", "add Mod3 = Num_Lock"],
            check=True, timeout=3,
        )
        self.assert_numlock_word_correction(32)

    def assert_locked_word_rejected(self, source: str, mask: int) -> None:
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.entry.get_text() == source, "locked source word")
        self.harness.send_key(KEY_PAUSE)
        self.pump_until(
            lambda: "Word edit dispatch status=0" in self.harness.diagnostic(),
            "incompatible lock/map rejected before selection",
        )
        self.assertEqual(self.entry.get_text(), source)
        self.assertEqual(self.entry.get_selection_bounds(), ())
        self.assertFalse(any(name == "Left" for _, name, _ in self.key_events))
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "0")
        self.assertEqual(self.keyboard_locks(), mask)
        self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD), "startup clipboard baseline")

    def test_numlock_with_caps_corrects_and_preserves_locks(self) -> None:
        self.prepare_word_editor()
        self.keyboard_locks(18)
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.entry.get_text() == "GHBDTN", "CapsLock source word")
        self.harness.send_key(KEY_PAUSE)
        self.pump_until(lambda: self.entry.get_text() == "ПРИВЕТ", "CapsLock layout correction")
        self.assertEqual(self.keyboard_locks(), 18)
        self.harness.type_word("f")
        self.pump_until(lambda: self.entry.get_text() == "ПРИВЕТА", "CapsLock and corrected layout remain active")

    def test_capslock_case_correction_preserves_locks(self) -> None:
        self.prepare_word_editor()
        self.keyboard_locks(2)
        self.harness.type_word("hello")
        self.pump_until(lambda: self.entry.get_text() == "HELLO", "CapsLock source word")
        self.send_chord((KEY_LEFTCTRL,))
        self.pump_until(lambda: self.entry.get_text() == "hello", "case correction under CapsLock")
        self.assertEqual(self.keyboard_locks(), 2)
        self.harness.type_word("f")
        self.pump_until(lambda: self.entry.get_text() == "helloF", "CapsLock remains active")

    def test_numlock_mixed_alt_row_is_rejected(self) -> None:
        self.prepare_word_editor()
        self.install_word_test_map('modifier_map Mod2 { <LALT> };')
        self.keyboard_locks(16)
        self.assert_locked_word_rejected("ghbdtn", 16)

    def test_numlock_replacement_type_is_rejected_and_recovers(self) -> None:
        self.prepare_word_editor()
        self.install_word_test_map(
            'key <AC05> { type[Group2]="KEYPAD", '
            'symbols[Group2]=[ Cyrillic_pe, Cyrillic_PE ] };',
        )
        self.assertEqual(self.lookup_test_keysym(LETTER_CODES["g"], 0, 16), "g")
        self.assertEqual(self.lookup_test_keysym(LETTER_CODES["g"], 1, 0), "Cyrillic_pe")
        self.assertEqual(self.lookup_test_keysym(LETTER_CODES["g"], 1, 16), "Cyrillic_PE")
        self.keyboard_locks(16)
        self.assert_locked_word_rejected("ghbdtn", 16)
        self.install_word_test_map()
        self.keyboard_locks(0)
        self.harness.send_key(KEY_PAUSE)
        self.pump_until(lambda: self.entry.get_text() == "привет", "compatible map recovery")
        self.assertEqual(self.entry.get_selection_bounds(), ())
        self.assertEqual(self.keyboard_locks(), 0)
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "1")

    def test_numlock_map_query_failure_is_rejected_and_recovers(self) -> None:
        self.prepare_word_editor()
        self.keyboard_locks(16)
        marker = pathlib.Path("/run/punto-e2e-fail-xkb-map")
        marker.touch(mode=0o600)
        self.addCleanup(marker.unlink, missing_ok=True)
        self.assert_locked_word_rejected("ghbdtn", 16)
        self.assertFalse(marker.exists(), "fault must reach the new XKB map query")
        self.harness.send_key(KEY_PAUSE)
        self.pump_until(lambda: self.entry.get_text() == "привет", "map query recovery")
        self.assertEqual(self.entry.get_selection_bounds(), ())
        self.assertEqual(self.keyboard_locks(), 16)
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "1")

    def assert_fresh_layout_correction(self, automatic: bool) -> None:
        self.prepare_word_editor()
        generation = self.stats_fields()[1]["config_generation"]
        self.lock_keyboard_group(1)
        self.harness.type_word("hello")
        self.pump_until(lambda: self.entry.get_text() == "руддщ", "externally selected Russian source")
        self.harness.send_key(KEY_SPACE if automatic else KEY_PAUSE)
        expected = "hello " if automatic else "hello"
        self.pump_until(lambda: self.entry.get_text() == expected, "fresh-layout correction", timeout=1)
        self.assertEqual(self.entry.get_selection_bounds(), ())
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "1")
        self.assertEqual(self.stats_fields()[1]["config_generation"], generation)
        self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD), "startup clipboard baseline")
        self.harness.type_word("f")
        self.pump_until(lambda: self.entry.get_text() == expected + "f", "corrected English group")

    def test_immediate_external_layout_manual_correction(self) -> None:
        self.assert_fresh_layout_correction(automatic=False)

    def test_immediate_external_layout_auto_correction(self) -> None:
        self.assert_fresh_layout_correction(automatic=True)

    def arm_keyboard_observation(self, hold: bool = False) -> tuple[pathlib.Path, pathlib.Path]:
        for name in ("arm-keyboard-observation", "hold-keyboard-observation",
                     "keyboard-observed", "release-keyboard-observation"):
            pathlib.Path("/run", "punto-e2e-" + name).unlink(missing_ok=True)
        pathlib.Path("/run/punto-e2e-arm-keyboard-observation").touch(mode=0o600)
        if hold:
            pathlib.Path("/run/punto-e2e-hold-keyboard-observation").touch(mode=0o600)
        ready = pathlib.Path("/run/punto-e2e-keyboard-observed")
        release = pathlib.Path("/run/punto-e2e-release-keyboard-observation")
        self.addCleanup(release.touch, mode=0o600, exist_ok=True)
        return ready, release

    def assert_no_word_mutation(self, expected: str) -> None:
        self.assertEqual(self.entry.get_text(), expected)
        self.assertEqual(self.entry.get_selection_bounds(), ())
        self.assertFalse(any(name == "Left" for _, name, _ in self.key_events))
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "0")
        self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD), "startup clipboard baseline")

    def test_layout_changes_after_observation_reject_before_selection(self) -> None:
        self.prepare_word_editor()
        self.harness.type_word("hello")
        self.pump_until(lambda: self.entry.get_text() == "hello", "source word")
        ready, _ = self.arm_keyboard_observation()
        self.harness.send_events([(EV_KEY, KEY_PAUSE, 1), (EV_SYN, SYN_REPORT, 0)])
        self.pump_until(ready.exists, "exact keyboard state reply captured")
        self.assertFalse(pathlib.Path("/run/punto-e2e-arm-keyboard-observation").exists())
        self.lock_keyboard_group(1)
        self.harness.send_events([(EV_KEY, KEY_PAUSE, 0), (EV_SYN, SYN_REPORT, 0)])
        self.pump_until(
            lambda: "Word edit dispatch status=0" in self.harness.diagnostic(),
            "changed source group rejected",
        )
        self.assertIn("Word edit dispatch status=0 rejection_stage=context",
                      self.harness.diagnostic())
        self.assert_no_word_mutation("hello")
        self.lock_keyboard_group(0)
        self.harness.send_key(KEY_PAUSE)
        self.pump_until(lambda: self.entry.get_text() == "руддщ", "fresh request recovery")
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "1")
        diagnostic = self.harness.stderr.decode("utf-8", errors="replace")
        self.assertIn("[punto] Word edit dispatch status=2\n", diagnostic)
        self.assertNotIn("status=2 rejection_stage=", diagnostic)
        for private_text in ("hello", "руддщ", "startup clipboard baseline"):
            self.assertNotIn(private_text, diagnostic)

    def test_new_input_invalidates_delayed_keyboard_observation(self) -> None:
        self.prepare_word_editor()
        self.harness.type_word("hello")
        self.pump_until(lambda: self.entry.get_text() == "hello", "source word")
        ready, release = self.arm_keyboard_observation(hold=True)
        self.harness.send_events([(EV_KEY, KEY_PAUSE, 1), (EV_SYN, SYN_REPORT, 0)])
        self.pump_until(ready.exists, "keyboard reply held in test driver")
        self.harness.type_word("f")
        self.pump_until(lambda: self.entry.get_text() == "hellof", "new input during observation", timeout=0.2)
        self.assert_no_word_mutation("hellof")
        release.touch(mode=0o600)
        self.harness.send_events([(EV_KEY, KEY_PAUSE, 0), (EV_SYN, SYN_REPORT, 0)])
        self.pump_until(lambda: not release.exists(), "old reply delivered")
        self.harness.send_key(KEY_PAUSE)
        self.pump_until(lambda: self.entry.get_text() == "руддща", "fresh candidate after cancelled reply")
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "1")

    def assert_control_invalidates_observation(self, command: bytes) -> None:
        self.prepare_word_editor()
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn", "source word")
        ready, release = self.arm_keyboard_observation(hold=True)
        self.harness.send_key(KEY_SPACE)
        self.pump_until(ready.exists, "automatic keyboard reply held")
        generation_before = int(self.stats_fields()[1]["config_generation"])
        expected_reply = b"OK Scheduled\n" if command == b"RELOAD\n" else b"OK DISABLED\n"
        self.assertEqual(ipc_request(command), expected_reply)
        self.pump_until(lambda: self.stats_fields()[1]["config_pending"] == "0", "control commit")
        committed = self.stats_fields()[1]
        self.assertEqual(committed["config_result"], "ok")
        if command == b"RELOAD\n":
            self.assertGreater(int(committed["config_generation"]), generation_before)
        else:
            self.assertEqual(int(committed["config_generation"]), generation_before)
        self.assert_no_word_mutation("ghbdtn ")
        release.touch(mode=0o600)
        self.pump_until(lambda: not release.exists(), "obsolete automatic reply delivered")
        self.pump_until(lambda: int(self.stats_fields()[1]["analyzed"]) == 1, "cancelled candidate retains diagnostic analysis")
        self.assert_no_word_mutation("ghbdtn ")
        expected = "привет "
        if command == b"RELOAD\n":
            # A committed reload deliberately discards the old physical buffer.
            self.harness.type_word("ghbdtn")
            self.pump_until(lambda: self.entry.get_text() == "ghbdtn ghbdtn", "fresh word after reload")
            expected = "ghbdtn привет"
        self.harness.send_key(KEY_PAUSE)
        self.pump_until(lambda: self.entry.get_text() == expected, "new manual request after control command")
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "1")

    def test_status_change_invalidates_delayed_keyboard_observation(self) -> None:
        self.assert_control_invalidates_observation(b"SET_STATUS 0\n")
        self.assertEqual(ipc_request(b"GET_STATUS\n"), b"OK DISABLED\n")

    def test_reload_invalidates_delayed_keyboard_observation(self) -> None:
        self.assert_control_invalidates_observation(b"RELOAD\n")

    def assert_queued_ipc_during_macro(self, command: bytes, changes_context: bool) -> None:
        self.prepare_word_editor()
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn", "native source word")
        paths = {name: pathlib.Path("/run", name) for name in (
            "punto-e2e-hold-macro-ipc", "punto-e2e-macro-ipc-held",
            "punto-e2e-macro-ipc-admitted", "punto-e2e-macro-ipc-expired",
        )}
        for path in paths.values():
            path.unlink(missing_ok=True)
            self.addCleanup(path.unlink, missing_ok=True)
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
            client.settimeout(EVENT_TIMEOUT)
            client.connect("/run/punto.sock")
            paths["punto-e2e-hold-macro-ipc"].touch(mode=0o600)
            self.harness.send_key(KEY_PAUSE)
            self.pump_until(paths["punto-e2e-macro-ipc-held"].exists,
                            "macro held before context check")
            sent_at = time.monotonic()
            client.sendall(command)
            client.setblocking(False)
            response = bytearray()

            def receive_reply():
                try:
                    chunk = client.recv(8192)
                except BlockingIOError:
                    return False
                self.assertTrue(chunk, "IPC connection closed before response")
                response.extend(chunk)
                return b"\n" in response

            self.pump_until(receive_reply, "queued command completed after macro")
            self.assertLess(time.monotonic() - sent_at, 3.0,
                            "queued IPC response remains bounded through macro execution")
        self.assertTrue(paths["punto-e2e-macro-ipc-admitted"].exists(),
                        "IPC request was published into the mailbox while macro was held")
        self.assertFalse(paths["punto-e2e-macro-ipc-expired"].exists(),
                         "macro resumed on admitted request, not hold timeout")
        self.assertFalse(paths["punto-e2e-hold-macro-ipc"].exists())
        if command == b"STATS\n":
            self.assertTrue(response.startswith(b"OK "), bytes(response))
            self.assertIn(b"word_dispatches=1", response)
        else:
            self.assertEqual(bytes(response), {
                b"GET_STATUS\n": b"OK ENABLED\n",
                b"SET_STATUS 0\n": b"OK DISABLED\n",
                b"RELOAD\n": b"OK Scheduled\n",
                b"SHUTDOWN\n": b"ERROR Shutdown not allowed via IPC\n",
            }[command])
        if changes_context:
            self.pump_until(lambda: "Word edit dispatch status=0" in self.harness.diagnostic(),
                            "queued write cancels macro before mutation")
            self.assert_no_word_mutation("ghbdtn")
        else:
            self.pump_until(lambda: self.entry.get_text() == "привет",
                            "read-only IPC preserves exact GTK correction")
            self.assertEqual(self.entry.get_selection_bounds(), ())
            self.assertEqual(self.stats_fields()[1]["word_dispatches"], "1")
        self.assertIsNone(self.harness.process.poll(), "IPC command did not terminate daemon")

    def test_queued_get_status_does_not_cancel_macro(self) -> None:
        self.assert_queued_ipc_during_macro(b"GET_STATUS\n", False)

    def test_queued_stats_does_not_cancel_macro(self) -> None:
        self.assert_queued_ipc_during_macro(b"STATS\n", False)

    def test_queued_status_change_cancels_macro(self) -> None:
        self.assert_queued_ipc_during_macro(b"SET_STATUS 0\n", True)

    def test_queued_reload_cancels_macro(self) -> None:
        self.assert_queued_ipc_during_macro(b"RELOAD\n", True)

    def test_queued_shutdown_cancels_macro_without_shutdown_authority(self) -> None:
        self.assert_queued_ipc_during_macro(b"SHUTDOWN\n", True)

    def test_keyboard_observation_stall_keeps_input_live_and_recovers(self) -> None:
        self.prepare_word_editor()
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn", "source word")
        self.harness.send_key(KEY_PAUSE)
        self.pump_until(lambda: bool(self.blackhole.connections), "observation connection accepted by blackhole")
        self.harness.type_word("f")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtnf", "passthrough behind stalled observation", timeout=0.2)
        self.assertEqual(ipc_request(b"GET_STATUS\n", timeout=0.2), b"OK ENABLED\n")
        self.assert_no_word_mutation("ghbdtnf")

        def observation_connection_closed() -> bool:
            try:
                return self.blackhole.connections[0].recv(4096, socket.MSG_DONTWAIT) == b""
            except BlockingIOError:
                return False

        self.pump_until(observation_connection_closed, "bounded observation transport shutdown", timeout=1)
        path = self.blackhole.path
        assert path is not None
        self.blackhole.stop()
        # The production connector rejects symlinks. Alias the actual private
        # socket inode so recovery retains that socket-type security check.
        path.hardlink_to(pathlib.Path("/tmp/.X11-unix/X" + self.x11.display[1:]))
        self.harness.send_key(KEY_PAUSE)
        self.pump_until(lambda: self.entry.get_text() == "привета", "healthy observation transport recovery")
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "1")
        self.assertEqual(self.entry.get_selection_bounds(), ())

    def test_word_case_hotkey_changes_real_editor_text(self) -> None:
        self.prepare_word_editor()
        self.set_selection(Gdk.SELECTION_CLIPBOARD, "clipboard remains unchanged")
        self.harness.type_word("hELLo")
        self.pump_until(lambda: self.entry.get_text() == "hELLo", "original word")
        self.harness.hotkey(KEY_LEFTCTRL, repeat=True)
        self.pump_until(lambda: self.entry.get_text() == "HellO", "case correction")
        self.pump_for(0.1)
        self.assertEqual(self.entry.get_text(), "HellO")
        self.assertEqual(self.entry.get_position(), 5)
        self.assertEqual(self.entry.get_selection_bounds(), ())
        self.assertEqual(
            self.selection_text(Gdk.SELECTION_CLIPBOARD),
            "clipboard remains unchanged",
        )
        self.assertFalse(any(name == "Pause" for _, name, _ in self.key_events))

    def test_auto_layout_changes_real_editor_text(self) -> None:
        self.prepare_word_editor()
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn", "original word")
        self.harness.send_key(KEY_SPACE)
        self.pump_until(lambda: self.entry.get_text() == "привет ", "auto correction")
        self.assertEqual(self.entry.get_position(), 7)
        self.assertEqual(self.entry.get_selection_bounds(), ())
        self.assertEqual(
            self.selection_text(Gdk.SELECTION_CLIPBOARD),
            "startup clipboard baseline",
        )
        self.harness.type_word("f")
        self.pump_until(
            lambda: self.entry.get_text() == "привет а", "corrected keyboard layout"
        )

    def test_word_hotkey_waits_for_modifier_release(self) -> None:
        self.prepare_word_editor()
        self.harness.type_word("hELLo")
        self.pump_until(lambda: self.entry.get_text() == "hELLo", "original word")
        self.harness.send_events([
            (EV_KEY, KEY_LEFTCTRL, 1), (EV_SYN, SYN_REPORT, 0),
            (EV_KEY, KEY_PAUSE, 1), (EV_SYN, SYN_REPORT, 0),
        ])
        self.pump_for(0.1)
        self.assertEqual(self.entry.get_text(), "hELLo")
        self.harness.send_events([
            (EV_KEY, KEY_PAUSE, 0), (EV_SYN, SYN_REPORT, 0),
        ])
        self.pump_for(0.1)
        self.assertEqual(self.entry.get_text(), "hELLo")
        self.harness.send_events([
            (EV_KEY, KEY_LEFTCTRL, 0), (EV_SYN, SYN_REPORT, 0),
        ])
        self.pump_until(lambda: self.entry.get_text() == "HellO", "released hotkey")

    def test_auto_layout_preserves_trailing_punctuation(self) -> None:
        self.prepare_word_editor()
        self.harness.type_word("ghbdtn")
        self.harness.send_key(KEY_DOT)
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn.", "punctuated word")
        self.harness.send_key(KEY_SPACE)
        self.pump_until(
            lambda: self.entry.get_text() == "привет. ", "punctuation preservation"
        )

    def test_word_editor_rejects_stale_content(self) -> None:
        self.prepare_word_editor()
        self.harness.type_word("hello")
        self.pump_until(lambda: self.entry.get_text() == "hello", "original word")
        self.entry.set_text("other")
        self.entry.set_position(5)
        self.harness.hotkey(KEY_LEFTCTRL, repeat=False)
        self.pump_until(
            lambda: "Word edit dispatch status=" in self.harness.diagnostic(),
            "stale edit decision",
        )
        self.assertEqual(self.entry.get_text(), "other")
        self.assertEqual(self.entry.get_selection_bounds(), ())
        self.harness.type_word("f")
        self.pump_until(lambda: self.entry.get_text() == "otherf", "typing after rejected correction preserves existing text")

    def send_chord(self, modifiers: tuple[int, ...], key: int = KEY_PAUSE) -> None:
        events = []
        for modifier in modifiers:
            events.extend([(EV_KEY, modifier, 1), (EV_SYN, SYN_REPORT, 0)])
        events.extend([(EV_KEY, key, 1), (EV_SYN, SYN_REPORT, 0),
                       (EV_KEY, key, 0), (EV_SYN, SYN_REPORT, 0)])
        for modifier in reversed(modifiers):
            events.extend([(EV_KEY, modifier, 0), (EV_SYN, SYN_REPORT, 0)])
        self.harness.send_events(events)

    def test_rejected_word_edit_preserves_newer_same_client_selection(self) -> None:
        self.prepare_word_editor()
        self.harness.type_word("hello")
        self.pump_until(lambda: self.entry.get_text() == "hello", "tracked source word")
        self.entry.set_text("other")
        self.entry.set_position(5)

        class SelectionRequest(ctypes.Structure):
            _fields_ = [("type", ctypes.c_int), ("serial", ctypes.c_ulong),
                        ("send_event", ctypes.c_int), ("display", ctypes.c_void_p),
                        ("owner", ctypes.c_ulong), ("requestor", ctypes.c_ulong),
                        ("selection", ctypes.c_ulong), ("target", ctypes.c_ulong),
                        ("property", ctypes.c_ulong), ("time", ctypes.c_ulong)]

        gdk = ctypes.CDLL(ctypes.util.find_library("gdk-3"))
        xlib = ctypes.CDLL(ctypes.util.find_library("X11"))
        xlib.XInternAtom.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
        xlib.XInternAtom.restype = ctypes.c_ulong
        callback_type = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p,
                                        ctypes.c_void_p, ctypes.c_void_p)
        requests = []

        @callback_type
        def observe_selection(raw_event, _event, _data):
            request = ctypes.cast(raw_event, ctypes.POINTER(SelectionRequest)).contents
            if request.type == 30 and request.selection == 1:
                utf8 = xlib.XInternAtom(request.display, b"UTF8_STRING", 0)
                if request.target == utf8:
                    requests.append(request.serial)
                    if len(requests) == 2:
                        self.entry.select_region(0, 2)
            return 0

        for name in ("gdk_window_add_filter", "gdk_window_remove_filter"):
            getattr(gdk, name).argtypes = [ctypes.c_void_p, callback_type, ctypes.c_void_p]
        gdk.gdk_window_add_filter(None, observe_selection, None)
        self.addCleanup(gdk.gdk_window_remove_filter, None, observe_selection, None)
        self.send_chord((KEY_LEFTCTRL,))
        self.pump_until(lambda: len(requests) >= 2, "selection changed at second real request")
        self.pump_until(lambda: "Word edit dispatch status=" in self.harness.diagnostic(),
                        "stale correction rejected after selection change")
        self.pump_for(0.02)
        self.assertEqual(self.entry.get_text(), "other")
        self.assertEqual(self.entry.get_selection_bounds(), (0, 2))
        self.assertFalse(any(name == "Right" for _, name, _ in self.key_events))

    def assert_selection_correction(self, source: str, replacement: str,
                                    modifiers: tuple[int, ...]) -> None:
        self.prepare_word_editor()
        original = "before " + source + " after"
        self.entry.set_text(original)
        self.entry.select_region(7, 7 + len(source))
        self.pump_until(lambda: self.selection_text(Gdk.SELECTION_PRIMARY) == source,
                        "selected source text")
        self.send_chord(modifiers)
        self.pump_until(lambda: self.entry.get_text() == "before " + replacement + " after",
                        "selection transformation changes only selected range")
        self.assertEqual(self.entry.get_selection_bounds(), ())
        self.assertEqual(self.output_count(KEY_PAUSE), 0)
        self.pump_until(lambda: self.selection_text(Gdk.SELECTION_CLIPBOARD) == "startup clipboard baseline",
                        "clipboard restored after transfer")

    def test_selection_layout_hotkey_changes_real_editor_text(self) -> None:
        self.assert_selection_correction("ghbdtn", "привет", (KEY_LEFTSHIFT,))
        self.harness.type_word("f")
        self.pump_until(lambda: self.entry.get_text() == "before привета after",
                        "selection layout correction toggles subsequent typing layout")

    def test_selection_case_hotkey_changes_real_editor_text(self) -> None:
        self.assert_selection_correction("sEleCt", "SeLEcT", (KEY_LEFTALT,))

    def test_selection_transliteration_has_modifier_priority(self) -> None:
        self.assert_selection_correction("привет", "privet", (KEY_LEFTCTRL, KEY_LEFTALT))

    def delay_selection_paste(self):
        self.prepare_word_editor()
        self.entry.set_text("before AbC after")
        self.entry.select_region(7, 10)
        self.pump_until(lambda: self.selection_text(Gdk.SELECTION_PRIMARY) == "AbC",
                        "actual selected source before delayed paste")
        intercepted = []

        def delay_paste(editor):
            intercepted.append(True)
            editor.stop_emission_by_name("paste-clipboard")

        handler = self.entry.connect("paste-clipboard", delay_paste)
        self.send_chord((KEY_LEFTALT,))
        self.pump_until(lambda: len(intercepted) == 1, "application receives paste chord")
        self.pump_until(lambda: "Word edit dispatch status=3" in self.harness.diagnostic(),
                        "bounded paste transfer wait expires")
        self.assertEqual(self.entry.get_text(), "before AbC after")
        self.assertEqual(self.entry.get_selection_bounds(), (7, 10))
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "0")
        self.assertEqual(ipc_request(b"GET_STATUS\n", timeout=0.2), b"OK ENABLED\n")
        self.entry.disconnect(handler)

    def test_late_paste_receipt_restores_clipboard_without_false_undo(self) -> None:
        self.delay_selection_paste()
        # Only the application's late request may acknowledge payload delivery.
        self.entry.emit("paste-clipboard")
        self.pump_until(lambda: self.entry.get_text() == "before aBc after", "late real GTK paste receives retained payload")
        self.pump_until(lambda: self.selection_text(Gdk.SELECTION_CLIPBOARD) == "startup clipboard baseline",
                        "late transfer restores original clipboard")
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "0")
        self.send_chord((KEY_LEFTCTRL,), KEY_Z)
        self.pump_until(lambda: self.output_count(KEY_Z, 0) == 1, "partial operation has no Punto undo")
        self.assertEqual(self.output_count(KEY_Z, 1), 1)

    def test_foreign_copy_after_paste_timeout_is_preserved_and_recovers(self) -> None:
        self.delay_selection_paste()
        self.set_selection(Gdk.SELECTION_CLIPBOARD, "new user copy")
        self.wait_event_loop_idle("foreign clipboard ownership observed")
        self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD), "new user copy")
        self.send_chord((KEY_LEFTALT,))
        self.pump_until(lambda: self.entry.get_text() == "before aBc after", "new correction after clipboard owner recovery")
        self.pump_until(lambda: self.selection_text(Gdk.SELECTION_CLIPBOARD) == "new user copy",
                        "new copy preserved after later successful correction")
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "1")

    def test_full_stdout_stops_buffered_macro_drain_after_first_failure(self) -> None:
        self.prepare_word_editor()
        self.entry.set_text("AbC")
        self.entry.select_region(0, 3)
        self.pump_until(lambda: self.selection_text(Gdk.SELECTION_PRIMARY) == "AbC", "selected source")
        intercepted = []

        def delay_paste(editor):
            editor.stop_emission_by_name("paste-clipboard")
            intercepted.append(True)

        self.entry.connect("paste-clipboard", delay_paste)
        self.send_chord((KEY_LEFTALT,))
        self.pump_until(lambda: len(intercepted) == 1, "macro is awaiting application transfer")
        self.harness.relay.stop()
        self.assertFalse(self.harness.relay.reader.is_alive())
        self.assertFalse(self.harness.relay.injector.is_alive())
        output = self.harness.process.stdout
        assert output is not None and not output.closed
        writer = os.open(f"/proc/{self.harness.process.pid}/fd/1", os.O_WRONLY | os.O_NONBLOCK)
        try:
            self.assertEqual(os.fstat(writer).st_ino, os.fstat(output.fileno()).st_ino)
            for size in (4096, 1):
                while True:
                    try:
                        os.write(writer, bytes(size))
                    except BlockingIOError:
                        break
        finally:
            os.close(writer)
        probe = os.open(f"/proc/{self.harness.process.pid}/fd/0", os.O_RDONLY | os.O_NONBLOCK)
        started = time.monotonic()
        try:
            self.harness.send_events([(EV_SYN, SYN_REPORT, 0)] * 3)

            def all_frames_buffered():
                available = array.array("i", [0])
                fcntl.ioctl(probe, termios.FIONREAD, available, True)
                return available[0] == 0

            self.pump_until(all_frames_buffered, "macro consumed all queued input frames", timeout=0.15)
            self.assertNotIn("Word edit dispatch status=", self.harness.diagnostic())
            self.pump_until(lambda: self.harness.process.poll() is not None,
                            "one stdout timeout terminates buffered drain", timeout=3.0)
            self.assertEqual(self.harness.process.returncode, 3)
            self.assertGreater(time.monotonic() - started, 1.8)
        finally:
            os.close(probe)
            output.close()

    def assert_eof_during_macro(self, payload: bytes, exit_code: int) -> None:
        self.prepare_word_editor()
        self.entry.set_text("AbC")
        self.entry.select_region(0, 3)
        self.pump_until(lambda: self.selection_text(Gdk.SELECTION_PRIMARY) == "AbC", "selected source")
        intercepted = []

        def delay_paste(editor):
            editor.stop_emission_by_name("paste-clipboard")
            intercepted.append(True)

        self.entry.connect("paste-clipboard", delay_paste)
        self.send_chord((KEY_LEFTALT,))
        self.pump_until(lambda: len(intercepted) == 1, "macro awaiting application paste")
        stream = self.harness.process.stdin
        assert stream is not None
        os.write(stream.fileno(), payload)
        stream.close()
        self.pump_until(lambda: self.harness.process.poll() is not None,
                        "EOF during macro reaches terminal process state")
        self.assertEqual(self.harness.process.returncode, exit_code)

    def test_partial_input_event_during_macro_is_runtime_failure(self) -> None:
        self.assert_eof_during_macro(b"\0", 3)

    def test_missing_syn_during_macro_is_runtime_failure(self) -> None:
        self.assert_eof_during_macro(bytes(InputEvent(type=EV_KEY, code=LETTER_CODES["f"], value=1)), 3)

    def test_complete_frame_eof_during_macro_is_orderly(self) -> None:
        self.assert_eof_during_macro(bytes(InputEvent(type=EV_SYN, code=SYN_REPORT, value=0)), 0)

    def test_repeated_manual_word_correction_is_reversible(self) -> None:
        self.prepare_word_editor()
        self.harness.type_word("hELLo")
        self.pump_until(lambda: self.entry.get_text() == "hELLo", "physical source word")
        self.send_chord((KEY_LEFTCTRL,))
        self.pump_until(lambda: self.entry.get_text() == "HellO", "first case correction")
        self.send_chord((KEY_LEFTCTRL,))
        self.pump_until(lambda: self.entry.get_text() == "hELLo", "second case correction")
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "2")

    def test_immediate_undo_restores_word_and_then_passes_through(self) -> None:
        self.prepare_word_editor()
        self.harness.type_word("hELLo")
        self.pump_until(lambda: self.entry.get_text() == "hELLo", "physical source word")
        self.send_chord((KEY_LEFTCTRL,))
        self.pump_until(lambda: self.entry.get_text() == "HellO", "case correction before undo")
        self.send_chord((KEY_LEFTCTRL,), KEY_Z)
        self.pump_until(lambda: self.entry.get_text() == "hELLo", "immediate Punto undo")
        self.wait_event_loop_idle("Punto undo completed")
        self.assertEqual(self.output_count(KEY_Z, 1), 0)
        self.assertEqual(self.output_count(KEY_Z, 0), 0)
        self.send_chord((KEY_LEFTCTRL,), KEY_Z)
        self.pump_until(lambda: self.output_count(KEY_Z, 0) == 1, "second undo release passed to application")
        self.assertEqual(self.output_count(KEY_Z, 1), 1)
        self.assertEqual(self.output_count(KEY_Z, 2), 0)

    def test_manual_case_after_backspace_uses_visible_word(self) -> None:
        self.prepare_word_editor()
        self.harness.type_word("hELLo")
        self.pump_until(lambda: self.entry.get_text() == "hELLo", "physical source word")
        self.send_chord((KEY_LEFTCTRL,))
        self.pump_until(lambda: self.entry.get_text() == "HellO", "first case correction")
        self.harness.send_key(KEY_BACKSPACE)
        self.pump_until(lambda: self.entry.get_text() == "Hell", "backspace edits visible correction")
        self.send_chord((KEY_LEFTCTRL,))
        self.pump_until(lambda: self.entry.get_text() == "hELL", "case correction uses remaining visible word")
        self.assertEqual(self.entry.get_selection_bounds(), ())
        self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD), "startup clipboard baseline")

    def test_ctrl_shift_z_passes_through_with_fresh_punto_undo(self) -> None:
        self.prepare_word_editor()
        self.harness.type_word("hELLo")
        self.pump_until(lambda: self.entry.get_text() == "hELLo", "physical source word")
        self.send_chord((KEY_LEFTCTRL,))
        self.pump_until(lambda: self.entry.get_text() == "HellO", "fresh correction before native redo")
        self.send_chord((KEY_LEFTCTRL, KEY_LEFTSHIFT), KEY_Z)
        self.pump_until(lambda: self.output_count(KEY_Z, 0) == 1, "native redo release")
        self.assertEqual(self.output_count(KEY_Z, 1), 1)
        self.wait_event_loop_idle("native redo completed")
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "1")
        self.assertEqual(self.entry.get_text(), "HellO")

    def assert_native_undo_after_invalidation(self, expire: bool) -> None:
        self.prepare_word_editor()
        self.harness.type_word("hELLo")
        self.pump_until(lambda: self.entry.get_text() == "hELLo", "physical source word")
        self.send_chord((KEY_LEFTCTRL,))
        self.pump_until(lambda: self.entry.get_text() == "HellO", "successful correction")
        if expire:
            self.pump_for(2.6)
            expected = "HellO"
        else:
            self.harness.type_word("f")
            self.pump_until(lambda: self.entry.get_text() == "HellOf", "new physical input invalidates undo")
            expected = "HellOf"
        self.send_chord((KEY_LEFTCTRL,), KEY_Z)
        self.pump_until(lambda: self.output_count(KEY_Z, 0) == 1, "native undo release after invalidation")
        self.assertEqual(self.output_count(KEY_Z, 1), 1)
        self.wait_event_loop_idle("invalidated undo processed")
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "1")
        self.assertEqual(self.entry.get_text(), expected)

    def test_expired_undo_passes_to_application(self) -> None:
        self.assert_native_undo_after_invalidation(expire=True)

    def test_new_input_invalidates_punto_undo(self) -> None:
        self.assert_native_undo_after_invalidation(expire=False)

    def test_manual_layout_preserves_repeated_space(self) -> None:
        self.prepare_word_editor()
        self.assertEqual(ipc_request(b"SET_STATUS 0\n"), b"OK DISABLED\n")
        self.harness.type_word("ghbdtn")
        self.harness.send_events([
            (EV_KEY, KEY_SPACE, 1), (EV_SYN, SYN_REPORT, 0),
            (EV_KEY, KEY_SPACE, 2), (EV_SYN, SYN_REPORT, 0),
            (EV_KEY, KEY_SPACE, 0), (EV_SYN, SYN_REPORT, 0),
        ])
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn  ", "physical repeated spaces")
        self.assertEqual(self.output_count(KEY_SPACE, 1), 1)
        self.assertEqual(self.output_count(KEY_SPACE, 2), 1)
        self.assertEqual(self.output_count(KEY_SPACE, 0), 1)
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "0")
        self.harness.send_key(KEY_PAUSE)
        self.pump_until(lambda: self.entry.get_text() == "привет  ", "manual correction preserves repeated separators")
        self.assertEqual(self.entry.get_selection_bounds(), ())
        self.assertEqual(self.output_count(KEY_PAUSE), 0)

    def test_manual_word_layout_preserves_literal_textview_tab(self) -> None:
        self.prepare_word_editor()
        self.assertEqual(ipc_request(b"SET_STATUS 0\n"), b"OK DISABLED\n")
        self.window.remove(self.entry)
        editor = Gtk.TextView()
        self.window.add(editor)
        self.window.show_all()
        editor.grab_focus()
        self.pump_until(editor.has_focus, "text view focus")
        document = editor.get_buffer()

        def visible_text():
            start, end = document.get_bounds()
            return document.get_text(start, end, True)

        self.harness.type_word("ghbdtn")
        self.harness.send_key(KEY_TAB)
        self.pump_until(lambda: visible_text() == "ghbdtn\t", "physical literal tab in text view")
        self.harness.send_key(KEY_PAUSE)
        self.pump_until(lambda: visible_text() == "привет\t", "word correction preserves literal tab")
        self.assertFalse(document.get_has_selection())
        self.pump_until(lambda: self.selection_text(Gdk.SELECTION_CLIPBOARD) == "startup clipboard baseline",
                        "tab replacement restores clipboard after transfer")

    def test_auto_word_layout_preserves_literal_textview_tab(self) -> None:
        self.prepare_word_editor()
        self.window.remove(self.entry)
        editor = Gtk.TextView()
        self.window.add(editor)
        self.window.show_all()
        editor.grab_focus()
        self.pump_until(editor.has_focus, "text view focus")
        document = editor.get_buffer()

        def visible_text():
            return document.get_text(*document.get_bounds(), True)

        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: visible_text() == "ghbdtn", "physical source before automatic Tab")
        self.harness.send_key(KEY_TAB)
        self.pump_until(lambda: visible_text() == "привет\t", "automatic correction preserves literal tab")
        self.assertFalse(document.get_has_selection())
        self.assertEqual(self.output_count(KEY_TAB, 1), 1)
        self.assertEqual(self.output_count(KEY_TAB, 0), 1)
        self.assertEqual(self.output_count(KEY_PAUSE), 0)
        self.pump_until(lambda: self.selection_text(Gdk.SELECTION_CLIPBOARD) == "startup clipboard baseline",
                        "automatic tab replacement restores clipboard")
        self.harness.type_word("f")
        self.pump_until(lambda: visible_text() == "привет\tа", "automatic Tab retains target layout")

    def send_text_batch(self, text: str) -> None:
        events = []
        for character in text:
            code = KEY_SPACE if character == " " else LETTER_CODES[character]
            events.extend([(EV_KEY, code, 1), (EV_SYN, SYN_REPORT, 0),
                           (EV_KEY, code, 0), (EV_SYN, SYN_REPORT, 0)])
        self.harness.send_events(events)

    def test_auto_correction_preserves_younger_unfinished_word(self) -> None:
        self.prepare_word_editor()
        self.send_text_batch("ghbdtn hel")
        self.pump_until(lambda: self.entry.get_text() == "привет hel", "older correction preserves unfinished English tail")
        self.assertEqual(self.entry.get_selection_bounds(), ())
        self.harness.type_word("f")
        self.pump_until(lambda: self.entry.get_text() == "привет helа", "new input follows corrected layout")

    def test_auto_history_preserves_words_observed_in_different_layouts(self) -> None:
        self.assert_auto_history_preserves_words_observed_in_different_layouts()

    def test_auto_history_waits_for_slow_gtk_replacement(self) -> None:
        self.assert_auto_history_preserves_words_observed_in_different_layouts(slow_replacement=True)

    def assert_auto_history_preserves_words_observed_in_different_layouts(self, slow_replacement=False) -> None:
        self.prepare_word_editor()
        stalled_selections = []
        if slow_replacement:
            def stall_first_replacement(entry, event):
                bounds = entry.get_selection_bounds()
                if event.keyval == Gdk.KEY_Cyrillic_pe and bounds and not stalled_selections:
                    stalled_selections.append(entry.get_chars(*bounds))
                    time.sleep(0.08)
                return False

            handler = self.entry.connect("key-press-event", stall_first_replacement)
            self.addCleanup(self.entry.disconnect, handler)
        ready, release = self.arm_keyboard_observation(hold=True)
        self.send_text_batch("ghbdtn ")
        self.pump_until(ready.exists, "first word observed in English layout")
        self.lock_keyboard_group(1)
        self.send_text_batch("world ")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn цщкдв ", "second physical word in Russian layout")
        release.touch(mode=0o600)
        if slow_replacement:
            self.pump_until(lambda: bool(stalled_selections), "GTK receives first replacement with suffix selected")
            self.assertEqual(stalled_selections, ["ghbdtn цщкдв "])
        self.pump_until(lambda: self.entry.get_text() == "привет world ", "ordered corrections preserve each word source layout")
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "2")
        self.assertEqual(self.entry.get_selection_bounds(), ())
        self.harness.type_word("f")
        self.pump_until(lambda: self.entry.get_text() == "привет world f", "last correction sets English layout")

    def test_variable_length_typo_preserves_younger_word_and_undo(self) -> None:
        self.prepare_word_editor()
        config = pathlib.Path("/tmp/punto-home/.config/punto/config.yaml")
        original = config.read_text(encoding="utf-8")
        self.addCleanup(config.write_text, original, encoding="utf-8")
        config.write_text(original.replace("typo_correction_enabled: false",
                                           "typo_correction_enabled: true"), encoding="utf-8")
        before = int(self.stats_fields()[1]["config_generation"])
        self.assertEqual(ipc_request(b"RELOAD\n"), b"OK Scheduled\n")
        self.pump_until(lambda: int(self.stats_fields()[1]["config_generation"]) > before
                        and self.stats_fields()[1]["config_pending"] == "0", "typo config committed")
        self.send_text_batch("helllo world ")
        self.pump_until(lambda: self.entry.get_text() == "hello world ", "variable length typo correction preserves younger word")
        self.assertEqual(self.entry.get_selection_bounds(), ())
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "1")
        self.send_chord((KEY_LEFTCTRL,), KEY_Z)
        self.pump_until(lambda: self.entry.get_text() == "helllo world ", "undo restores original lengths and tail")

    def test_automatic_undo_learns_exclusion_and_survives_restart(self) -> None:
        self.assert_automatic_undo_learning("ghbdtn", "привет")

    def test_automatic_undo_learns_punctuation_key_and_survives_restart(self) -> None:
        from unittest.mock import patch

        with patch.dict(LETTER_CODES, {";": 39}):
            self.assert_automatic_undo_learning(";tcn", "жест")

    def assert_automatic_undo_learning(self, source: str, corrected: str) -> None:
        self.prepare_word_editor()
        self.send_text_batch(source + " ")
        self.pump_until(lambda: self.entry.get_text() == corrected + " ", "automatic correction before learning")
        self.send_chord((KEY_LEFTCTRL,), KEY_Z)
        self.pump_until(lambda: self.entry.get_text() == source + " ", "undo restores automatic correction")
        self.send_text_batch(source + " ")
        self.pump_until(lambda: self.stats_fields()[1]["analyzed"] == "2", "repeated word analyzed")
        self.pump_for(0.1)
        self.assertEqual(self.entry.get_text(), source + " " + source + " ")
        exclusions = pathlib.Path("/etc/punto/undo_exclusions.txt")
        self.pump_until(lambda: exclusions.exists() and source + "\n" in exclusions.read_text(),
                        "learned exclusion persisted")
        self.harness.send_key(KEY_PAUSE)
        self.pump_until(lambda: self.entry.get_text() == source + " " + corrected + " ", "manual correction remains available for learned exclusion")
        self.harness.stop()
        self.entry.set_text("")
        self.lock_keyboard_group(0)
        Gtk.Clipboard.get(Gdk.SELECTION_PRIMARY).clear()
        self.harness = EventLoopHarness(DRIVER, self.x11.display)
        self.addCleanup(self.harness.stop)
        self.harness.wait_ready(self.pump_once)
        self.prepare_word_editor()
        self.send_text_batch(source + " ")
        self.pump_until(lambda: self.stats_fields()[1]["analyzed"] == "1", "word analyzed after restart")
        self.pump_for(0.1)
        self.assertEqual(self.entry.get_text(), source + " ")
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "0")

    def assert_backspace_learning(self, intervening_key: bool) -> None:
        self.prepare_word_editor()
        self.send_text_batch("ghbdtn ")
        self.pump_until(lambda: self.entry.get_text() == "привет ", "automatic correction before backspaces")
        self.pump_until(lambda: self.stats_fields()[1]["word_dispatches"] == "1", "automatic dispatch recorded")
        if intervening_key:
            self.harness.type_word("f")
            self.pump_until(lambda: self.entry.get_text() == "привет а", "intervening ordinary input")
        for _ in range(3):
            self.harness.send_key(KEY_BACKSPACE)
        remaining = "приве" if intervening_key else "прив"
        self.pump_until(lambda: self.entry.get_text() == remaining, "three real backspaces delivered")
        self.assertEqual(self.output_count(KEY_BACKSPACE, 1), 3)
        self.assertEqual(self.output_count(KEY_BACKSPACE, 0), 3)
        exclusions = pathlib.Path("/etc/punto/undo_exclusions.txt")
        if not intervening_key:
            self.pump_until(lambda: exclusions.exists() and "ghbdtn\n" in exclusions.read_text(),
                            "three-backspace learning durably stored")
        self.harness.send_key(KEY_LEFT)
        self.pump_until(lambda: self.output_count(KEY_LEFT, 0) == 1, "navigation resets old word tracking")
        self.entry.set_text("")
        self.entry.set_position(0)
        self.lock_keyboard_group(0)
        Gtk.Clipboard.get(Gdk.SELECTION_PRIMARY).clear()
        self.send_text_batch("ghbdtn ")
        if intervening_key:
            self.pump_until(lambda: self.entry.get_text() == "привет ", "intervening input prevents exclusion learning")
            self.assertEqual(self.stats_fields()[1]["word_dispatches"], "2")
            self.assertFalse(exclusions.exists() and "ghbdtn\n" in exclusions.read_text())
        else:
            self.pump_until(lambda: self.stats_fields()[1]["analyzed"] == "2", "learned word analyzed again")
            self.wait_event_loop_idle("exclusion decision completed")
            self.assertEqual(self.entry.get_text(), "ghbdtn ")
            self.assertEqual(self.stats_fields()[1]["word_dispatches"], "1")

    def test_three_backspaces_learn_automatic_correction(self) -> None:
        self.assert_backspace_learning(intervening_key=False)

    def test_ordinary_key_before_backspaces_does_not_learn(self) -> None:
        self.assert_backspace_learning(intervening_key=True)

    def assert_failed_undo_does_not_learn(self, failure: str) -> None:
        self.prepare_word_editor()
        self.send_text_batch("ghbdtn ")
        self.pump_until(lambda: self.entry.get_text() == "привет ", "automatic correction before reserved undo")
        self.pump_until(lambda: self.stats_fields()[1]["word_dispatches"] == "1", "automatic correction recorded")
        self.entry.set_text("other")
        self.entry.set_position(-1)
        if failure == "rejected":
            self.lock_keyboard_group(0)
        if failure == "cancelled":
            ready, release = self.arm_keyboard_observation(hold=True)
        self.send_chord((KEY_LEFTCTRL,), KEY_Z)
        if failure == "cancelled":
            self.pump_until(ready.exists, "reserved undo keyboard reply held")
        else:
            status = 0 if failure == "rejected" else 1
            self.pump_until(lambda: f"Word edit dispatch status={status}" in self.harness.diagnostic(),
                            "reserved undo rejected without successful restoration")
        self.assertEqual(self.entry.get_text(), "other")
        self.assertEqual(self.output_count(KEY_Z, 1), 0, "original Ctrl+Z was reserved, not passed through")
        for _ in range(3):
            self.harness.send_key(KEY_BACKSPACE)
        self.pump_until(lambda: self.entry.get_text() == "ot", "backspaces edit unrelated text", timeout=0.5)
        if failure == "cancelled":
            release.touch(mode=0o600)
            self.pump_until(lambda: not release.exists(), "cancelled undo reply delivered")
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "1")
        self.harness.send_key(KEY_LEFT)
        self.pump_until(lambda: self.output_count(KEY_LEFT, 0) == 1, "old word tracking reset")
        self.entry.set_text("")
        self.entry.set_position(0)
        self.lock_keyboard_group(0)
        Gtk.Clipboard.get(Gdk.SELECTION_PRIMARY).clear()
        self.send_text_batch("ghbdtn ")
        self.pump_until(lambda: self.entry.get_text() == "привет ", "failed undo must not teach an exclusion")
        exclusions = pathlib.Path("/etc/punto/undo_exclusions.txt")
        self.assertFalse(exclusions.exists() and "ghbdtn\n" in exclusions.read_text())
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "2")

    def test_prepared_undo_failure_does_not_learn_from_backspaces(self) -> None:
        self.assert_failed_undo_does_not_learn("prepared")

    def test_rejected_undo_does_not_learn_from_backspaces(self) -> None:
        self.assert_failed_undo_does_not_learn("rejected")

    def test_cancelled_undo_observation_does_not_learn_from_backspaces(self) -> None:
        self.assert_failed_undo_does_not_learn("cancelled")

    def test_combined_fix_preserves_trailing_punctuation(self) -> None:
        self.prepare_word_editor()
        config = pathlib.Path("/tmp/punto-home/.config/punto/config.yaml")
        original = config.read_text(encoding="utf-8")
        self.addCleanup(config.write_text, original, encoding="utf-8")
        config.write_text(
            original.replace("sticky_shift_correction_enabled: false",
                             "sticky_shift_correction_enabled: true"),
            encoding="utf-8",
        )
        _, before = self.stats_fields()
        self.assertEqual(ipc_request(b"RELOAD\n"), b"OK Scheduled\n")

        def committed() -> bool:
            fields = self.stats_fields()[1]
            return (
                int(fields["config_generation"]) > int(before["config_generation"])
                and fields["config_pending"] == "0"
                and fields["config_result"] == "ok"
            )

        self.pump_until(
            committed,
            "case correction config commit",
        )
        self.harness.type_word("GHbdtn")
        self.harness.send_key(KEY_DOT)
        self.pump_until(lambda: self.entry.get_text() == "GHbdtn.", "original word")
        self.harness.send_key(KEY_SPACE)
        self.pump_until(
            lambda: self.entry.get_text() == "Привет. ", "combined correction"
        )

    def test_later_input_cancels_pending_word_hotkey(self) -> None:
        self.prepare_word_editor()
        self.harness.type_word("hello")
        self.pump_until(lambda: self.entry.get_text() == "hello", "original word")
        self.harness.send_events([
            (EV_KEY, KEY_PAUSE, 1), (EV_SYN, SYN_REPORT, 0),
            (EV_KEY, KEY_PAUSE, 0), (EV_SYN, SYN_REPORT, 0),
            (EV_KEY, LETTER_CODES["a"], 1), (EV_SYN, SYN_REPORT, 0),
            (EV_KEY, LETTER_CODES["a"], 0), (EV_SYN, SYN_REPORT, 0),
        ])
        self.pump_until(lambda: self.entry.get_text() == "helloa", "newer input")
        self.pump_for(0.1)
        self.assertEqual(self.entry.get_text(), "helloa")

    def test_word_hotkey_waits_for_ordinary_key_release(self) -> None:
        self.prepare_word_editor()
        self.harness.type_word("hell")
        self.harness.send_events([
            (EV_KEY, LETTER_CODES["o"], 1), (EV_SYN, SYN_REPORT, 0),
        ])
        self.pump_until(lambda: self.entry.get_text() == "hello", "held final key")
        self.harness.hotkey(KEY_LEFTCTRL, repeat=False)
        self.pump_for(0.1)
        self.assertEqual(self.entry.get_text(), "hello")
        self.harness.send_events([
            (EV_KEY, LETTER_CODES["o"], 0), (EV_SYN, SYN_REPORT, 0),
        ])
        self.pump_until(lambda: self.entry.get_text() == "HELLO", "final key release")

    def test_modifier_press_cancels_pending_auto_correction(self) -> None:
        self.prepare_word_editor()
        self.harness.type_word("ghbdtn")
        self.harness.send_events([
            (EV_KEY, KEY_SPACE, 1), (EV_SYN, SYN_REPORT, 0),
        ])
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn ", "held delimiter")
        self.pump_until(
            lambda: self.stats_fields()[1]["need_switch"] == "1", "auto decision"
        )
        self.harness.send_events([
            (EV_KEY, KEY_LEFTCTRL, 1), (EV_SYN, SYN_REPORT, 0),
            (EV_KEY, KEY_LEFTCTRL, 0), (EV_SYN, SYN_REPORT, 0),
            (EV_KEY, KEY_SPACE, 0), (EV_SYN, SYN_REPORT, 0),
        ])
        self.pump_for(0.2)
        self.assertEqual(self.entry.get_text(), "ghbdtn ")
        self.assertNotIn("Word edit dispatch status=", self.harness.diagnostic())

    def test_disable_cancels_pending_auto_correction(self) -> None:
        self.prepare_word_editor()
        self.pump_until(
            lambda: self.stats_fields()[1]["config_pending"] == "0",
            "initial config commit",
        )
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn", "source word")
        self.harness.send_events(
            [(EV_KEY, KEY_SPACE, 1), (EV_SYN, SYN_REPORT, 0)]
        )
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn ", "held space")
        self.pump_until(
            lambda: int(self.stats_fields()[1]["need_switch"]) >= 1,
            "pending automatic correction",
        )
        self.assertEqual(ipc_request(b"SET_STATUS 0\n"), b"OK DISABLED\n")
        self.harness.send_events(
            [(EV_KEY, KEY_SPACE, 0), (EV_SYN, SYN_REPORT, 0)]
        )
        self.pump_for(0.2)
        self.assertEqual(self.entry.get_text(), "ghbdtn ")
        self.assertNotIn("Word edit dispatch status=", self.harness.diagnostic())

    def test_auto_toggle_controls_real_text_without_changing_config(self) -> None:
        self.prepare_word_editor()
        self.pump_until(
            lambda: self.stats_fields()[1]["config_pending"] == "0",
            "initial config commit",
        )
        self.assertEqual(ipc_request(b"SET_STATUS 0\n"), b"OK DISABLED\n")
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn", "source word")
        self.harness.send_key(KEY_SPACE)
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn ", "disabled auto")
        self.pump_for(0.2)
        self.assertEqual(self.entry.get_text(), "ghbdtn ")
        self.assertEqual(self.stats_fields()[1]["configured_enabled"], "1")
        self.assertEqual(ipc_request(b"SET_STATUS 1\n"), b"OK ENABLED\n")
        self.harness.type_word("ghbdtn")
        self.pump_until(
            lambda: self.entry.get_text() == "ghbdtn ghbdtn", "second source word"
        )
        self.harness.send_key(KEY_SPACE)
        self.pump_until(
            lambda: self.entry.get_text() == "ghbdtn привет ", "enabled auto"
        )
        fields = self.stats_fields()[1]
        self.assertEqual(fields["enabled"], "1")
        self.assertEqual(fields["text_mutation"], "x11")
        self.assertEqual(fields["word_dispatches"], "1")

    def test_manual_word_edit_remains_available_when_auto_disabled(self) -> None:
        self.prepare_word_editor()
        self.pump_until(
            lambda: self.stats_fields()[1]["config_pending"] == "0",
            "initial config commit",
        )
        self.assertEqual(ipc_request(b"SET_STATUS 0\n"), b"OK DISABLED\n")
        self.harness.type_word("hELLo")
        self.pump_until(lambda: self.entry.get_text() == "hELLo", "source word")
        self.harness.hotkey(KEY_LEFTCTRL, repeat=False)
        self.pump_until(lambda: self.entry.get_text() == "HellO", "manual case edit")
        self.assertEqual(ipc_request(b"GET_STATUS\n"), b"OK DISABLED\n")

    def test_word_editor_rejects_unsupported_keyboard_map(self) -> None:
        self.prepare_word_editor()
        self.harness.type_word("hello")
        self.pump_until(lambda: self.entry.get_text() == "hello", "original word")
        subprocess.run(
            ["setxkbmap", "-display", self.x11.display, "-layout", "de,ru"],
            check=True, timeout=3,
        )
        self.harness.hotkey(KEY_LEFTCTRL, repeat=False)
        self.pump_until(
            lambda: "Word edit dispatch status=0" in self.harness.diagnostic(),
            "unsupported map rejection",
        )
        self.assertEqual(self.entry.get_text(), "hello")
        self.assertEqual(self.entry.get_selection_bounds(), ())

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

    def prepare_vte_line_editor(self, selected_source: str = "AbC"):
        self.prepare_word_editor()
        self.window.destroy()
        self.window = Gtk.Window(title="punto-event-loop-vte-line-e2e")
        self.window.set_wmclass("gnome-terminal-server", "Gnome-terminal")
        self.window.set_default_size(640, 320)
        terminal = Vte.Terminal()
        self.window.add(terminal)
        accelerator = Gtk.AccelGroup()
        self.window.add_accel_group(accelerator)

        def paste_clipboard(*_arguments):
            terminal.paste_clipboard()
            return True

        accelerator.connect(Gdk.KEY_v,
                            Gdk.ModifierType.CONTROL_MASK | Gdk.ModifierType.SHIFT_MASK,
                            Gtk.AccelFlags.VISIBLE, paste_clipboard)
        self.window.show_all()
        temporary = tempfile.TemporaryDirectory(prefix="punto-vte-line-")
        self.addCleanup(temporary.cleanup)
        received = pathlib.Path(temporary.name) / "received"
        reader = (
            "import pathlib, sys\n"
            "destination = pathlib.Path(sys.argv[1])\n"
            "print(sys.argv[2], flush=True)\n"
            "print('scrollback sentinel', flush=True)\n"
            "for line in sys.stdin:\n"
            "    with destination.open('a', encoding='utf-8') as stream:\n"
            "        stream.write(line)\n"
            "        stream.flush()\n"
        )
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", DeprecationWarning)
            spawned, child_pid = terminal.spawn_sync(
                Vte.PtyFlags.DEFAULT, None,
                [sys.executable, "-c", reader, str(received), selected_source],
                ["PATH=/usr/bin:/bin", "LANG=C.UTF-8", "LC_ALL=C.UTF-8"],
                GLib.SpawnFlags.DEFAULT, None, None, None)
        self.assertTrue(spawned)
        self.assertGreater(child_pid, 1)

        def stop_reader():
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

        self.addCleanup(stop_reader)
        terminal.grab_focus()
        self.pump_until(lambda: self.window.get_window().is_viewable(), "VTE window mapped")
        xid = self.window.get_window().get_xid()
        self.xdo("windowfocus", "--sync", str(xid))
        self.publish_active_window(xid)
        self.pump_until(lambda: "scrollback sentinel" in terminal.get_text_format(Vte.Format.TEXT),
                        "canonical PTY reader ready")
        Gtk.Clipboard.get(Gdk.SELECTION_PRIMARY).clear()
        return terminal, received

    def select_vte_source_word(self, terminal, source: str, recent_clicks: int = 0) -> None:
        pointer_events = []

        def record_pointer(_widget, event):
            pointer_events.append((event.type, event.time, event.x, event.y,
                                   int(event.state), getattr(event, "button", None)))
            return False

        for event_name in ("button-press-event", "button-release-event", "motion-notify-event"):
            terminal.connect(event_name, record_pointer)
        self.pump_until(
            lambda: terminal.get_mapped() and terminal.get_realized()
            and terminal.get_char_width() > 0 and terminal.get_char_height() > 0
            and terminal.get_allocated_width() > (len(source) + 1) * terminal.get_char_width()
            and terminal.get_allocated_height() > terminal.get_char_height()
            and terminal.get_text_format(Vte.Format.TEXT).startswith(source + "\n"),
            "visible first-row VTE source and allocated character cells")
        origin = terminal.translate_coordinates(self.window, 0, 0)
        self.assertIsNotNone(origin)
        padding = terminal.get_style_context().get_padding(Gtk.StateFlags.NORMAL)
        x = origin[0] + padding.left + 1
        y = origin[1] + padding.top + terminal.get_char_height() // 2
        xid = str(self.window.get_window().get_xid())
        deadline = time.monotonic() + EVENT_TIMEOUT

        def wait_receipt(predicate, description):
            self.pump_until(predicate, description, timeout=max(0.0, deadline - time.monotonic()))

        try:
            for _ in range(recent_clicks):
                try:
                    self.xdo("mousemove", "--window", xid, str(x), str(y), "mousedown", "1")
                    wait_receipt(lambda: any(event[0] == Gdk.EventType.BUTTON_PRESS and event[5] == 1
                                             for event in pointer_events), "VTE received prior click press")
                finally:
                    self.xdo("mouseup", "1")
                wait_receipt(lambda: any(event[0] == Gdk.EventType.BUTTON_RELEASE and event[5] == 1
                                         for event in pointer_events), "VTE received prior click release")
                pointer_events.clear()
            click_interval = terminal.get_settings().get_property("gtk-double-click-time") / 1000.0
            # GTK3 also recognizes triple clicks across twice this interval:
            # https://github.com/GNOME/gtk/blob/3.24.41/gdk/gdkevents.c#L2079
            single_click_at = time.monotonic() + 2 * click_interval + 0.001
            wait_receipt(lambda: time.monotonic() >= single_click_at,
                         "GTK double/triple-click recognition window elapsed")
            try:
                self.xdo("mousemove", "--window", xid, str(x), str(y), "mousedown", "1")
                wait_receipt(lambda: any(event[0] == Gdk.EventType.BUTTON_PRESS and event[5] == 1
                                         for event in pointer_events), "VTE received gesture button press")
                after_press = len(pointer_events)
                end_x = x + len(source) * terminal.get_char_width()
                self.xdo("mousemove", "--sync", "--window", xid, str(end_x), str(y))
                wait_receipt(lambda: any(
                    event[0] == Gdk.EventType.MOTION_NOTIFY
                    and event[4] & int(Gdk.ModifierType.BUTTON1_MASK)
                    and abs(event[2] - (end_x - origin[0])) <= 1
                    and abs(event[3] - (y - origin[1])) <= 1
                    for event in pointer_events[after_press:]), "VTE received held-button endpoint motion")
                after_motion = len(pointer_events)
            finally:
                self.xdo("mouseup", "1")
            wait_receipt(lambda: any(event[0] == Gdk.EventType.BUTTON_RELEASE and event[5] == 1
                                     for event in pointer_events[after_motion:]), "VTE received gesture release")
            self.assertFalse(any(event[0] in (Gdk.EventType.DOUBLE_BUTTON_PRESS,
                                              Gdk.EventType.TRIPLE_BUTTON_PRESS)
                                 for event in pointer_events), "drag must begin with a single click")
            wait_receipt(lambda: terminal.get_has_selection()
                         and self.selection_text(Gdk.SELECTION_PRIMARY) == source,
                         "real VTE drag selects exactly the source word")
        except AssertionError:
            print(f"VTE pointer diagnostic source={source!r} origin={origin!r} "
                  f"cell={terminal.get_char_width()}x{terminal.get_char_height()} "
                  f"selection={terminal.get_has_selection()} "
                  f"primary={self.selection_text(Gdk.SELECTION_PRIMARY)!r} "
                  f"events={pointer_events!r} text={terminal.get_text_format(Vte.Format.TEXT)!r}",
                  file=sys.stderr)
            raise

    def test_vte_word_layout_correction_reaches_real_pty(self) -> None:
        terminal, received = self.prepare_vte_line_editor()
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: "ghbdtn" in terminal.get_text_format(Vte.Format.TEXT),
                        "terminal physical source word")
        self.harness.send_key(KEY_PAUSE)
        self.pump_until(lambda: "привет" in terminal.get_text_format(Vte.Format.TEXT),
                        "terminal corrected line")
        self.assertFalse(received.exists(), "correction must not submit a command")
        self.assertIn("scrollback sentinel", terminal.get_text_format(Vte.Format.TEXT))
        self.harness.send_key(KEY_ENTER)
        self.pump_until(lambda: received.exists() and received.read_bytes() == "привет\n".encode(),
                        "exact corrected canonical PTY input")
        self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD), "startup clipboard baseline")

    def assert_vte_selection_inserts(self, source: str, transformed: str,
                                     modifiers: tuple[int, ...], recent_clicks: int = 0) -> None:
        terminal, received = self.prepare_vte_line_editor(source)
        self.harness.type_word("prefix")
        self.pump_until(lambda: "prefix" in terminal.get_text_format(Vte.Format.TEXT),
                        "nonempty live terminal input")
        self.select_vte_source_word(terminal, source, recent_clicks)
        self.assertEqual(self.selection_text(Gdk.SELECTION_PRIMARY), source)
        self.send_chord(modifiers)
        self.pump_until(lambda: "prefix" + transformed in terminal.get_text_format(Vte.Format.TEXT),
                        "transformed selection inserted without deleting live input")
        self.assertFalse(received.exists(), "selection insertion must not submit a command")
        self.assertIn(source + "\n", terminal.get_text_format(Vte.Format.TEXT))
        self.assertIn("scrollback sentinel", terminal.get_text_format(Vte.Format.TEXT))
        self.pump_until(lambda: self.selection_text(Gdk.SELECTION_CLIPBOARD) == "startup clipboard baseline",
                        "terminal paste restores clipboard after transfer")
        self.harness.send_key(KEY_ENTER)
        self.pump_until(lambda: received.exists() and received.read_bytes() == ("prefix" + transformed + "\n").encode(),
                        "exact canonical PTY selection insertion")

    def test_vte_selection_case_inserts_without_deleting_scrollback(self) -> None:
        self.assert_vte_selection_inserts("AbC", "aBc", (KEY_LEFTALT,))

    def test_vte_selection_layout_inserts_without_deleting_scrollback(self) -> None:
        self.assert_vte_selection_inserts("ghbdtn", "привет", (KEY_LEFTSHIFT,), recent_clicks=1)

    def test_vte_selection_layout_after_recent_click_preserves_exact_selection(self) -> None:
        self.assert_vte_selection_inserts("ghbdtn", "привет", (KEY_LEFTSHIFT,), recent_clicks=2)

    def test_vte_selection_translit_has_full_modifier_priority(self) -> None:
        self.assert_vte_selection_inserts("привет", "privet", (KEY_LEFTCTRL, KEY_LEFTALT, KEY_LEFTSHIFT))

    def assert_vte_selection_transform_is_rejected(
        self, missing_class: bool, clipboard_sentinel: str
    ) -> None:
        terminal, received = self.prepare_vte_line_editor()
        self.harness.type_word("prefix")
        self.pump_until(lambda: "prefix" in terminal.get_text_format(Vte.Format.TEXT), "real terminal input before rejected selection")
        xid = self.window.get_window().get_xid()
        if missing_class:
            self.select_vte_source_word(terminal, "AbC")
            subprocess.run(["xprop", "-display", self.x11.display, "-id", str(xid), "-remove", "WM_CLASS"],
                           check=True, timeout=3)
            wm_class = subprocess.run(["xprop", "-display", self.x11.display, "-id", str(xid), "WM_CLASS"],
                                      check=True, capture_output=True, text=True, timeout=3).stdout
            self.assertIn("not found", wm_class)
        else:
            terminal.select_all()
            self.pump_until(terminal.get_has_selection, "real multiline VTE selection")
        primary_before = self.selection_text(Gdk.SELECTION_PRIMARY)
        self.assertIsNotNone(primary_before, "VTE selection must own PRIMARY")
        if missing_class:
            self.assertEqual(primary_before, "AbC")
        else:
            self.assertIn("\n", primary_before)
            self.assertIn("AbC", primary_before)
        terminal_before = terminal.get_text_format(Vte.Format.TEXT)
        key_events = []
        terminal.connect("key-press-event", lambda _widget, event: key_events.append(Gdk.keyval_name(event.keyval)) or False)
        self.set_selection(Gdk.SELECTION_CLIPBOARD, clipboard_sentinel)
        before = self.harness.relay.snapshot()
        self.harness.hotkey(KEY_LEFTALT, repeat=True)
        self.pump_until(lambda: "Word edit dispatch status=0" in self.harness.diagnostic(), "terminal selection pre-dispatch rejection")
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "0")
        self.assertEqual(terminal.get_text_format(Vte.Format.TEXT), terminal_before)
        self.assertFalse(received.exists(), "rejected selection must not submit terminal input")
        self.assertFalse(any(key in {"v", "V", "BackSpace", "Left", "Right", "Return"} for key in key_events))
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
        self.harness.send_key(KEY_ENTER)
        self.pump_until(lambda: received.exists() and received.read_bytes() == b"prefix\n",
                        "rejected transformation preserves exact canonical PTY input")

    def test_vte_selection_transform_is_rejected_before_dispatch(self) -> None:
        self.assert_vte_selection_transform_is_rejected(
            False, "vte clipboard sentinel"
        )

    def test_custom_vte_unknown_wm_class_is_rejected_before_dispatch(self) -> None:
        self.assert_vte_selection_transform_is_rejected(
            True, "custom vte clipboard sentinel"
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
    require_private_network_namespace()
    test_name = os.environ.get("PUNTO_EVENT_LOOP_E2E_TEST")
    arguments = [sys.argv[0]]
    if test_name:
        arguments.extend(f"EventLoopGtkE2E.{name}" for name in test_name.split(","))
    unittest.main(argv=arguments, verbosity=2)
