#!/usr/bin/env python3

import ctypes
import ctypes.util
import array
import fcntl
import hashlib
import json
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

PASTE_CASE_SOURCE = "AbC🙂"
PASTE_CASE_REPLACEMENT = "aBc🙂"
PASTE_CASE_ORIGINAL = "before " + PASTE_CASE_SOURCE + " after"
PASTE_CASE_RESULT = "before " + PASTE_CASE_REPLACEMENT + " after"
PASTE_CASE_BOUNDS = (7, 7 + len(PASTE_CASE_SOURCE))

EV_SYN = 0
EV_KEY = 1
SYN_REPORT = 0
KEY_BACKSPACE = 14
KEY_TAB = 15
KEY_ENTER = 28
KEY_LEFTCTRL = 29
KEY_GRAVE = 41
KEY_LEFTSHIFT = 42
KEY_BACKSLASH = 43
KEY_C = 46
KEY_V = 47
KEY_DOT = 52
KEY_RIGHTSHIFT = 54
KEY_LEFTALT = 56
KEY_SPACE = 57
KEY_NUMLOCK = 69
KEY_RIGHTCTRL = 97
KEY_RIGHTALT = 100
KEY_F8 = 66
KEY_LEFT = 105
KEY_RIGHT = 106
KEY_END = 107
KEY_PAUSE = 119
KEY_LEFTMETA = 125
KEY_RIGHTMETA = 126
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


def process_start_time(pid: int) -> str | None:
    try:
        fields = pathlib.Path(f"/proc/{pid}/stat").read_text().rpartition(")")[2].split()
    except (FileNotFoundError, ProcessLookupError):
        return None
    return fields[19]


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


class XKeyEvent(ctypes.Structure):
    _fields_ = [
        ("type", ctypes.c_int), ("serial", ctypes.c_ulong),
        ("send_event", ctypes.c_int), ("display", ctypes.c_void_p),
        ("window", ctypes.c_ulong), ("root", ctypes.c_ulong),
        ("subwindow", ctypes.c_ulong), ("time", ctypes.c_ulong),
        ("x", ctypes.c_int), ("y", ctypes.c_int),
        ("x_root", ctypes.c_int), ("y_root", ctypes.c_int),
        ("state", ctypes.c_uint), ("keycode", ctypes.c_uint),
        ("same_screen", ctypes.c_int),
    ]


class XEvent(ctypes.Union):
    _fields_ = [("type", ctypes.c_int), ("key", XKeyEvent),
                ("padding", ctypes.c_long * 24)]


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


def run_in_sandbox(driver: pathlib.Path,
                   session_bridge: pathlib.Path | None = None,
                   test_script: pathlib.Path | None = None) -> int:
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
        "--dir",
        "/run/user",
        "--dir",
        "/run/user/0",
        "--tmpfs",
        "/tmp",
        "--tmpfs",
        "/etc",
        "--ro-bind",
        str(driver.resolve()),
        "/tmp/punto-event-loop-e2e-driver",
        "--setenv",
        "PATH",
        (os.environ["PUNTO_PROTO_TOOL_DIR"] + ":/usr/bin:/bin"
         if os.environ.get("PUNTO_PROTO_TOOL_DIR") else "/usr/bin:/bin"),
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
        "PUNTO_E2E_OUTER_PID_NAMESPACE",
        os.readlink("/proc/self/ns/pid"),
        "--setenv",
        "PUNTO_RUNTIME_GID",
        "0",
    ]
    if session_bridge is not None:
        command.extend(("--ro-bind", str(session_bridge.resolve()),
                        "/tmp/punto-session-bridge"))
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
        "GTK_MODULES",
        "GTK_IM_MODULE",
        "GTK_IM_MODULE_FILE",
        "PUNTO_PROTO_IM_DELAY_MS",
        "PUNTO_EVENT_LOOP_E2E_TEST",
        "PUNTO_EVENT_LOOP_E2E_ORIGIN_TRACE",
        "PUNTO_EVENT_LOOP_E2E_SHARD",
        "PUNTO_E2E_CONFIG_SESSION_RACE",
        "PUNTO_REAL_LAYOUT_BRIDGE_E2E",
        "PUNTO_ACCESSIBLE_CONTEXT_E2E",
        "PUNTO_DELIVERED_PROBE",
        "PUNTO_DELIVERED_CASE",
    ):
        value = os.environ.get(variable)
        if value:
            command.extend(("--setenv", variable, value))
    if os.environ.get("PUNTO_PREPARATION_OLD_COUNTERFACTUAL") == "1":
        command.extend(("--setenv", "PUNTO_PREPARATION_OLD_COUNTERFACTUAL", "1"))
    inner = [sys.executable, str((test_script or pathlib.Path(__file__)).resolve()),
             "/tmp/punto-event-loop-e2e-driver"]
    if session_bridge is not None:
        inner.append("/tmp/punto-session-bridge")
        command.extend(("--unsetenv", "NO_AT_BRIDGE", "--unsetenv", "GTK_MODULES",
                        "--setenv", "GSETTINGS_BACKEND", "memory"))
        inner = [shutil.which("dbus-run-session") or "dbus-run-session",
                 "--", *inner]
    command.extend(inner)
    try:
        completed = subprocess.run(command, timeout=120, check=False)
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
        self.delayed_key_release: tuple[int, float | None] | None = None
        self.key_release_blocked = threading.Event()
        self.permit_key_release = threading.Event()
        self.key_release_delivered = threading.Event()
        self.key_release_blocked_at = 0.0
        self.key_release_delivered_at = 0.0
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

    def arm_delayed_key_release(self, code: int, delay: float | None = None) -> None:
        self.key_release_blocked.clear()
        self.permit_key_release.clear()
        self.key_release_delivered.clear()
        self.delayed_key_release = (code, delay)

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

    def handle_non_key(self, event_type: int, code: int, value: int,
                       connection: int, x11) -> None:
        pass

    def handle_key_delivered(self, code: int, value: int) -> None:
        pass

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
                    self.handle_non_key(event_type, code, value,
                                        connection, x11)
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
                delayed_release = (value == 0 and self.delayed_key_release is not None
                                   and self.delayed_key_release[0] == code)
                if delayed_release:
                    _, delay = self.delayed_key_release
                    self.delayed_key_release = None
                    self.key_release_blocked_at = time.monotonic()
                    self.key_release_blocked.set()
                    permitted = self.permit_key_release.wait(
                        timeout=EVENT_TIMEOUT if delay is None else delay)
                    if delay is None and not permitted:
                        raise RuntimeError("delayed key release was never permitted")
                # XTest ignores a second press of an already held key. Evdev
                # repeat is a delivered keystroke, represented by X11's pair.
                if value == 2 and code in keys_down:
                    if xtst.XTestFakeKeyEvent(connection, code + 8, False, 0) == 0:
                        raise RuntimeError(f"XTest rejected repeat release {code}")
                if xtst.XTestFakeKeyEvent(connection, code + 8, pressed, 0) == 0:
                    raise RuntimeError(f"XTest rejected evdev code {code}")
                x11.XFlush(connection)
                self.handle_key_delivered(code, value)
                if delayed_release:
                    self.key_release_delivered_at = time.monotonic()
                    self.key_release_delivered.set()
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
        self.permit_key_release.set()
        self.reader.join(timeout=1)
        if break_pipe and not self.stream.closed:
            self.stream.close()
        self.injector.join(timeout=1)


class DesktopLayoutShortcut:
    """Private X11 stand-in for a desktop input-source shortcut manager."""

    def __init__(self, display: str, *, captured: bool = True,
                 handling_delay: float = 0.0, modifier: int = KEY_LEFTCTRL,
                 key: int = KEY_GRAVE, modifier_mask: int = 4) -> None:
        self.display = display
        self.captured = captured
        self.handling_delay = handling_delay
        self.modifier = modifier
        self.key = key
        self.modifier_mask = modifier_mask
        self.activations = 0
        self.source_index = 0
        self.enabled = True
        self.activation_limit: int | None = None
        self.focus_transition: tuple[int, int] | None = None
        self.focus_transitions = 0
        self.selection_disturbance = False
        self.selection_disturbances = 0
        self.reassert_source = False
        self.source_reassertions = 0
        self.before_activation = None
        self.block_next_activation = threading.Event()
        self.activation_started = threading.Event()
        self.permit_activation = threading.Event()
        self.source_only_next_activation = threading.Event()
        self.error: BaseException | None = None
        self.ready = threading.Event()
        self.stop_requested = threading.Event()
        self.thread = threading.Thread(target=self._run, daemon=True)

    def start(self) -> None:
        self.thread.start()
        if not self.ready.wait(timeout=START_TIMEOUT):
            raise RuntimeError("desktop layout shortcut did not start")
        if self.error is not None:
            raise self.error

    def _run(self) -> None:
        x11 = ctypes.CDLL(ctypes.util.find_library("X11"))
        x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
        x11.XOpenDisplay.restype = ctypes.c_void_p
        x11.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
        x11.XDefaultRootWindow.restype = ctypes.c_ulong
        x11.XGrabKey.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_uint,
                                 ctypes.c_ulong, ctypes.c_int, ctypes.c_int,
                                 ctypes.c_int]
        x11.XPending.argtypes = [ctypes.c_void_p]
        x11.XPending.restype = ctypes.c_int
        x11.XNextEvent.argtypes = [ctypes.c_void_p, ctypes.POINTER(XEvent)]
        x11.XkbGetState.argtypes = [ctypes.c_void_p, ctypes.c_uint,
                                    ctypes.POINTER(XkbState)]
        x11.XkbGetState.restype = ctypes.c_int
        x11.XkbLockGroup.argtypes = [ctypes.c_void_p, ctypes.c_uint,
                                     ctypes.c_uint]
        x11.XkbLockGroup.restype = ctypes.c_int
        x11.XSetInputFocus.argtypes = [ctypes.c_void_p, ctypes.c_ulong,
                                       ctypes.c_int, ctypes.c_ulong]
        x11.XSync.argtypes = [ctypes.c_void_p, ctypes.c_int]
        x11.XCloseDisplay.argtypes = [ctypes.c_void_p]
        xtst = ctypes.CDLL(ctypes.util.find_library("Xtst"))
        xtst.XTestFakeKeyEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint,
                                           ctypes.c_int, ctypes.c_ulong]
        xtst.XTestFakeKeyEvent.restype = ctypes.c_int

        connection = x11.XOpenDisplay(self.display.encode("ascii"))
        if not connection:
            self.error = RuntimeError("desktop layout shortcut could not open Xvfb")
            self.ready.set()
            return
        root = x11.XDefaultRootWindow(connection)
        try:
            state = XkbState()
            if x11.XkbGetState(connection, 0x0100, ctypes.byref(state)) != 0:
                raise RuntimeError("desktop shortcut could not read initial XKB group")
            if state.group != self.source_index:
                raise RuntimeError("desktop source and initial XKB group differ")
            # ControlMask=4, LockMask=2, Mod2Mask=16, Mod3Mask=32,
            # GrabModeAsync=1. The suite also remaps NumLock from Mod2 to Mod3,
            # so the desktop stand-in owns both valid lock variants.
            if self.captured:
                for locks in (0, 2, 16, 18, 32, 34):
                    x11.XGrabKey(connection, self.key + 8,
                                 self.modifier_mask | locks, root,
                                 False, 1, 1)
            x11.XSync(connection, False)
            self.ready.set()
            while not self.stop_requested.is_set():
                while x11.XPending(connection):
                    event = XEvent()
                    x11.XNextEvent(connection, ctypes.byref(event))
                    if event.type != 2 or event.key.keycode != self.key + 8:
                        continue
                    if not self.enabled:
                        continue
                    if (self.activation_limit is not None and
                            self.activations >= self.activation_limit):
                        continue
                    if self.block_next_activation.is_set():
                        self.block_next_activation.clear()
                        self.activation_started.set()
                        if not self.permit_activation.wait(
                                timeout=EVENT_TIMEOUT + 8.0):
                            raise RuntimeError("blocked desktop activation was never released")
                    if self.focus_transition is not None:
                        transient, restore = self.focus_transition
                        x11.XSetInputFocus(connection, transient, 1, 0)
                        x11.XSync(connection, False)
                        self.focus_transitions += 1
                        time.sleep(0.1)
                        x11.XSetInputFocus(connection, restore, 1, 0)
                        x11.XSync(connection, False)
                    if self.selection_disturbance:
                        if (xtst.XTestFakeKeyEvent(connection, KEY_RIGHT + 8,
                                                  True, 0) == 0 or
                                xtst.XTestFakeKeyEvent(connection, KEY_RIGHT + 8,
                                                      False, 0) == 0):
                            raise RuntimeError(
                                "desktop shortcut could not disturb selection")
                        x11.XSync(connection, False)
                        self.selection_disturbances += 1
                    if self.handling_delay:
                        time.sleep(self.handling_delay)
                    if self.before_activation is not None:
                        completed = threading.Event()

                        def apply_transition():
                            try:
                                self.before_activation()
                            except BaseException as error:
                                self.error = error
                            finally:
                                completed.set()
                            return False

                        GLib.idle_add(apply_transition)
                        if not completed.wait(timeout=2.0):
                            raise RuntimeError("desktop transition callback did not complete")
                        if self.error is not None:
                            raise self.error
                    self.source_index = 1 - self.source_index
                    if not self.source_only_next_activation.is_set():
                        if x11.XkbLockGroup(connection, 0x0100,
                                            self.source_index) == 0:
                            raise RuntimeError(
                                "desktop shortcut could not switch XKB group")
                    else:
                        self.source_only_next_activation.clear()
                    x11.XSync(connection, False)
                    self.activations += 1
                if self.reassert_source:
                    state = XkbState()
                    if x11.XkbGetState(connection, 0x0100, ctypes.byref(state)) != 0:
                        raise RuntimeError("desktop owner could not observe XKB group")
                    if state.group != self.source_index:
                        if x11.XkbLockGroup(connection, 0x0100, self.source_index) == 0:
                            raise RuntimeError("desktop owner could not reassert its source")
                        x11.XSync(connection, False)
                        self.source_reassertions += 1
                time.sleep(0.001)
        except BaseException as error:
            if not self.stop_requested.is_set():
                self.error = error
        finally:
            self.ready.set()
            x11.XCloseDisplay(connection)

    def stop(self) -> None:
        if not self.thread.is_alive():
            if self.error is not None:
                raise self.error
            return
        self.stop_requested.set()
        self.permit_activation.set()
        self.thread.join(timeout=1)
        if self.thread.is_alive():
            raise RuntimeError("desktop layout shortcut did not stop")
        if self.error is not None:
            raise self.error

    def align_source(self, group: int) -> None:
        if group not in (0, 1):
            raise ValueError("desktop source group must be 0 or 1")
        self.source_index = group

    def arm_blocked_activation(self) -> None:
        self.activation_started.clear()
        self.permit_activation.clear()
        self.block_next_activation.set()

    def arm_source_only_activation(self) -> None:
        self.source_only_next_activation.set()


class EventLoopHarness:
    def __init__(
        self,
        driver: pathlib.Path,
        display: str,
        extra_environment: dict[str, str] | None = None,
        relay_class: type[XTestRelay] = XTestRelay,
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
        self.relay = relay_class(self.process.stdout, display)
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
        self.relay.permit_key_release.set()
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


def ipc_request(command: bytes, timeout: float = EVENT_TIMEOUT, *, pump=None) -> bytes:
    return ipc_request_at("/run/punto.sock", command, timeout, pump=pump)


def ipc_request_at(path: str, command: bytes, timeout: float = EVENT_TIMEOUT,
                   *, pump=None) -> bytes:
    deadline = time.monotonic() + timeout if pump is not None else None

    def remaining():
        left = deadline - time.monotonic()
        if left <= 0:
            raise TimeoutError("foreground IPC deadline expired")
        return left

    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(remaining() if pump is not None else timeout)
        client.connect(path)
        if pump is not None:
            client.settimeout(remaining())
        client.sendall(command)
        if pump is not None:
            client.setblocking(False)
        response = bytearray()
        while b"\n" not in response:
            if pump is not None:
                if not select.select([client], [], [], min(0.002, remaining()))[0]:
                    pump()
                    remaining()
                    continue
                remaining()
            chunk = client.recv(512)
            if not chunk:
                break
            response.extend(chunk)
        if pump is not None:
            remaining()
        return bytes(response)


Gtk = None
Gdk = None
Vte = None
GLib = None
Gio = None
SESSION_BRIDGE = None


class RealLayoutBridgeFixture:
    """Private D-Bus endpoint that performs the adapter's XKB side effect."""

    EXTENSION_ID = "11" * 16
    LEASE_ID = "44" * 16

    def __init__(self, display: str) -> None:
        self.display = display
        self.role = 0
        self.state_generation = 8
        self.switches = 0
        self.suppress_xkb_once = False
        self.bound_helper: str | None = None
        self.operation_key: tuple[object, ...] | None = None
        self.operation_result: object | None = None
        self.hold_switch_reply = False
        self.hold_operation_reply = False
        self.switch_committed = threading.Event()
        self.operation_queried = threading.Event()
        self.switch_committed_us = 0
        self.switch_deadline_us = 0
        self.pending_invocations: list[object] = []
        self.hold_state_once = False
        self.state_requested = threading.Event()
        self.pending_state_invocation: object | None = None
        self.before_activation = None
        self.source_reassertions = 0
        self.reassert_timer = None
        self.error: BaseException | None = None
        self.connection = Gio.bus_get_sync(Gio.BusType.SESSION, None)
        reply = self.connection.call_sync(
            "org.freedesktop.DBus", "/org/freedesktop/DBus",
            "org.freedesktop.DBus", "RequestName",
            GLib.Variant("(su)", ("org.gnome.Shell", 4)),
            GLib.VariantType("(u)"), Gio.DBusCallFlags.NO_AUTO_START,
            1000, None)
        if reply.unpack()[0] != 1:
            raise RuntimeError("real layout fixture could not own Shell name")
        protocol = pathlib.Path(__file__).resolve().parents[1] / "protocol" / "punto-dbus-v1.xml"
        node = Gio.DBusNodeInfo.new_for_xml(protocol.read_text(encoding="ascii"))
        interface = node.lookup_interface("org.punto.Switcher.InputSource1")
        if interface is None:
            raise RuntimeError("layout bridge protocol interface missing")
        self.registration = self.connection.register_object(
            "/org/punto/Switcher/InputSource1", interface,
            self._call, None, None)

    def _lock_group(self, group: int) -> None:
        x11 = ctypes.CDLL(ctypes.util.find_library("X11"))
        x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
        x11.XOpenDisplay.restype = ctypes.c_void_p
        x11.XkbLockGroup.argtypes = [ctypes.c_void_p, ctypes.c_uint,
                                     ctypes.c_uint]
        x11.XkbLockGroup.restype = ctypes.c_int
        x11.XSync.argtypes = [ctypes.c_void_p, ctypes.c_int]
        x11.XCloseDisplay.argtypes = [ctypes.c_void_p]
        connection = x11.XOpenDisplay(self.display.encode("ascii"))
        if not connection:
            raise RuntimeError("layout bridge fixture could not open Xvfb")
        try:
            if x11.XkbLockGroup(connection, 0x0100, group) == 0:
                raise RuntimeError("layout bridge fixture could not lock group")
            x11.XSync(connection, False)
        finally:
            x11.XCloseDisplay(connection)

    def _operation(self, outcome: int, digest: str) -> object:
        return GLib.Variant(
            "(usstty)", (outcome, self.EXTENSION_ID, digest, 7,
                          self.state_generation, self.role))

    def _call(self, _connection, _sender, _path, _interface, method,
              parameters, invocation) -> None:
        values = parameters.unpack()
        if method == "BindHelper":
            self.bound_helper = values[0]
            invocation.return_value(GLib.Variant(
                "(us)", (0, self.EXTENSION_ID)))
            return
        if method == "GetState":
            if self.hold_state_once:
                self.hold_state_once = False
                self.pending_state_invocation = invocation
                self.state_requested.set()
                return
            invocation.return_value(GLib.Variant(
                "(ussttty)", (0, self.EXTENSION_ID, self.LEASE_ID,
                               GLib.get_monotonic_time() + 30_000_000,
                               7, self.state_generation, self.role)))
            return
        if method == "Switch":
            unresolved = pathlib.Path("/run/punto-e2e-force-layout-bridge-unresolved")
            if unresolved.exists():
                unresolved.unlink()
                self.operation_key = values[1:6]
                self.operation_result = self._operation(22, values[5])
                invocation.return_value(self.operation_result)
                return
            target = values[10]
            if self.before_activation is not None:
                try:
                    self.before_activation()
                except BaseException as error:
                    self.error = error
                    invocation.return_dbus_error("org.punto.Test.FixtureError", str(error))
                    return
            self.role = target
            self.state_generation += 1
            self.switches += 1
            self.operation_key = values[1:6]
            if self.suppress_xkb_once:
                self.suppress_xkb_once = False
            else:
                self._lock_group(target)
            self.operation_result = self._operation(1, values[5])
            if self.hold_switch_reply:
                self.switch_deadline_us = values[6]
                self.switch_committed_us = GLib.get_monotonic_time()
                self.pending_invocations.append(invocation)
                self.switch_committed.set()
                return
            invocation.return_value(self.operation_result)
            return
        if method == "GetOperation":
            if (values[0] == self.bound_helper and
                    values[1:6] == self.operation_key and
                    self.operation_result is not None):
                self.operation_queried.set()
                if self.hold_operation_reply:
                    self.pending_invocations.append(invocation)
                    return
                invocation.return_value(self.operation_result)
            else:
                invocation.return_value(self._operation(18, values[5]))
            return
        invocation.return_value(self._operation(18, values[5]))

    def arm_result_loss(self) -> None:
        self.switch_committed.clear()
        self.operation_queried.clear()
        self.switch_committed_us = 0
        self.switch_deadline_us = 0
        self.hold_switch_reply = True
        self.hold_operation_reply = True

    def release_result_loss(self) -> None:
        self.hold_switch_reply = False
        self.hold_operation_reply = False
        self.pending_invocations.clear()

    def release_state(self) -> None:
        if self.pending_state_invocation is None:
            raise RuntimeError("no held GetState invocation")
        invocation = self.pending_state_invocation
        self.pending_state_invocation = None
        invocation.return_value(GLib.Variant(
            "(ussttty)", (0, self.EXTENSION_ID, self.LEASE_ID,
                           GLib.get_monotonic_time() + 30_000_000,
                           7, self.state_generation, self.role)))

    def close(self) -> None:
        self.stop_reasserting()
        self.connection.unregister_object(self.registration)
        self.connection.close_sync(None)

    def start_reasserting(self, observe_group) -> None:
        self.stop_reasserting()
        self.source_reassertions = 0

        def reassert():
            try:
                if observe_group() != self.role:
                    self._lock_group(self.role)
                    self.source_reassertions += 1
                self.reassert_timer = GLib.timeout_add(2, reassert)
                return False
            except BaseException as error:
                self.error = error
                self.reassert_timer = None
                return False

        self.reassert_timer = GLib.timeout_add(2, reassert)

    def stop_reasserting(self) -> None:
        if self.reassert_timer is not None:
            GLib.source_remove(self.reassert_timer)
            self.reassert_timer = None
        if self.error is not None:
            raise self.error


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

        pathlib.Path("/run/user/0").mkdir(parents=True, mode=0o700, exist_ok=True)
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

        global Gtk, Gdk, Vte, GLib, Gio
        import gi

        gi.require_version("Gtk", "3.0")
        gi.require_version("Gdk", "3.0")
        gi.require_version("Vte", "2.91")
        from gi.repository import GLib as imported_glib
        from gi.repository import Gio as imported_gio

        GLib = imported_glib
        Gio = imported_gio
        cls.real_layout_bridge = None
        cls.helper_layout_bridge = None
        cls.session_bridge_process = None
        if os.environ.get("PUNTO_REAL_LAYOUT_BRIDGE_E2E") == "1" and SESSION_BRIDGE is None:
            raise RuntimeError("real layout bridge helper was not provided")
        if SESSION_BRIDGE is not None:
            cls.helper_layout_bridge = RealLayoutBridgeFixture(cls.x11.display)
            if os.environ.get("PUNTO_REAL_LAYOUT_BRIDGE_E2E") == "1":
                cls.real_layout_bridge = cls.helper_layout_bridge
            environment = os.environ.copy()
            environment["XDG_RUNTIME_DIR"] = "/run/user/0"
            state_directory = pathlib.Path("/tmp/punto-helper-state")
            state_directory.mkdir(mode=0o700)
            environment["STATE_DIRECTORY"] = str(state_directory)
            # This helper test binary supplies logind/proc fixtures. Real peer
            # admission is covered separately outside the private user namespace.
            proc_root = pathlib.Path("/tmp/punto-helper-auth")
            (proc_root / "self").mkdir(parents=True, exist_ok=True)
            (proc_root / "self" / "uid_map").write_text("0 0 4294967295\n")
            environment["PUNTO_TEST_PROC_ROOT"] = str(proc_root)
            cls.session_bridge_environment = environment
            cls.session_bridge_process = subprocess.Popen(
                [str(SESSION_BRIDGE)], env=environment,
                stdout=subprocess.PIPE, stderr=None, text=True)
            descriptor = pathlib.Path(
                "/run/user/0/punto/session-bridge.json")
            deadline = time.monotonic() + START_TIMEOUT
            while time.monotonic() < deadline and not descriptor.exists():
                GLib.MainContext.default().iteration(False)
                if cls.session_bridge_process.poll() is not None:
                    stdout, stderr = cls.session_bridge_process.communicate()
                    raise RuntimeError(
                        f"session bridge exited early: {stdout!r} {stderr!r}")
                time.sleep(0.005)
            if not descriptor.exists():
                raise RuntimeError("session bridge descriptor was not published")

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
        if cls.session_bridge_process is not None:
            cls.session_bridge_process.send_signal(signal.SIGTERM)
            cls.session_bridge_process.wait(timeout=3)
            if cls.session_bridge_process.returncode != 0:
                stdout, stderr = cls.session_bridge_process.communicate()
                raise RuntimeError(
                    f"session bridge teardown failed: {stdout!r} {stderr!r}")
        if cls.helper_layout_bridge is not None:
            cls.helper_layout_bridge.close()
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
        if self.real_layout_bridge is None:
            self.layout_shortcut = DesktopLayoutShortcut(self.x11.display)
            self.layout_shortcut.start()
            self.addCleanup(self.layout_shortcut.stop)
            self.filter_shortcut_grab_focus = True
            self.shortcut_grab_focus_events = []
            self._install_shortcut_focus_filter()
        else:
            self.layout_shortcut = None
            self.real_layout_bridge.role = 0
            self.real_layout_bridge.before_activation = None
            self.real_layout_bridge.error = None
            self.addCleanup(self.real_layout_bridge.stop_reasserting)
            self.addCleanup(setattr, self.real_layout_bridge, "before_activation", None)
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
        history_limits = {
            "test_word_history_bound_one_survives_no_correction_stream": 1,
            "test_word_history_bound_five_survives_no_correction_stream": 5,
            "test_history_rollover_keeps_newest_delayed_observation": 1,
            "test_raw_candidate_bound_one_keeps_newest_delayed_word": 1,
            "test_raw_candidate_bound_five_keeps_newest_delayed_word": 5,
        }
        if self._testMethodName in history_limits:
            path = pathlib.Path("/tmp/punto-home/.config/punto/config.yaml")
            original = path.read_text(encoding="utf-8")
            self.addCleanup(path.write_text, original, encoding="utf-8")
            path.write_text(
                original.replace(
                    "  max_rollback_words: 5\n",
                    f"  max_rollback_words: {history_limits[self._testMethodName]}\n",
                ),
                encoding="utf-8",
            )
        if self._testMethodName == "test_initial_user_config_sets_auto_default":
            path = pathlib.Path("/tmp/punto-home/.config/punto/config.yaml")
            original = path.read_text(encoding="utf-8")
            self.addCleanup(path.write_text, original, encoding="utf-8")
            path.write_text(
                original.replace("enabled: true", "enabled: false", 1),
                encoding="utf-8",
            )
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
            "test_post_cleanup_right_session_change_invalidates_receipt",
        }:
            for marker_name in (
                "punto-e2e-switch-session",
                "punto-e2e-new-session-observed",
                "punto-e2e-new-session-committed",
                "punto-e2e-old-config-ready",
                "punto-e2e-release-old-config",
                "punto-e2e-new-config-loaded",
            ):
                pathlib.Path("/run", marker_name).unlink(missing_ok=True)
            release = pathlib.Path("/run/punto-e2e-release-old-config")
            self.addCleanup(release.touch, exist_ok=True)
            if self._testMethodName == (
                "test_post_cleanup_right_session_change_invalidates_receipt"
            ):
                release.touch(mode=0o600)
            harness_environment["PUNTO_E2E_CONFIG_SESSION_RACE"] = "1"
        if self._testMethodName == (
            "test_ru_layout_snapshot_drives_analysis_without_input_x11"
        ):
            harness_environment["PUNTO_E2E_OBSERVED_LAYOUT"] = "1"
        if self._testMethodName == (
            "test_twenty_six_initial_failures_recover_from_zero_dispatches"
        ):
            harness_environment["PUNTO_E2E_DIRECT_LAYOUT_SWITCH"] = "1"
        self.harness = EventLoopHarness(
            DRIVER, self.x11.display, harness_environment or None,
            relay_class=getattr(self, "relay_class", XTestRelay),
        )
        self.addCleanup(self.harness.stop)
        self.harness.wait_ready(self.pump_once)

    def _install_shortcut_focus_filter(self) -> None:
        class FocusChange(ctypes.Structure):
            _fields_ = [("type", ctypes.c_int), ("serial", ctypes.c_ulong),
                        ("send_event", ctypes.c_int), ("display", ctypes.c_void_p),
                        ("window", ctypes.c_ulong), ("mode", ctypes.c_int),
                        ("detail", ctypes.c_int)]

        class GenericCookie(ctypes.Structure):
            _fields_ = FocusChange._fields_[:4] + [
                ("extension", ctypes.c_int), ("evtype", ctypes.c_int),
                ("cookie", ctypes.c_uint), ("data", ctypes.c_void_p)]

        class XIFocusChange(ctypes.Structure):
            _fields_ = FocusChange._fields_[:4] + [
                ("extension", ctypes.c_int), ("evtype", ctypes.c_int),
                ("time", ctypes.c_ulong), ("deviceid", ctypes.c_int),
                ("sourceid", ctypes.c_int), ("detail", ctypes.c_int),
                ("root", ctypes.c_ulong), ("event", ctypes.c_ulong),
                ("child", ctypes.c_ulong), ("root_x", ctypes.c_double),
                ("root_y", ctypes.c_double), ("event_x", ctypes.c_double),
                ("event_y", ctypes.c_double), ("mode", ctypes.c_int)]

        gdk = ctypes.CDLL(ctypes.util.find_library("gdk-3"))
        xlib = ctypes.CDLL(ctypes.util.find_library("X11"))
        xlib.XQueryExtension.argtypes = [ctypes.c_void_p, ctypes.c_char_p,
                                       ctypes.POINTER(ctypes.c_int),
                                       ctypes.POINTER(ctypes.c_int),
                                       ctypes.POINTER(ctypes.c_int)]
        xi_opcode = ctypes.c_int()
        callback_type = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p,
                                        ctypes.c_void_p, ctypes.c_void_p)

        @callback_type
        def filter_focus(raw_event, _event, _data):
            event = ctypes.cast(raw_event, ctypes.POINTER(FocusChange)).contents
            mode = event.mode if event.type in (9, 10) else None
            if event.type == 35:
                cookie = ctypes.cast(raw_event, ctypes.POINTER(GenericCookie)).contents
                if cookie.evtype in (9, 10) and cookie.data:
                    if not xi_opcode.value:
                        event_base, error_base = ctypes.c_int(), ctypes.c_int()
                        xlib.XQueryExtension(cookie.display, b"XInputExtension",
                                            ctypes.byref(xi_opcode),
                                            ctypes.byref(event_base), ctypes.byref(error_base))
                    if cookie.extension == xi_opcode.value:
                        mode = ctypes.cast(cookie.data,
                                           ctypes.POINTER(XIFocusChange)).contents.mode
            if mode in (1, 2):
                self.shortcut_grab_focus_events.append((event.type, mode))
                # The legacy grab stand-in must not add focus loss absent from the real bridge.
                if self.filter_shortcut_grab_focus:
                    return 2
            return 0

        for name in ("gdk_window_add_filter", "gdk_window_remove_filter"):
            getattr(gdk, name).argtypes = [ctypes.c_void_p, callback_type, ctypes.c_void_p]
            getattr(gdk, name).restype = None
        gdk.gdk_window_add_filter(None, filter_focus, None)
        self.addCleanup(gdk.gdk_window_remove_filter, None, filter_focus, None)

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
            if (getattr(self, "layout_shortcut", None) is not None and
                    self.layout_shortcut.error is not None):
                raise self.layout_shortcut.error
            if (self.real_layout_bridge is not None and
                    self.real_layout_bridge.error is not None):
                raise self.real_layout_bridge.error
            time.sleep(0.002)
        diagnostic = ""
        if hasattr(self, "harness"):
            diagnostic = self.harness.diagnostic()
            diagnostic += f"\nprocess={self.harness.process.poll()}"
            diagnostic += f"\nrelay={self.harness.relay.snapshot()[-40:]}"
        if hasattr(self, "browser_state"):
            try:
                diagnostic += f"\nbrowser={self.browser_state()!r}"
            except Exception as error:
                diagnostic += f"\nbrowser_state_error={error!r}"
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

    def pointer_position(self) -> tuple[int, int]:
        fields = dict(line.split("=", 1) for line in
                      self.xdo("getmouselocation", "--shell").splitlines())
        return int(fields["X"]), int(fields["Y"])

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

    def selection_owner(self, name: str = "PRIMARY") -> int:
        x11 = ctypes.CDLL(ctypes.util.find_library("X11"))
        x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
        x11.XOpenDisplay.restype = ctypes.c_void_p
        x11.XInternAtom.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
        x11.XInternAtom.restype = ctypes.c_ulong
        x11.XGetSelectionOwner.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
        x11.XGetSelectionOwner.restype = ctypes.c_ulong
        x11.XCloseDisplay.argtypes = [ctypes.c_void_p]
        connection = x11.XOpenDisplay(self.x11.display.encode("ascii"))
        self.assertTrue(connection)
        try:
            atom = x11.XInternAtom(connection, name.encode("ascii"), 0)
            return x11.XGetSelectionOwner(connection, atom)
        finally:
            x11.XCloseDisplay(connection)

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

    def external_selection_text(self, selection: str = "clipboard") -> str:
        reader = subprocess.Popen(
            ["xclip", "-selection", selection, "-out"],
            env={"DISPLAY": self.x11.display, "PATH": "/usr/bin:/bin"},
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        self.pump_until(
            lambda: reader.poll() is not None,
            f"external {selection} read",
        )
        stdout, stderr = reader.communicate(timeout=1)
        self.assertEqual(reader.returncode, 0, stderr.decode(errors="replace"))
        return stdout.decode("utf-8")

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

    def pump_stats_once(self) -> None:
        if Gtk.events_pending():
            Gtk.main_iteration_do(False)

    def stats_fields(self) -> tuple[bytes, dict[str, str]]:
        if getattr(self, "_stats_wait_active", False):
            raise RuntimeError("nested foreground STATS wait")
        self._stats_wait_active = True
        try:
            response = ipc_request(b"STATS\n", timeout=0.5,
                                   pump=self.pump_stats_once)
        finally:
            self._stats_wait_active = False
        parts = response.decode("ascii", errors="strict").strip().split()
        self.assertGreater(len(parts), 1)
        self.assertEqual(parts[0], "OK")
        return response, dict(part.split("=", 1) for part in parts[1:])

    def wait_word_dispatches(self, expected: int, description: str) -> None:
        def dispatch_count_reached() -> bool:
            try:
                return int(self.stats_fields()[1]["word_dispatches"]) == expected
            except (ConnectionRefusedError, FileNotFoundError,
                    TimeoutError, socket.timeout):
                return False

        self.pump_until(dispatch_count_reached, description)

    def wait_analysis_ready(self) -> None:
        def diagnostic_analysis_ready() -> bool:
            _, fields = self.stats_fields()
            return (
                fields["config_pending"] == "0"
                and fields["config_result"] == "ok"
                and fields["analysis_health"] == "ready"
                and fields["worker_threads"] == "1"
                and fields["x11_health"] == "ready"
            )

        self.pump_until(diagnostic_analysis_ready, "initialized diagnostic analysis")

    def assert_no_correction_history_is_bounded(self, limit: int) -> None:
        self.prepare_word_editor()
        self.wait_analysis_ready()
        analyzed = int(self.stats_fields()[1]["analyzed"])
        word_count = 64
        for index in range(word_count):
            self.harness.type_word("hello")
            self.harness.send_key(KEY_SPACE)
            analyzed += 1
            self.pump_until(
                lambda: int(self.stats_fields()[1]["analyzed"]) >= analyzed,
                f"NoCorrection result {index + 1}",
            )
            fields = self.stats_fields()[1]
            self.assertLessEqual(int(fields["history_words"]), limit)
            self.assertLessEqual(int(fields["history_max_words"]), limit)

        prefix = "hello " * word_count
        self.assertEqual(self.entry.get_text(), prefix)
        self.harness.type_word("ghbdtn")
        self.harness.send_key(KEY_SPACE)
        self.pump_until(
            lambda: self.entry.get_text() == prefix + "привет ",
            "automatic correction after bounded NoCorrection stream",
        )
        self.assertEqual(self.entry.get_position(), len(prefix + "привет "))
        self.harness.type_word("a")
        self.pump_until(
            lambda: self.entry.get_text() == prefix + "привет ф",
            "physical input after bounded-history correction",
        )

    def test_word_history_bound_one_survives_no_correction_stream(self) -> None:
        self.assert_no_correction_history_is_bounded(1)

    def test_word_history_bound_five_survives_no_correction_stream(self) -> None:
        self.assert_no_correction_history_is_bounded(5)

    def test_history_rollover_keeps_newest_delayed_observation(self) -> None:
        self.prepare_word_editor()
        ready, release = self.arm_keyboard_observation(hold=True)
        self.harness.type_word("hello")
        self.harness.send_key(KEY_SPACE)
        self.pump_until(ready.exists, "oldest word observation held")
        self.harness.type_word("ghbdtn")
        self.harness.send_key(KEY_SPACE)
        self.pump_until(
            lambda: self.entry.get_text() == "hello ghbdtn ",
            "newest word entered across history rollover",
        )
        fields = self.stats_fields()[1]
        self.assertEqual(fields["history_words"], "1")
        self.assertEqual(fields["history_max_words"], "1")
        release.touch(mode=0o600)
        self.pump_until(lambda: not release.exists(), "old observation released")
        self.pump_until(
            lambda: self.entry.get_text() == "hello привет ",
            "newest delayed observation remains correctable",
        )
        self.assertEqual(self.entry.get_position(), len("hello привет "))
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "1")

    def assert_raw_candidate_queue_is_bounded(self, limit: int) -> None:
        self.prepare_word_editor()
        self.wait_analysis_ready()
        ready, release = self.arm_keyboard_observation(hold=True)
        source_words = ["ghbdtn"] + ["hello"] * 31 + ["ghbdtn"]
        self.harness.type_word(source_words[0])
        self.harness.send_key(KEY_SPACE)
        self.pump_until(ready.exists, "old raw candidate observation held")

        for word in source_words[1:]:
            self.harness.type_word(word)
            self.harness.send_key(KEY_SPACE)
        source_text = " ".join(source_words) + " "
        self.pump_until(lambda: self.entry.get_text() == source_text,
                        "raw candidate burst entered without analysis waits")

        fields = self.stats_fields()[1]
        self.assertLessEqual(int(fields["history_words"]), limit)
        self.assertLessEqual(int(fields["raw_queued_candidates"]), limit)
        self.assertLessEqual(int(fields["raw_candidates"]), limit + 1)

        release.touch(mode=0o600)
        self.pump_until(lambda: not release.exists(), "old raw observation released")
        expected = " ".join(source_words[:-1] + ["привет"]) + " "
        self.pump_until(lambda: self.entry.get_text() == expected,
                        "newest retained raw candidate corrected")
        self.assertEqual(self.entry.get_position(), len(expected))
        self.harness.type_word("a")
        self.pump_until(lambda: self.entry.get_text() == expected + "ф",
                        "physical input follows bounded raw recovery")
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "1")

    def test_raw_candidate_bound_one_keeps_newest_delayed_word(self) -> None:
        self.assert_raw_candidate_queue_is_bounded(1)

    def test_raw_candidate_bound_five_keeps_newest_delayed_word(self) -> None:
        self.assert_raw_candidate_queue_is_bounded(5)

    def test_auto_candidate_delimiter_bypasses_unresponsive_x11(self) -> None:
        self.wait_analysis_ready()
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

    def test_secondary_redundant_status_preserves_word_origin(self) -> None:
        self.prepare_word_editor()
        primary = self.stats_fields()[1]
        self.assertEqual(primary["control_plane"], "primary")
        self.assertEqual(primary["enabled"], "1")
        primary_pid = self.harness.process.pid
        primary_start = process_start_time(primary_pid)
        self.assertIsNotNone(primary_start)
        primary_output = self.harness.relay.snapshot()
        self.assertEqual(primary_output, [])
        secondary = EventLoopHarness(
            DRIVER, self.x11.display,
            relay_class=getattr(self, "relay_class", XTestRelay),
        )
        self.addCleanup(secondary.stop)
        secondary_pid = secondary.process.pid
        secondary_start = process_start_time(secondary_pid)
        self.assertIsNotNone(secondary_start)
        socket_path = f"/run/punto-{secondary_pid}.sock"
        settled: dict[str, dict[str, str]] = {}

        def secondary_fields() -> dict[str, str]:
            self.assertIsNone(secondary.process.poll(), secondary.diagnostic())
            self.assertEqual(process_start_time(secondary_pid), secondary_start)
            if secondary.relay.error is not None:
                raise secondary.relay.error
            if not pathlib.Path(socket_path).exists():
                return {}
            response = ipc_request_at(socket_path, b"STATS\n", timeout=0.5,
                                      pump=self.pump_stats_once)
            parts = response.decode("ascii", errors="strict").strip().split()
            self.assertEqual(parts[0], "OK")
            fields = dict(part.split("=", 1) for part in parts[1:])
            self.assertEqual(fields["control_plane"], "secondary")
            return fields

        def paired_config_committed() -> bool:
            first = self.stats_fields()[1]
            peer = secondary_fields()
            if not peer:
                return False
            ready = {"config_pending": "0", "config_result": "ok", "enabled": "1",
                     "configured_enabled": "1", "x11_health": "ready", "analysis_health": "ready"}
            if any(fields.get(key) != value for fields in (first, peer)
                   for key, value in ready.items()):
                return False
            if any(first[key] != peer[key]
                   for key in ("applied_config_generation", "status_generation")):
                return False
            last = self.stats_fields()[1]
            if any(last[key] != first[key] for key in
                   ("config_generation", "applied_config_generation", "status_generation",
                    "config_pending", "config_result")):
                return False
            self.assertEqual(last["control_plane"], "primary")
            settled.update(primary=last, secondary=peer)
            return True

        def await_secondary_status(generation: str, enabled: str) -> None:
            self.pump_until(
                lambda: secondary_fields().get("status_generation") == generation,
                f"secondary applied status generation {generation}",
            )
            fields = secondary_fields()
            self.assertEqual(fields["enabled"], enabled)
            self.assertEqual(fields["configured_enabled"], "1")
            self.assertEqual(fields["config_pending"], "0")
            self.assertEqual(fields["config_result"], "ok")
            self.assertEqual(fields["x11_health"], "ready")
            self.assertEqual(fields["analysis_health"], "ready")
            for key in ("config_generation", "applied_config_generation"):
                self.assertEqual(fields[key], settled["secondary"][key])

        def publish_status(enabled: str) -> None:
            before = self.stats_fields()[1]
            for key in ("config_generation", "applied_config_generation"):
                self.assertEqual(before[key], settled["primary"][key])
            self.assertEqual(ipc_request(f"SET_STATUS {enabled}\n".encode("ascii"),
                                        pump=self.pump_stats_once),
                             b"OK ENABLED\n" if enabled == "1" else b"OK DISABLED\n")
            after = self.stats_fields()[1]
            self.assertEqual(int(after["status_generation"]),
                             int(before["status_generation"]) + 1)
            for key in ("config_generation", "applied_config_generation"):
                self.assertEqual(after[key], before[key])
            self.assertEqual(after["config_pending"], "0")
            self.assertEqual(after["config_result"], "ok")
            await_secondary_status(after["status_generation"], enabled)

        try:
            self.pump_until(lambda: b"[punto-e2e] editor_warmed\n" in bytes(secondary.stderr),
                            "secondary completed idle editor preparation")
            self.pump_until(paired_config_committed,
                            "primary and secondary consumed the same settled config and status")
            origin_offset = len(secondary.stderr)
            secondary.type_word("ghbdtn")
            self.pump_until(lambda: self.entry.get_text() == "ghbdtn",
                            "physical secondary origin word")
            self.pump_until(
                lambda: any(line.startswith(b"[punto-e2e] origin_capture_result ")
                            and line.endswith(b" present=1")
                            for line in bytes(secondary.stderr)[origin_offset:].splitlines()),
                "secondary first physical character captured an editor origin",
            )
            for name, fields in (("primary", self.stats_fields()[1]),
                                 ("secondary", secondary_fields())):
                for key in ("config_generation", "applied_config_generation", "status_generation",
                            "config_pending", "config_result"):
                    self.assertEqual(fields[key], settled[name][key])
            publish_status("1")
            secondary.send_key(KEY_SPACE)
            self.pump_until(lambda: self.entry.get_text() == "привет ",
                            "secondary auto correction after redundant SET_STATUS 1")
            self.pump_until(lambda: secondary_fields()["word_dispatches"] == "1",
                            "secondary automatic dispatch completion")
            secondary.type_word("a")
            self.pump_until(lambda: self.entry.get_text() == "привет ф",
                            "secondary next physical glyph uses corrected RU source")
            self.assertEqual(self.keyboard_group(), 1)
            self.pump_until(lambda: self.external_selection_text() == "startup clipboard baseline",
                            "redundant ON preserved secondary clipboard")

            secondary.send_key(KEY_SPACE)
            self.pump_until(lambda: self.entry.get_text() == "привет ф ",
                            "secondary delimiter before OFF contrast")
            self.set_desktop_layout(0)
            secondary.type_word("ghbdtn")
            self.pump_until(lambda: self.entry.get_text() == "привет ф ghbdtn",
                            "secondary source before actual OFF transition")
            publish_status("0")
            secondary.send_key(KEY_SPACE)
            self.pump_until(lambda: self.entry.get_text() == "привет ф ghbdtn ",
                            "consumed OFF preserves ordinary secondary word")
            self.pump_until(lambda: secondary_fields()["input_in_flight"] == "0",
                            "disabled secondary input drained")
            self.assertEqual(secondary_fields()["word_dispatches"], "1")
            self.assertEqual(self.entry.get_text(), "привет ф ghbdtn ")
            publish_status("1")
            secondary.type_word("ghbdtn")
            self.pump_until(lambda: self.entry.get_text() == "привет ф ghbdtn ghbdtn",
                            "fresh secondary source after enabling")
            secondary.send_key(KEY_SPACE)
            self.pump_until(lambda: self.entry.get_text() == "привет ф ghbdtn привет ",
                            "same secondary automatic recovery after actual ON transition")
            self.pump_until(lambda: secondary_fields()["word_dispatches"] == "2",
                            "secondary recovery dispatch completion")
            secondary.type_word("a")
            self.pump_until(lambda: self.entry.get_text() == "привет ф ghbdtn привет ф",
                            "fresh recovery next glyph follows RU source")
            self.pump_until(lambda: self.external_selection_text() == "startup clipboard baseline",
                            "secondary corrections restored unchanged clipboard")
            for code in (KEY_LEFTCTRL, KEY_RIGHTCTRL, KEY_LEFTSHIFT, KEY_RIGHTSHIFT,
                         KEY_LEFTALT, KEY_RIGHTALT, KEY_LEFTMETA, KEY_RIGHTMETA):
                self.assertFalse(self.key_is_down(code), f"modifier remains held: {code}")
            self.assertIsNone(secondary.process.poll())
            self.assertEqual(process_start_time(secondary_pid), secondary_start)
            self.assertIsNone(self.harness.process.poll())
            self.assertEqual(process_start_time(primary_pid), primary_start)
            self.assertEqual(self.harness.relay.snapshot(), primary_output)
        except Exception:
            print("[punto-e2e] secondary full diagnostic\n" +
                  bytes(secondary.stderr).decode("utf-8", errors="replace"), file=sys.stderr)
            raise

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
        block = pathlib.Path("/run/punto-e2e-block-refresh")
        blocked = pathlib.Path("/run/punto-e2e-refresh-blocked")
        self.addCleanup(block.unlink, missing_ok=True)
        self.addCleanup(blocked.unlink, missing_ok=True)
        block.touch(mode=0o600)
        self.pump_until(blocked.exists, "periodic session refresh held", timeout=4.5)
        progress = int(self.stats_fields()[1]["x11_last_progress_ms"])
        snapshots = self.harness.diagnostic().count("[fixture] session_snapshot")
        self.set_desktop_layout(1)
        self.assertEqual(self.keyboard_group(), 1)
        block.unlink()
        self.pump_until(
            lambda: self.harness.diagnostic().count("[fixture] session_snapshot") > snapshots,
            "fresh actual session snapshot after releasing refresh",
        )
        self.assertEqual(
            self.harness.diagnostic().rsplit("[fixture] session_snapshot group=", 1)[1][0],
            "1", "session refresh must sample current XKB, not a startup constant",
        )
        self.pump_until(
            lambda: int(self.stats_fields()[1]["x11_last_progress_ms"]) > progress
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
        self.wait_analysis_ready()
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
            pathlib.Path("/run/punto-e2e-new-session-committed").exists,
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

    def real_layout_bridge_manual_correction(self) -> None:
        self.prepare_word_editor()
        before = self.real_layout_bridge.switches
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn",
                        "real bridge source word")
        self.harness.send_key(KEY_PAUSE)
        self.pump_until(lambda: self.entry.get_text() == "привет",
                        "real bridge manual correction")
        self.assertEqual(self.real_layout_bridge.switches, before + 1)
        self.assertEqual(self.keyboard_group(), 1)
        self.wait_word_dispatches(1, "manual bridge dispatch completion")

    def real_layout_bridge_automatic_correction(self) -> None:
        self.prepare_word_editor()
        before = self.real_layout_bridge.switches
        self.harness.type_word("ghbdtn")
        self.harness.send_key(KEY_SPACE)
        self.pump_until(lambda: self.entry.get_text() == "привет ",
                        "real bridge automatic correction")
        self.assertEqual(self.real_layout_bridge.switches, before + 1)
        self.assertEqual(self.keyboard_group(), 1)
        self.wait_word_dispatches(1, "automatic bridge dispatch completion")

    def real_layout_bridge_helper_replacement_replays_once(self) -> None:
        self.prepare_word_editor()
        daemon_pid = self.harness.process.pid
        fixture = self.real_layout_bridge
        self.addCleanup(fixture.pending_invocations.clear)
        self.addCleanup(setattr, fixture, "hold_switch_reply", False)
        fixture.hold_switch_reply = True
        fixture.switch_committed.clear()
        before = fixture.switches
        replacement_errors: list[BaseException] = []

        def replace_helper() -> None:
            try:
                if not fixture.switch_committed.wait(timeout=2):
                    raise RuntimeError("layout operation was not committed")
                old_process = self.__class__.session_bridge_process
                old_process.kill()
                old_process.wait(timeout=2)
                time.sleep(0.15)
                new_process = subprocess.Popen(
                    [str(SESSION_BRIDGE)],
                    env=self.__class__.session_bridge_environment,
                    stdout=subprocess.PIPE, stderr=None, text=True)
                self.__class__.session_bridge_process = new_process
            except BaseException as error:
                replacement_errors.append(error)

        replacement = threading.Thread(target=replace_helper, daemon=True)
        self.addCleanup(replacement.join, timeout=3)
        replacement.start()
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn",
                        "replacement source word")
        self.harness.send_key(KEY_PAUSE)
        self.pump_until(
            lambda: "Word edit dispatch status=1" in self.harness.diagnostic(),
            "old helper child proof refuses before replacement", timeout=6)
        replacement.join(timeout=2)
        self.assertFalse(replacement.is_alive())
        self.assertEqual(replacement_errors, [])
        self.assertIsNone(self.__class__.session_bridge_process.stderr,
                          "replacement diagnostics must not use an unread pipe")
        self.assertEqual(fixture.switches, before + 1)
        self.assertEqual(self.keyboard_group(), 1)
        self.assertEqual(self.entry.get_text(), "ghbdtn")
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "0")
        self.assertEqual(self.output_count(KEY_V), 0)
        self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD),
                         "startup clipboard baseline")
        fixture.hold_switch_reply = False
        fixture.pending_invocations.clear()

        self.harness.send_key(KEY_RIGHT)
        self.pump_until(lambda: self.entry.get_selection_bounds() == (),
                        "physical navigation collapses old preparation")
        fail_read = pathlib.Path("/run/punto-e2e-fail-next-primary-payload-read")
        failed_read = pathlib.Path("/run/punto-e2e-primary-payload-read-failed")
        for marker in (fail_read, failed_read):
            marker.unlink(missing_ok=True)
            self.addCleanup(marker.unlink, missing_ok=True)
        attempts = self.harness.diagnostic().count("Word edit dispatch status=")
        timeouts = self.harness.diagnostic().count(
            "Clipboard read failed selection=primary stage=selection_notify_timeout")
        lefts = sum(phase == "press" and name == "Left"
                    for phase, name, _time in self.key_events)
        fail_read.touch(mode=0o600)
        self.harness.send_key(KEY_PAUSE)
        self.pump_until(
            lambda: self.harness.diagnostic().count(
                "Word edit dispatch status=") > attempts,
            "unknown PRIMARY cannot authorize a fresh manual range")
        self.assertTrue(failed_read.exists())
        self.assertFalse(fail_read.exists())
        self.assertGreater(self.harness.diagnostic().count(
            "Clipboard read failed selection=primary stage=selection_notify_timeout"),
            timeouts)
        self.assertEqual(self.entry.get_text(), "ghbdtn")
        self.assertEqual(self.entry.get_selection_bounds(), ())
        self.assertEqual(sum(phase == "press" and name == "Left"
                             for phase, name, _time in self.key_events), lefts)
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "0")
        self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD),
                         "startup clipboard baseline")
        for code in (KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_LEFTALT):
            self.assertFalse(self.key_is_down(code))
        self.harness.send_key(KEY_PAUSE)
        self.pump_until(lambda: self.entry.get_text() == "привет",
                        "fresh manual command after helper replacement")
        self.wait_word_dispatches(1, "replacement correction dispatch completion")
        self.assertEqual(fixture.switches, before + 1)
        self.harness.type_word("f")
        self.pump_until(lambda: self.entry.get_text() == "привета",
                        "next physical character uses recovered RU source")
        self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD),
                         "startup clipboard baseline")
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertIsNone(self.harness.process.poll())
        for code in (KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_LEFTALT):
            self.assertFalse(self.key_is_down(code))

    def real_layout_bridge_stalled_state_preserves_input_order(self) -> None:
        self.prepare_word_editor()
        fixture = self.real_layout_bridge
        fixture.hold_state_once = True
        fixture.state_requested.clear()
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn",
                        "stalled bridge source word")
        self.harness.send_key(KEY_PAUSE)
        self.pump_until(fixture.state_requested.is_set,
                        "held real bridge GetState")
        before_output = len(self.harness.relay.snapshot())
        self.harness.type_word("ab")
        fixture.release_state()
        self.pump_until(lambda: self.entry.get_text() == "приветфи",
                        "buffered physical input after bridge correction")
        letter_presses = [code for _timestamp, event_type, code, value
                          in self.harness.relay.snapshot()[before_output:]
                          if event_type == EV_KEY and value == 1 and
                          code in {LETTER_CODES["a"], LETTER_CODES["b"]}]
        self.assertEqual(letter_presses,
                         [LETTER_CODES["a"], LETTER_CODES["b"]])
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "1")

    def real_layout_bridge_xkb_mismatch_recovers(self) -> None:
        self.prepare_word_editor()
        daemon_pid = self.harness.process.pid
        self.real_layout_bridge.suppress_xkb_once = True
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn",
                        "real bridge mismatch source word")
        rejected = self.harness.diagnostic().count("Word edit dispatch status=0")
        self.harness.send_key(KEY_PAUSE)
        self.pump_until(
            lambda: self.harness.diagnostic().count(
                "Word edit dispatch status=0") > rejected,
            "real bridge XKB mismatch rejection")
        self.assertEqual(self.entry.get_text(), "ghbdtn")
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "0")

        self.real_layout_bridge.role = 0
        self.harness.send_key(KEY_SPACE)
        self.harness.type_word("ghbdtn")
        self.harness.send_key(KEY_PAUSE)
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn привет",
                        "real bridge recovery without daemon restart")
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.wait_word_dispatches(1, "real bridge recovery dispatch completion")

    def test_manual_word_hotkey_survives_concurrent_session_refresh(self) -> None:
        self.prepare_word_editor()
        self.harness.type_word("hello")
        self.pump_until(lambda: self.entry.get_text() == "hello", "source word")
        block = pathlib.Path("/run/punto-e2e-block-refresh")
        blocked = pathlib.Path("/run/punto-e2e-refresh-blocked")
        block.touch(mode=0o600)
        self.addCleanup(block.unlink, missing_ok=True)
        self.addCleanup(blocked.unlink, missing_ok=True)
        self.pump_until(blocked.exists, "periodic discovery overlap", timeout=4.5)
        self.send_chord((KEY_LEFTCTRL,))
        self.pump_until(
            lambda: self.entry.get_text() == "HELLO",
            "manual edit while discovery remains blocked",
        )
        block.unlink(missing_ok=True)


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

    def desktop_source_role(self) -> int:
        if self.real_layout_bridge is not None:
            return self.real_layout_bridge.role
        return self.layout_shortcut.source_index

    def desktop_activation_count(self) -> int:
        if self.real_layout_bridge is not None:
            return self.real_layout_bridge.switches
        return self.layout_shortcut.activations

    def desktop_owner(self):
        return self.real_layout_bridge or self.layout_shortcut

    def enable_desktop_reassertion(self) -> None:
        if self.real_layout_bridge is not None:
            self.real_layout_bridge.start_reasserting(self.keyboard_group)
        else:
            self.layout_shortcut.reassert_source = True

    def set_desktop_layout(self, group: int) -> None:
        if self.real_layout_bridge is not None:
            if self.real_layout_bridge.role != group:
                self.real_layout_bridge.role = group
                self.real_layout_bridge.state_generation += 1
        else:
            self.layout_shortcut.align_source(group)
        self.lock_keyboard_group(group)

    def test_desktop_source_does_not_follow_direct_xkb_write(self) -> None:
        self.prepare_word_editor()
        self.assertEqual(self.desktop_source_role(), 0)
        self.lock_keyboard_group(1)
        self.assertEqual(self.keyboard_group(), 1)
        self.assertEqual(self.desktop_source_role(), 0)
        self.send_chord((KEY_LEFTCTRL,), key=KEY_GRAVE)
        self.pump_until(lambda: self.layout_shortcut.activations == 1,
                        "desktop source activation after external XKB write")
        self.assertEqual(self.desktop_source_role(), 1)
        self.assertEqual(self.keyboard_group(), 1)

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

    def keyboard_group(self) -> int:
        x11 = ctypes.CDLL(ctypes.util.find_library("X11"))
        x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
        x11.XOpenDisplay.restype = ctypes.c_void_p
        x11.XkbGetState.argtypes = [ctypes.c_void_p, ctypes.c_uint,
                                    ctypes.POINTER(XkbState)]
        x11.XkbGetState.restype = ctypes.c_int
        x11.XCloseDisplay.argtypes = [ctypes.c_void_p]
        connection = x11.XOpenDisplay(self.x11.display.encode("ascii"))
        self.assertTrue(connection)
        try:
            state = XkbState()
            self.assertEqual(x11.XkbGetState(connection, 0x0100,
                                             ctypes.byref(state)), 0)
            return state.group
        finally:
            x11.XCloseDisplay(connection)

    def key_is_down(self, code: int) -> bool:
        x11 = ctypes.CDLL(ctypes.util.find_library("X11"))
        x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
        x11.XOpenDisplay.restype = ctypes.c_void_p
        x11.XQueryKeymap.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
        x11.XCloseDisplay.argtypes = [ctypes.c_void_p]
        connection = x11.XOpenDisplay(self.x11.display.encode("ascii"))
        self.assertTrue(connection, "key observation opens only the private Xvfb")
        try:
            keys = (ctypes.c_ubyte * 32)()
            self.assertNotEqual(x11.XQueryKeymap(connection, keys), 0)
            keycode = code + 8
            return bool(keys[keycode // 8] & (1 << (keycode % 8)))
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
        telemetry = self.stats_fields()[1]
        self.assertEqual(telemetry["word_dispatches"], "1")
        self.assertGreater(int(telemetry["avg_macro_us"]), 0)
        self.assertGreater(int(telemetry["avg_macro_payload_bytes"]), 0)
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

    def assert_locked_word_rejected(self, source: str, mask: int,
                                    *, after_native_range: bool = False) -> None:
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.entry.get_text() == source, "locked source word")
        self.harness.send_key(KEY_PAUSE)
        status = 1 if after_native_range else 0
        self.pump_until(lambda: f"Word edit dispatch status={status}" in self.harness.diagnostic(),
                        "incompatible lock/map refuses before any replacement")
        self.assertEqual(self.entry.get_text(), source)
        self.assertEqual(self.entry.get_selection_bounds(), ())
        if after_native_range:
            self.assertIn("rejection_stage=current_word_text_plan", self.harness.diagnostic())
            self.assertEqual(sum(phase == "press" and name == "Left"
                                 for phase, name, _ in self.key_events), 1)
            self.assertEqual(self.entry.get_position(), len(source))
        else:
            self.assertFalse(any(name == "Left" for _, name, _ in self.key_events))
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "0")
        self.assertEqual(self.keyboard_locks(), mask)
        self.assertEqual(self.keyboard_group(), 0)
        self.assertEqual(self.output_count(KEY_PAUSE), 0)
        for key in (KEY_LEFTCTRL, KEY_LEFTSHIFT):
            self.assertFalse(self.key_is_down(key))
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
        self.assert_locked_word_rejected("ghbdtn", 16, after_native_range=True)
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
        self.set_desktop_layout(1)
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
        if self.real_layout_bridge is None:
            self.assertTrue(self.shortcut_grab_focus_events,
                            "legacy stand-in must exercise actual X11 grab notifications")

    def test_unfiltered_shortcut_grab_revokes_editor_authority(self) -> None:
        self.prepare_word_editor()
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn", "source before grab")
        self.filter_shortcut_grab_focus = False
        self.shortcut_grab_focus_events.clear()
        daemon_pid = self.harness.process.pid
        self.harness.send_key(KEY_PAUSE)
        self.pump_until(lambda: "Word edit dispatch status=" in self.harness.diagnostic(),
                        "unfiltered grab revokes editor authority")
        self.assertTrue(self.shortcut_grab_focus_events)
        self.assertEqual(self.entry.get_text(), "ghbdtn")
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "0")
        self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD),
                         "startup clipboard baseline")
        for key in (KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_LEFTALT):
            self.assertFalse(self.key_is_down(key))
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertIsNone(self.harness.process.poll())

    def test_immediate_external_layout_manual_correction(self) -> None:
        self.assert_fresh_layout_correction(automatic=False)

    def test_immediate_external_layout_auto_correction(self) -> None:
        self.assert_fresh_layout_correction(automatic=True)

    def test_desktop_source_without_xkb_transition_fails_closed(self) -> None:
        self.prepare_word_editor()
        fault = pathlib.Path("/run/punto-e2e-withhold-post-bridge-focus")
        consumed = pathlib.Path("/run/punto-e2e-post-bridge-focus-reply-withheld")
        for marker in (fault, consumed):
            marker.unlink(missing_ok=True)
            self.addCleanup(marker.unlink, missing_ok=True)
        fault.touch(mode=0o600)
        activations = self.desktop_activation_count()
        daemon_pid = self.harness.process.pid
        daemon_start = process_start_time(daemon_pid)
        clipboard_owner = self.selection_owner("CLIPBOARD")
        self.addCleanup(self.set_desktop_layout, 0)
        if self.real_layout_bridge is not None:
            self.addCleanup(setattr, self.real_layout_bridge, "suppress_xkb_once", False)
            self.real_layout_bridge.suppress_xkb_once = True
        else:
            self.layout_shortcut.arm_source_only_activation()
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn",
                        "GTK source before desktop/XKB mismatch")
        attempts = self.harness.diagnostic().count("Word edit dispatch status=")
        self.harness.send_key(KEY_PAUSE)
        self.pump_until(
            lambda: self.desktop_activation_count() == activations + 1 and
            self.desktop_source_role() == 1,
            "desktop source changed without XKB state",
        )
        self.pump_until(
            lambda: self.harness.diagnostic().count(
                "Word edit dispatch status=") > attempts,
            "desktop/XKB mismatch rejected",
        )
        self.assertEqual(self.keyboard_group(), 0)
        self.assertFalse(fault.exists())
        self.assertTrue(consumed.exists(), "genuine focus reply captured before deadline")
        self.assertRegex(
            self.harness.diagnostic(),
            r"selection admission stage=\S+ context_result=3 [^\n]*"
            r"focus_result=1 focus_reply=-1(?:\n|$)",
            "missing focus reply must be distinguishable from a successful NONE reply",
        )
        self.assertIn("Word edit dispatch status=0 rejection_stage=layout_bridge_xkb_timeout",
                      self.harness.diagnostic())
        if self.real_layout_bridge is not None:
            self.assertFalse(self.real_layout_bridge.suppress_xkb_once)
        self.assertEqual(self.entry.get_text(), "ghbdtn")
        self.pump_until(lambda: self.entry.get_selection_bounds() == (),
                        "desktop/XKB mismatch selection cleanup")
        self.assertEqual(self.entry.get_position(), len("ghbdtn"))
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "0")
        self.assertEqual(self.selection_owner("CLIPBOARD"), clipboard_owner)
        self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD),
                         "startup clipboard baseline")
        for key in (KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_LEFTALT):
            self.assertFalse(self.key_is_down(key))

        self.set_desktop_layout(0)
        rights = self.output_count(KEY_RIGHT, 0)
        self.harness.send_key(KEY_RIGHT)
        self.pump_until(lambda: self.output_count(KEY_RIGHT, 0) == rights + 1,
                        "physical navigation retires the rejected word")
        self.harness.send_key(KEY_SPACE)
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn ghbdtn",
                        "fresh word after desktop/XKB mismatch")
        self.harness.send_key(KEY_PAUSE)
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn привет",
                        "fresh manual correction after desktop/XKB mismatch")
        self.assertEqual(self.desktop_activation_count(), activations + 2)
        self.assertEqual(self.desktop_source_role(), 1)
        self.assertEqual(self.keyboard_group(), 1)
        self.assertEqual(self.entry.get_selection_bounds(), ())
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "1")
        self.harness.type_word("f")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn привета",
                        "following character uses recovered RU layout")
        self.pump_until(lambda: self.selection_text(Gdk.SELECTION_CLIPBOARD)
                        == "startup clipboard baseline", "manual clipboard restoration")
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertEqual(process_start_time(daemon_pid), daemon_start)
        self.assertIsNone(self.harness.process.poll())
        for key in (KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_LEFTALT):
            self.assertFalse(self.key_is_down(key))

    def test_equivalent_text_plan_with_changed_xkb_identity_fails_closed(
            self) -> None:
        self.prepare_word_editor()
        switches = self.real_layout_bridge.switches if self.real_layout_bridge else None
        marker = pathlib.Path(
            "/run/punto-e2e-mutate-xkb-identity-after-layout")
        consumed = pathlib.Path("/run/punto-e2e-xkb-map-identity-mutated")
        for path in (marker, consumed):
            path.unlink(missing_ok=True)
            self.addCleanup(path.unlink, missing_ok=True)
        marker.touch(mode=0o600)
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn",
                        "GTK source before XKB identity change")
        self.harness.send_key(KEY_PAUSE)
        self.pump_until(
            lambda: "rejection_stage=layout_bridge_keymap_changed"
            in self.harness.diagnostic(),
            "changed XKB identity rejected",
        )
        self.assertFalse(marker.exists())
        self.assertTrue(consumed.exists(), "actual map reply identity was changed")
        if switches is not None:
            self.assertEqual(self.real_layout_bridge.switches, switches + 1)
        self.assertEqual(self.keyboard_group(), 1)
        self.assertEqual(self.desktop_source_role(), 1)
        self.assertEqual(self.entry.get_text(), "ghbdtn")
        self.assertEqual(self.entry.get_selection_bounds(), ())
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "0")
        self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD),
                         "startup clipboard baseline")
        for key in (KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_LEFTALT):
            self.assertFalse(self.key_is_down(key))

    def test_post_bridge_xkb_map_failure_never_polls_closed_connection_and_recovers(
            self) -> None:
        self.prepare_word_editor()
        daemon_pid = self.harness.process.pid
        daemon_start = process_start_time(daemon_pid)
        switches = self.real_layout_bridge.switches if self.real_layout_bridge else None
        fault = pathlib.Path("/run/punto-e2e-fail-post-bridge-xkb-map")
        consumed = pathlib.Path("/run/punto-e2e-xkb-map-error-consumed")
        arm_lineage = pathlib.Path(
            "/run/punto-e2e-arm-cleanup-receipt-retained")
        lineage = pathlib.Path("/run/punto-e2e-cleanup-receipt-retained")
        unsafe_poll = pathlib.Path(
            "/run/punto-e2e-unsafe-raw-event-poll-on-closed-xcb")
        for marker in (fault, consumed, arm_lineage, lineage, unsafe_poll):
            marker.unlink(missing_ok=True)
            self.addCleanup(marker.unlink, missing_ok=True)
        fault.touch(mode=0o600)
        arm_lineage.touch(mode=0o600)
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn",
                        "word before post-bridge keymap fault")
        self.harness.send_key(KEY_PAUSE)
        self.pump_until(
            lambda: "rejection_stage=layout_bridge_keymap_changed"
            in self.harness.diagnostic(),
            "post-bridge keymap failure rejected",
        )
        self.assertFalse(fault.exists())
        self.assertTrue(consumed.exists(), "actual map reply returned protocol failure")
        if switches is not None:
            self.assertEqual(self.real_layout_bridge.switches, switches + 1)
        self.assertTrue(lineage.exists())
        self.assertFalse(unsafe_poll.exists())
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertIsNone(self.harness.process.poll())
        self.assertEqual(ipc_request(b"GET_STATUS\n"), b"OK ENABLED\n")
        self.assertEqual(self.entry.get_text(), "ghbdtn")
        self.assertEqual(self.entry.get_selection_bounds(), (0, 6))
        self.pump_for(0.05)
        self.assertEqual(self.entry.get_selection_bounds(), (0, 6))
        self.assertEqual(
            self.selection_text(Gdk.SELECTION_CLIPBOARD),
            "startup clipboard baseline",
        )
        self.assertEqual(self.keyboard_group(), 1)
        self.assertEqual(self.desktop_source_role(), 1)
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "0")

        before_right = len(self.harness.relay.snapshot())
        self.harness.send_key(KEY_RIGHT)
        self.pump_until(
            lambda: self.entry.get_selection_bounds() == (),
            "physical Right reconciles retained selection",
        )
        self.assertGreater(len(self.harness.relay.snapshot()), before_right)
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "0")
        self.harness.send_key(KEY_SPACE)
        self.pump_until(
            lambda: self.entry.get_text() == "ghbdtn ",
            "physical delimiter after safe rejection",
        )
        self.harness.type_word("hello")
        self.pump_until(
            lambda: self.entry.get_text() == "ghbdtn руддщ",
            "fresh physical word after post-bridge keymap failure",
        )
        self.harness.send_key(KEY_PAUSE)
        self.pump_until(
            lambda: self.entry.get_text() == "ghbdtn hello",
            "same-daemon recovery after post-bridge keymap failure",
        )
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "1")
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertEqual(process_start_time(daemon_pid), daemon_start)
        self.assertIsNone(self.harness.process.poll())
        self.harness.type_word("f")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn hellof",
                        "next ordinary key uses the recovered English source")
        self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD),
                         "startup clipboard baseline")
        for key in (KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_LEFTALT):
            self.assertFalse(self.key_is_down(key))

    def prepare_failed_primary_lineage(self) -> int:
        self.prepare_word_editor()
        daemon_pid = self.harness.process.pid
        names = ("fail-post-bridge-xkb-map", "arm-cleanup-receipt-retained",
                 "cleanup-receipt-retained")
        markers = {name: pathlib.Path("/run", "punto-e2e-" + name) for name in names}
        for marker in markers.values():
            marker.unlink(missing_ok=True)
            self.addCleanup(marker.unlink, missing_ok=True)
        markers["fail-post-bridge-xkb-map"].touch(mode=0o600)
        markers["arm-cleanup-receipt-retained"].touch(mode=0o600)
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn",
                        "source before actual post-bridge protocol fault")
        self.harness.send_key(KEY_PAUSE)
        self.pump_until(lambda: "rejection_stage=layout_bridge_keymap_changed"
                        in self.harness.diagnostic(), "post-bridge refusal")
        self.assertFalse(markers["fail-post-bridge-xkb-map"].exists())
        self.assertTrue(markers["cleanup-receipt-retained"].exists())
        self.assertEqual(self.entry.get_selection_bounds(), (0, 6))
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "0")
        self.pump_for(.05)
        return daemon_pid

    def assert_lineage_recovery_ends_in_en(self, expected: str, daemon_pid: int) -> None:
        self.pump_until(lambda: self.entry.get_text() == expected,
                        "fresh actual word succeeds after safe failure without restart")
        self.assertEqual(self.entry.get_selection_bounds(), ())
        self.assertEqual(self.entry.get_position(), len(expected))
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "1")
        self.assertEqual(self.keyboard_group(), 0)
        self.harness.type_word("f")
        self.pump_until(lambda: self.entry.get_text() == expected + "f",
                        "next physical character uses EN after recovery")
        self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD),
                         "startup clipboard baseline")
        for key in (KEY_LEFTCTRL, KEY_LEFTSHIFT):
            self.assertFalse(self.key_is_down(key))
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertIsNone(self.harness.process.poll())

    def test_failed_primary_stale_pause_does_not_consume_fresh_recovery(self) -> None:
        daemon_pid = self.prepare_failed_primary_lineage()
        lefts = sum(phase == "press" and name == "Left"
                    for phase, name, _ in self.key_events)
        attempts = self.harness.diagnostic().count("Word edit dispatch status=")
        self.harness.send_key(KEY_PAUSE)
        self.pump_until(lambda: self.harness.diagnostic().count(
            "Word edit dispatch status=") > attempts, "no-fresh Pause refused")
        self.assertIn("rejection_stage=retained_lineage_stale_input",
                      self.harness.diagnostic())
        self.assertEqual(self.entry.get_text(), "ghbdtn")
        self.assertEqual(self.entry.get_selection_bounds(), (0, 6))
        self.assertEqual(sum(phase == "press" and name == "Left"
                             for phase, name, _ in self.key_events), lefts,
                         "no new native selection or replacement before fresh input")
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "0")
        self.assertEqual(self.output_count(KEY_PAUSE), 0)
        self.harness.send_key(KEY_RIGHT)
        self.pump_until(lambda: self.entry.get_selection_bounds() == (),
                        "Right reconciles failed selection")
        self.harness.send_key(KEY_SPACE)
        self.harness.type_word("hello")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn руддщ",
                        "fresh physical word after stale refusal")
        self.harness.send_key(KEY_PAUSE)
        self.assert_lineage_recovery_ends_in_en("ghbdtn hello", daemon_pid)

    def test_failed_primary_none_uses_new_actual_range_without_old_replay(self) -> None:
        daemon_pid = self.prepare_failed_primary_lineage()
        self.entry.set_text("руддщ")
        self.entry.set_position(-1)
        Gtk.Clipboard.get(Gdk.SELECTION_PRIMARY).clear()
        self.pump_until(lambda: self.selection_owner() == 0,
                        "actual X server PRIMARY owner is NONE")
        self.assertEqual(self.entry.get_selection_bounds(), ())
        self.harness.send_key(KEY_PAUSE)
        self.assert_lineage_recovery_ends_in_en("hello", daemon_pid)

    def test_failed_primary_exact_readable_receipt_admits_new_range_after_fresh_input(self) -> None:
        daemon_pid = self.prepare_failed_primary_lineage()
        primary = Gtk.Clipboard.get(Gdk.SELECTION_PRIMARY)

        def primary_identity():
            timestamp = primary.wait_for_contents(Gdk.atom_intern("TIMESTAMP", False))
            self.assertIsNotNone(timestamp)
            self.assertEqual(timestamp.get_format(), 32)
            return self.selection_owner(), bytes(timestamp.get_data())

        identity = primary_identity()
        self.assertEqual(self.selection_text(Gdk.SELECTION_PRIMARY), "ghbdtn")
        intercepted = []

        def consume_character(_editor, event):
            if event.hardware_keycode == LETTER_CODES["f"] + 8:
                intercepted.append(True)
                return True
            return False

        handler = self.entry.connect("key-press-event", consume_character)
        try:
            self.harness.type_word("f")
            self.pump_until(lambda: bool(intercepted), "physical text key consumed by editor")
        finally:
            self.entry.disconnect(handler)
        self.assertEqual(self.entry.get_text(), "ghbdtn")
        self.assertEqual(self.entry.get_selection_bounds(), (0, 6))
        self.assertEqual(primary_identity(), identity)
        self.assertEqual(self.selection_text(Gdk.SELECTION_PRIMARY), "ghbdtn")
        lefts = sum(phase == "press" and name == "Left"
                    for phase, name, _ in self.key_events)
        attempts = self.harness.diagnostic().count("Word edit dispatch status=")
        self.harness.send_key(KEY_PAUSE)
        self.pump_until(lambda: self.harness.diagnostic().count(
            "Word edit dispatch status=") > attempts, "fresh native-range decision")
        self.assertGreater(sum(phase == "press" and name == "Left"
                               for phase, name, _ in self.key_events), lefts,
                           "exact failed identity admits a new native selection, not old replay")
        self.pump_until(lambda: self.entry.get_text() == "привет",
                        "fresh native receipt authorizes one new actual-range conversion")
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "1")
        self.assertEqual(self.entry.get_selection_bounds(), ())
        self.assertEqual(self.entry.get_position(), 6)
        self.assertEqual(self.keyboard_group(), 1)
        self.harness.type_word("f")
        self.pump_until(lambda: self.entry.get_text() == "привета",
                        "next physical character uses the recovered Russian source")
        self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD),
                         "startup clipboard baseline")
        for key in (KEY_LEFTCTRL, KEY_LEFTSHIFT):
            self.assertFalse(self.key_is_down(key))
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertIsNone(self.harness.process.poll())

    def test_post_bridge_failure_lineage_rejects_new_readable_selection(
            self) -> None:
        self.prepare_word_editor()
        daemon_pid = self.harness.process.pid
        fault = pathlib.Path("/run/punto-e2e-fail-post-bridge-xkb-map")
        for marker in (fault,):
            marker.unlink(missing_ok=True)
            self.addCleanup(marker.unlink, missing_ok=True)
        fault.touch(mode=0o600)
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn",
                        "word before failure-lineage negative")
        self.harness.send_key(KEY_PAUSE)
        self.pump_until(
            lambda: "rejection_stage=layout_bridge_keymap_changed"
            in self.harness.diagnostic(),
            "failure lineage established",
        )
        self.harness.send_key(KEY_RIGHT)
        self.pump_until(lambda: self.entry.get_selection_bounds() == (),
                        "physical Right before new selection")
        self.harness.send_key(KEY_SPACE)
        self.harness.type_word("hello")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn руддщ",
                        "fresh word before readable takeover")
        self.entry.select_region(7, 9)
        self.pump_until(
            lambda: self.selection_text(Gdk.SELECTION_PRIMARY) == "ру",
            "new readable same-client selection",
        )
        attempts = self.harness.diagnostic().count("Word edit dispatch status=")
        self.harness.send_key(KEY_PAUSE)
        self.pump_until(
            lambda: self.harness.diagnostic().count(
                "Word edit dispatch status=") > attempts,
            "new readable selection rejection",
        )
        self.assertIn("rejection_stage=retained_selection_untrusted",
                      self.harness.diagnostic())
        self.assertEqual(self.entry.get_text(), "ghbdtn руддщ")
        self.assertEqual(self.entry.get_selection_bounds(), (7, 9))
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "0")
        self.assertEqual(self.harness.process.pid, daemon_pid)

    def test_post_bridge_failure_lineage_rejects_primary_transport_failure(
            self) -> None:
        self.prepare_word_editor()
        daemon_pid = self.harness.process.pid
        fault = pathlib.Path("/run/punto-e2e-fail-post-bridge-xkb-map")
        transport = pathlib.Path(
            "/run/punto-e2e-fail-next-primary-transport")
        for marker in (fault, transport):
            marker.unlink(missing_ok=True)
            self.addCleanup(marker.unlink, missing_ok=True)
        fault.touch(mode=0o600)
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn",
                        "word before lineage transport negative")
        self.harness.send_key(KEY_PAUSE)
        self.pump_until(
            lambda: "rejection_stage=layout_bridge_keymap_changed"
            in self.harness.diagnostic(),
            "transport-negative lineage established",
        )
        self.harness.send_key(KEY_RIGHT)
        self.pump_until(lambda: self.entry.get_selection_bounds() == (),
                        "physical Right before transport failure")
        self.harness.send_key(KEY_SPACE)
        self.harness.type_word("hello")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn руддщ",
                        "fresh word before PRIMARY transport failure")
        right_presses = self.output_count(KEY_RIGHT)
        attempts = self.harness.diagnostic().count("Word edit dispatch status=")
        transport.touch(mode=0o600)
        self.harness.send_key(KEY_PAUSE)
        self.pump_until(
            lambda: self.harness.diagnostic().count(
                "Word edit dispatch status=") > attempts,
            "PRIMARY transport failure rejection",
        )
        self.assertIn("rejection_stage=retained_lineage_clipboard_transport",
                      self.harness.diagnostic())
        self.assertFalse(transport.exists())
        self.assertEqual(self.entry.get_text(), "ghbdtn руддщ")
        self.assertEqual(self.entry.get_selection_bounds(), ())
        self.assertEqual(self.output_count(KEY_RIGHT), right_presses)
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "0")
        self.assertEqual(ipc_request(b"GET_STATUS\n"), b"OK ENABLED\n")
        self.assertEqual(self.harness.process.pid, daemon_pid)

    def test_transient_resolver_xkb_map_is_rejected_before_selection(
            self) -> None:
        self.prepare_word_editor()
        marker = pathlib.Path(
            "/run/punto-e2e-mutate-resolve-xkb-map")
        marker.unlink(missing_ok=True)
        self.addCleanup(marker.unlink, missing_ok=True)
        marker.touch(mode=0o600)
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn",
                        "GTK source before resolver map change")
        attempts = self.harness.diagnostic().count("Word edit dispatch status=")
        self.harness.send_key(KEY_PAUSE)
        self.pump_until(
            lambda: self.harness.diagnostic().count(
                "Word edit dispatch status=") > attempts,
            "resolver map mismatch rejection")
        self.assertFalse(marker.exists())
        self.assertEqual(self.entry.get_text(), "ghbdtn")
        self.assertEqual(self.entry.get_selection_bounds(), ())
        self.assertEqual(self.selection_text(Gdk.SELECTION_PRIMARY), None)
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "0")

    def test_layout_timeout_retries_and_recovers_next_manual_word_without_restart(
            self) -> None:
        self.prepare_word_editor()
        activations = self.desktop_activation_count()
        daemon_pid = self.harness.process.pid
        daemon_start = process_start_time(daemon_pid)
        clipboard = self.selection_text(Gdk.SELECTION_CLIPBOARD)
        clipboard_owner = self.selection_owner("CLIPBOARD")
        marker = pathlib.Path("/run/punto-e2e-force-layout-bridge-unresolved")
        marker.unlink(missing_ok=True)
        self.addCleanup(marker.unlink, missing_ok=True)
        if self.real_layout_bridge is not None:
            self.real_layout_bridge.operation_queried.clear()
            generation = self.real_layout_bridge.state_generation
        marker.touch(mode=0o600)
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn", "GTK source word")
        rejected = self.harness.diagnostic().count(
            "Word edit dispatch status=0 rejection_stage=layout_bridge_switch_unresolved retry_policy=1")
        self.harness.send_key(KEY_PAUSE)
        self.pump_until(
            lambda: self.harness.diagnostic().count(
                "Word edit dispatch status=0 rejection_stage=layout_bridge_switch_unresolved retry_policy=1") > rejected,
            "GTK unresolved bridge operation",
            timeout=4,
        )
        self.assertFalse(marker.exists(), "original unresolved fault consumed")
        if self.real_layout_bridge is not None:
            fixture = self.real_layout_bridge
            self.assertTrue(fixture.operation_queried.is_set(),
                            "matching query observed for the current Unknown operation")
            self.assertTrue(fixture.operation_key)
            self.assertEqual(fixture.operation_result.unpack(),
                             (22, fixture.EXTENSION_ID, fixture.operation_key[4],
                              7, generation, 0))
            self.assertEqual(fixture.state_generation, generation)
            self.assertEqual(fixture.role, 0)
        self.pump_until(
            lambda: self.entry.get_selection_bounds() == (),
            "GTK timeout cleanup collapses prepared selection",
        )
        self.assertEqual(self.keyboard_group(), 0)
        self.assertEqual(self.desktop_activation_count(), activations)
        self.assertEqual((self.entry.get_text(), self.entry.get_position()),
                         ("ghbdtn", 6))
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "0")
        self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD), clipboard)
        self.assertEqual(self.selection_owner("CLIPBOARD"), clipboard_owner)
        for key in (KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_LEFTALT, KEY_PAUSE):
            self.assertFalse(self.key_is_down(key))
        self.harness.send_key(KEY_SPACE)
        self.pump_until(
            lambda: self.entry.get_text() == "привет ",
            "safe timeout retries the original word after fresh input",
        )
        self.assertEqual(self.desktop_activation_count(), activations + 1)
        self.assertEqual(self.keyboard_group(), 1)
        self.assertEqual(self.entry.get_position(), 7)
        self.assertEqual(self.entry.get_selection_bounds(), ())
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "1")
        self.harness.type_word("hello")
        self.pump_until(lambda: self.entry.get_text() == "привет руддщ",
                        "GTK next source word follows the recovered RU layout")
        self.harness.send_key(KEY_PAUSE)
        self.pump_until(
            lambda: self.entry.get_text() == "привет hello",
            "GTK reverse correction recovers without manual layout compensation",
        )
        self.assertEqual(self.desktop_activation_count(), activations + 2)
        self.assertEqual(self.entry.get_selection_bounds(), ())
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "2")
        self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD), clipboard)
        self.assertEqual(self.selection_owner("CLIPBOARD"), clipboard_owner)
        for key in (KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_LEFTALT, KEY_PAUSE):
            self.assertFalse(self.key_is_down(key))
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertEqual(process_start_time(daemon_pid), daemon_start)
        self.assertIsNone(self.harness.process.poll())

    def assert_layout_cleanup_automatic_recovery(
            self, renew_primary: bool = False, timeout_primary: bool = False) -> None:
        self.prepare_word_editor()
        self.assertEqual(ipc_request(b"SET_STATUS 1\n"), b"OK ENABLED\n")
        daemon_pid = self.harness.process.pid
        daemon_start = process_start_time(daemon_pid)
        activations = self.desktop_activation_count()
        primary = Gtk.Clipboard.get(Gdk.SELECTION_PRIMARY)
        ownership = []
        if renew_primary:
            handler = primary.connect(
                "owner-change",
                lambda _clipboard, event: ownership.append(
                    (event.owner, event.selection_time)),
            )
            self.addCleanup(primary.disconnect, handler)
        phase = 1
        arm = pathlib.Path(f"/run/punto-e2e-arm-layout-cleanup-phase-{phase}")
        reached = pathlib.Path(
            f"/run/punto-e2e-layout-cleanup-phase-{phase}-reached")
        release = pathlib.Path(
            f"/run/punto-e2e-release-layout-cleanup-phase-{phase}")
        for marker in (arm, reached, release):
            marker.unlink(missing_ok=True)
            self.addCleanup(marker.unlink, missing_ok=True)
        receipt_arm = pathlib.Path("/run/punto-e2e-arm-cleanup-receipt-retained")
        receipt_retained = pathlib.Path("/run/punto-e2e-cleanup-receipt-retained")
        if renew_primary:
            for marker in (receipt_arm, receipt_retained):
                marker.unlink(missing_ok=True)
                self.addCleanup(marker.unlink, missing_ok=True)
            receipt_arm.touch(mode=0o600)
        arm.touch(mode=0o600)
        if self.real_layout_bridge is not None:
            self.addCleanup(self.real_layout_bridge.release_result_loss)
            self.real_layout_bridge.arm_result_loss()
        else:
            self.layout_shortcut.arm_blocked_activation()
            self.addCleanup(self.layout_shortcut.permit_activation.set)

        self.harness.type_word("ghbdtn")
        self.harness.send_key(KEY_SPACE)
        if self.real_layout_bridge is not None:
            fixture = self.real_layout_bridge
            self.pump_until(fixture.switch_committed.is_set,
                            "GTK automatic activation committed before reply loss")
            self.assertGreater(fixture.switch_committed_us, 0)
            self.assertLess(fixture.switch_committed_us, fixture.switch_deadline_us)
            self.assertEqual(self.desktop_activation_count(), activations + 1)
            self.assertEqual(self.desktop_source_role(), 1)
            self.assertEqual(self.keyboard_group(), 1)
            self.pump_until(fixture.operation_queried.is_set,
                            "matching GetOperation also has no available result")
        else:
            self.pump_until(self.layout_shortcut.activation_started.is_set,
                            "GTK automatic activation held past timeout")
        self.pump_until(reached.exists, "GTK automatic cleanup phase reached")
        if self.real_layout_bridge is not None:
            self.real_layout_bridge.release_result_loss()
        else:
            self.layout_shortcut.permit_activation.set()
        self.pump_until(
            lambda: self.desktop_activation_count() == activations + 1 and
            self.keyboard_group() == 1,
            "GTK automatic activation completed during cleanup",
        )
        release.touch(mode=0o600)
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn ",
                        "GTK failed automatic text remains collapsed")
        self.pump_until(
            lambda: self.entry.get_selection_bounds() == () and
            self.entry.get_position() == 7,
            "GTK failed automatic caret restored after source text",
        )
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "0")

        if renew_primary:
            self.pump_until(receipt_retained.exists,
                            "service-owned cleanup identity retained")
            owner = self.selection_owner()
            self.assertNotEqual(owner, 0)
            self.pump_until(
                lambda: any(window and window.get_xid() == owner
                            for window, _stamp in ownership),
                "real PRIMARY ownership event for the retained GTK window",
            )
            owner_window, old_stamp = next(
                (window, stamp) for window, stamp in reversed(ownership)
                if window and window.get_xid() == owner
            )
            self.assertIsNone(self.selection_text(Gdk.SELECTION_PRIMARY))
            self.assertTrue(Gdk.selection_owner_set_for_display(
                Gdk.Display.get_default(), owner_window, Gdk.SELECTION_PRIMARY,
                Gdk.CURRENT_TIME, False,
            ))
            Gdk.flush()
            self.pump_until(
                lambda: any(window and window.get_xid() == owner and
                            stamp != old_stamp for window, stamp in ownership),
                "same GTK connection renews actual server PRIMARY identity",
            )
            self.assertEqual(self.selection_owner(), owner)
            self.assertIsNone(self.selection_text(Gdk.SELECTION_PRIMARY))
            lefts = sum(phase == "press" and name == "Left"
                        for phase, name, _time in self.key_events)
            attempts = self.harness.diagnostic().count("Word edit dispatch status=")
            none_reads = self.harness.diagnostic().count(
                "Clipboard read failed selection=primary stage=selection_notify_rejected")
            self.harness.send_key(KEY_PAUSE)
            self.pump_until(
                lambda: self.harness.diagnostic().count(
                    "Word edit dispatch status=") > attempts,
                "stale Pause refuses before another native selection",
            )
            self.assertGreater(self.harness.diagnostic().count(
                "Clipboard read failed selection=primary stage=selection_notify_rejected"),
                none_reads, "the manager received an actual NONE reply, not a timeout")
            self.assertEqual(self.entry.get_text(), "ghbdtn ")
            self.assertEqual(self.entry.get_selection_bounds(), ())
            self.assertEqual(self.entry.get_position(), 7)
            self.assertEqual(sum(phase == "press" and name == "Left"
                                 for phase, name, _time in self.key_events), lefts)
            self.assertEqual(self.stats_fields()[1]["word_dispatches"], "0")

        fail_read = pathlib.Path("/run/punto-e2e-fail-next-primary-payload-read")
        failed_read = pathlib.Path("/run/punto-e2e-primary-payload-read-failed")
        if timeout_primary:
            for marker in (fail_read, failed_read):
                marker.unlink(missing_ok=True)
                self.addCleanup(marker.unlink, missing_ok=True)
            fail_read.touch(mode=0o600)
            attempts = self.harness.diagnostic().count("Word edit dispatch status=")
            timeouts = self.harness.diagnostic().count(
                "Clipboard read failed selection=primary stage=selection_notify_timeout")
            lefts = sum(phase == "press" and name == "Left"
                        for phase, name, _time in self.key_events)
        self.harness.type_word("hello")
        self.harness.send_key(KEY_SPACE)
        if timeout_primary:
            self.pump_until(
                lambda: self.harness.diagnostic().count(
                    "Word edit dispatch status=") > attempts,
                "unknown payload result refuses fresh recovery",
            )
            self.assertTrue(failed_read.exists())
            self.assertFalse(fail_read.exists())
            self.assertGreater(self.harness.diagnostic().count(
                "Clipboard read failed selection=primary stage=selection_notify_timeout"),
                timeouts)
            self.assertEqual(self.entry.get_text(), "ghbdtn руддщ ")
            self.assertEqual(sum(phase == "press" and name == "Left"
                                 for phase, name, _time in self.key_events), lefts)
            self.assertEqual(self.stats_fields()[1]["word_dispatches"], "0")
            self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD),
                             "startup clipboard baseline")
            self.assertEqual(self.harness.process.pid, daemon_pid)
            self.assertIsNone(self.harness.process.poll())
            for key in (KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_LEFTALT):
                self.assertFalse(self.key_is_down(key))
            return
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn hello ",
                        "GTK automatic recovery needs no manual toggle")
        self.assertEqual(self.desktop_activation_count(), activations + 2)
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "1")
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertEqual(process_start_time(daemon_pid), daemon_start)

    def test_layout_cleanup_race_recovers_next_automatic_word(self) -> None:
        self.assert_layout_cleanup_automatic_recovery()

    def test_layout_cleanup_same_owner_none_recovers_after_stale_pause(self) -> None:
        self.assert_layout_cleanup_automatic_recovery(renew_primary=True)
        self.assertEqual(self.keyboard_group(), 0)
        self.harness.type_word("f")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn hello f",
                        "next ordinary key uses the recovered English source")
        self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD),
                         "startup clipboard baseline")
        for key in (KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_LEFTALT):
            self.assertFalse(self.key_is_down(key))

    def test_layout_cleanup_primary_timeout_cannot_admit_fresh_range(self) -> None:
        self.assert_layout_cleanup_automatic_recovery(
            renew_primary=True, timeout_primary=True)

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
        self.set_desktop_layout(0)
        self.harness.send_key(KEY_PAUSE)
        self.pump_until(lambda: self.entry.get_text() == "руддщ", "fresh request recovery")
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "1")
        self.pump_until(
            lambda: "[punto] Word edit dispatch status=2\n"
            in self.harness.diagnostic(),
            "prepared recovery outcome is published",
        )
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

    def test_empty_word_manual_hotkey_is_observed_and_recovers(self) -> None:
        self.prepare_word_editor()
        self.set_selection(Gdk.SELECTION_CLIPBOARD, "empty-word sentinel")
        self.assertEqual(self.entry.get_text(), "")
        self.assertEqual(self.entry.get_selection_bounds(), ())
        self.assertEqual(self.keyboard_group(), 0)
        daemon_pid = self.harness.process.pid
        dispatch = "Word edit dispatch status="
        before = self.harness.diagnostic().count(dispatch)

        self.harness.send_key(KEY_PAUSE)
        self.pump_until(
            lambda: self.harness.diagnostic().count(dispatch) > before,
            "handled empty-word manual hotkey",
        )
        self.assertEqual(self.harness.diagnostic().count(dispatch), before + 1)
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "0")
        self.assertEqual(self.entry.get_text(), "")
        self.assertEqual(self.keyboard_group(), 0)
        self.assertEqual(self.output_count(KEY_PAUSE), 0)
        self.assertEqual(
            self.selection_text(Gdk.SELECTION_CLIPBOARD), "empty-word sentinel"
        )

        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn", "fresh word")
        self.harness.send_key(KEY_PAUSE)
        self.pump_until(
            lambda: self.entry.get_text() == "привет", "fresh manual correction"
        )
        self.assertEqual(self.keyboard_group(), 1)
        self.harness.type_word("f")
        self.pump_until(
            lambda: self.entry.get_text() == "привета", "next key after recovery"
        )
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertIsNone(self.harness.process.poll())
        self.assertEqual(self.entry.get_position(), 7)
        self.assertEqual(
            self.selection_text(Gdk.SELECTION_CLIPBOARD), "empty-word sentinel"
        )

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

    def test_word_layout_corrects_left_adjacent_duplicate_without_moving_caret(self) -> None:
        self.prepare_word_editor()
        self.entry.set_text("a suffix")
        self.entry.set_position(0)
        self.harness.type_word("a")
        self.pump_until(
            lambda: self.entry.get_text() == "aa suffix"
            and self.entry.get_position() == 1,
            "adjacent duplicate source before correction",
        )
        self.harness.send_key(KEY_PAUSE)
        self.pump_until(
            lambda: self.entry.get_text() == "фa suffix",
            "only the newly typed left duplicate corrected",
        )
        self.assertEqual(self.entry.get_position(), 1)
        self.assertEqual(self.entry.get_selection_bounds(), ())
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "1")

    def test_word_layout_rejects_desktop_disturbance_without_retargeting(self) -> None:
        self.prepare_word_editor()
        self.entry.set_text("a suffix")
        self.entry.set_position(0)
        self.harness.type_word("a")
        self.pump_until(
            lambda: self.entry.get_text() == "aa suffix"
            and self.entry.get_position() == 1,
            "adjacent duplicate before disturbed desktop transition",
        )
        self.layout_shortcut.selection_disturbance = True
        self.harness.send_key(KEY_PAUSE)
        self.pump_until(
            lambda: self.layout_shortcut.selection_disturbances == 1,
            "disturbed duplicate desktop transition",
        )
        self.pump_until(
            lambda: "rejection_stage=layout_cleanup_foreign_raw_key"
            in self.harness.diagnostic(),
            "raw-key rejection after desktop disturbance",
        )
        self.assertEqual(self.entry.get_text(), "aa suffix")
        self.assertEqual(self.entry.get_selection_bounds(), ())
        self.assertEqual(self.layout_shortcut.selection_disturbances, 1)
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "0")

    def assert_initial_xinput_failure_is_safe(self, stage: str) -> None:
        self.prepare_word_editor()
        activations = self.desktop_activation_count()
        daemon_pid = self.harness.process.pid
        daemon_start = process_start_time(daemon_pid)
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn", "source word")
        self.set_selection(Gdk.SELECTION_CLIPBOARD, "xinput clipboard sentinel")
        arm = pathlib.Path("/run", f"punto-e2e-fail-initial-xinput-{stage}")
        consumed = pathlib.Path(
            "/run", f"punto-e2e-initial-xinput-{stage}-fault-consumed"
        )
        unsafe = pathlib.Path(
            "/run/punto-e2e-unsafe-xcb-get-setup-after-xinput-failure"
        )
        for marker in (arm, consumed, unsafe):
            marker.unlink(missing_ok=True)
            self.addCleanup(marker.unlink, missing_ok=True)
        arm.touch(mode=0o600)
        started = time.monotonic()
        self.harness.send_key(KEY_PAUSE)
        self.pump_until(consumed.exists, f"initial XInput {stage} fault consumed")
        self.pump_until(
            lambda: "Word edit dispatch status=0 rejection_stage=xinput"
            in self.harness.diagnostic(),
            f"initial XInput {stage} rejection",
        )
        self.assertLess(time.monotonic() - started, 1.0)
        self.assertFalse(arm.exists())
        self.assertFalse(unsafe.exists())
        self.assertEqual(self.entry.get_text(), "ghbdtn")
        self.assertEqual(self.entry.get_position(), 6)
        self.assertEqual(self.entry.get_selection_bounds(), ())
        self.assertIsNone(self.selection_text(Gdk.SELECTION_PRIMARY))
        self.assertEqual(
            self.selection_text(Gdk.SELECTION_CLIPBOARD),
            "xinput clipboard sentinel",
        )
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "0")
        self.assertEqual(self.keyboard_group(), 0)
        self.assertEqual(self.desktop_source_role(), 0)
        self.assertEqual(self.desktop_activation_count(), activations)
        self.assertEqual(ipc_request(b"GET_STATUS\n"), b"OK ENABLED\n")
        self.harness.send_key(KEY_PAUSE)
        self.pump_until(
            lambda: self.entry.get_text() == "привет",
            f"healthy correction after initial XInput {stage} fault",
        )
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "1")
        self.assertEqual(self.desktop_activation_count(), activations + 1)
        self.harness.type_word("f")
        self.pump_until(lambda: self.entry.get_text() == "привета",
                        "next ordinary key follows the recovered Russian source")
        self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD),
                         "xinput clipboard sentinel")
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertEqual(process_start_time(daemon_pid), daemon_start)
        self.assertIsNone(self.harness.process.poll())
        for key in (KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_LEFTALT):
            self.assertFalse(self.key_is_down(key))

    def test_word_layout_rejects_initial_xinput_barrier_failure(self) -> None:
        self.assert_initial_xinput_failure_is_safe("barrier")

    def test_word_layout_rejects_initial_xinput_version_failure(self) -> None:
        self.assert_initial_xinput_failure_is_safe("version")

    def prepare_russian_textview_word(self):
        self.prepare_word_editor()
        self.assertEqual(ipc_request(b"SET_STATUS 0\n"), b"OK DISABLED\n")
        self.set_desktop_layout(1)
        self.window.remove(self.entry)
        editor = Gtk.TextView()
        self.window.add(editor)
        self.window.show_all()
        editor.grab_focus()
        self.pump_until(editor.has_focus, "Russian TextView focus")
        document = editor.get_buffer()
        self.harness.type_word("hello")
        self.harness.send_key(KEY_TAB)
        self.pump_until(
            lambda: document.get_text(*document.get_bounds(), True) == "руддщ\t",
            "actual Russian word and literal Tab require manual plaintext paste",
        )
        return document





    def assert_word_layout_rejects_incomplete_raw_key_history(
        self, arm_marker: str
    ) -> None:
        self.prepare_word_editor()
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn", "source word")
        markers = {
            name: pathlib.Path("/run", "punto-e2e-" + name)
            for name in (
                arm_marker,
                "overflow-raw-events-drained",
                "raw-observation-returned-before-drain",
            )
        }
        for marker in markers.values():
            marker.unlink(missing_ok=True)
            self.addCleanup(marker.unlink, missing_ok=True)
        markers[arm_marker].touch(mode=0o600)
        started = time.monotonic()
        self.harness.send_key(KEY_PAUSE)
        self.pump_until(
            lambda: any(
                stage in self.harness.diagnostic()
                for stage in (
                    "rejection_stage=layout_bridge_raw_key_preflight",
                    "rejection_stage=layout_transition_raw_key_incomplete",
                    "rejection_stage=layout_cleanup_foreign_raw_key",
                    "rejection_stage=layout_cleanup_raw_key_incomplete",
                )
            ),
            "bounded rejection of incomplete raw-key history",
        )
        self.assertLess(time.monotonic() - started, 1.0)
        self.assertTrue(
            markers["raw-observation-returned-before-drain"].exists()
        )
        self.assertEqual(self.entry.get_text(), "ghbdtn")
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "0")

    def test_word_layout_rejects_raw_key_count_overflow_boundedly(self) -> None:
        self.assert_word_layout_rejects_incomplete_raw_key_history(
            "arm-overflow-raw-events"
        )

    def test_word_layout_rejects_raw_key_time_overflow_boundedly(self) -> None:
        self.assert_word_layout_rejects_incomplete_raw_key_history(
            "arm-slow-raw-events"
        )

    def test_word_layout_rejects_raw_key_observation_failure(self) -> None:
        self.prepare_word_editor()
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn", "source word")
        marker = pathlib.Path(
            "/run/punto-e2e-arm-fail-raw-event-observation"
        )
        marker.unlink(missing_ok=True)
        marker.touch(mode=0o600)
        self.addCleanup(marker.unlink, missing_ok=True)
        self.harness.send_key(KEY_PAUSE)
        self.pump_until(
            lambda: any(
                stage in self.harness.diagnostic()
                for stage in (
                    "rejection_stage=layout_transition_raw_key_observation",
                    "rejection_stage=layout_cleanup_foreign_raw_key",
                )
            ),
            "raw-key observation failure rejects before replay",
        )
        self.assertFalse(marker.exists(), "raw-key fault injection was reached")
        self.assertEqual(self.entry.get_text(), "ghbdtn")
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "0")

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
        daemon_pid = self.harness.process.pid
        daemon_start = process_start_time(daemon_pid)
        activations = self.desktop_activation_count()
        clipboard = self.selection_text(Gdk.SELECTION_CLIPBOARD)
        clipboard_owner = self.selection_owner("CLIPBOARD")
        self.harness.type_word("ghbdtn")
        self.harness.send_key(KEY_DOT)
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn.", "punctuated word")
        self.harness.send_key(KEY_SPACE)
        self.pump_until(
            lambda: self.entry.get_text() == "привет. ", "punctuation preservation"
        )
        self.assertEqual(self.desktop_activation_count(), activations + 1)
        self.assertEqual((self.entry.get_position(), self.entry.get_selection_bounds()),
                         (8, ()))
        self.assertEqual(self.keyboard_group(), 1)
        self.assertEqual(self.desktop_source_role(), 1)
        self.wait_word_dispatches(1, "punctuation correction completed")
        self.harness.type_word("f")
        self.pump_until(lambda: self.entry.get_text() == "привет. а",
                        "next physical input after punctuation uses RU")
        self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD), clipboard)
        self.assertEqual(self.selection_owner("CLIPBOARD"), clipboard_owner)
        for key in (KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_LEFTALT, KEY_PAUSE):
            self.assertFalse(self.key_is_down(key))
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertEqual(process_start_time(daemon_pid), daemon_start)
        self.assertIsNone(self.harness.process.poll())

    def test_manual_word_case_uses_actual_content_not_stale_history(self) -> None:
        self.prepare_word_editor()
        self.harness.type_word("hello")
        self.pump_until(lambda: self.entry.get_text() == "hello", "original word")
        self.entry.set_text("other")
        self.entry.set_position(5)
        self.harness.hotkey(KEY_LEFTCTRL, repeat=False)
        self.pump_until(
            lambda: "Word edit dispatch status=" in self.harness.diagnostic(),
            "actual current-range edit decision",
        )
        self.assertEqual(self.entry.get_text(), "OTHER")
        self.assertEqual(self.entry.get_selection_bounds(), ())
        self.assertEqual(self.entry.get_position(), 5)
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "1")
        self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD), "startup clipboard baseline")
        self.harness.type_word("f")
        self.pump_until(lambda: self.entry.get_text() == "OTHERf", "next input follows the actual corrected word")

    def send_chord(self, modifiers: tuple[int, ...], key: int = KEY_PAUSE) -> None:
        events = []
        for modifier in modifiers:
            events.extend([(EV_KEY, modifier, 1), (EV_SYN, SYN_REPORT, 0)])
        events.extend([(EV_KEY, key, 1), (EV_SYN, SYN_REPORT, 0),
                       (EV_KEY, key, 0), (EV_SYN, SYN_REPORT, 0)])
        for modifier in reversed(modifiers):
            events.extend([(EV_KEY, modifier, 0), (EV_SYN, SYN_REPORT, 0)])
        self.harness.send_events(events)

    def assert_rejected_word_edit_preserves_newer_same_client_selection(
            self, stale_text: str, position: int,
            selection: tuple[int, int]) -> None:
        self.prepare_word_editor()
        self.harness.type_word("hello")
        self.pump_until(lambda: self.entry.get_text() == "hello", "tracked source word")
        self.entry.set_text(stale_text)
        self.entry.set_position(position)

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
                        self.entry.select_region(*selection)
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
        self.assertEqual(self.entry.get_text(), stale_text)
        self.assertEqual(self.entry.get_selection_bounds(), selection)
        self.assertFalse(any(name == "Right" for _, name, _ in self.key_events))

    def test_rejected_word_edit_preserves_newer_same_client_selection(self) -> None:
        self.assert_rejected_word_edit_preserves_newer_same_client_selection(
            "other", 5, (0, 2)
        )

    def test_rejected_word_edit_preserves_newer_same_extent_selection(self) -> None:
        self.assert_rejected_word_edit_preserves_newer_same_client_selection(
            "otherx", 6, (0, 5)
        )

    def assert_queued_selection_failure_collapses_and_recovers(
            self, marker_name: str) -> None:
        self.prepare_word_editor()
        marker = pathlib.Path("/run", marker_name)
        marker.unlink(missing_ok=True)
        marker.touch(mode=0o600)
        self.addCleanup(marker.unlink, missing_ok=True)
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn",
                        "source before queued selection failure")
        diagnostic_before = self.harness.diagnostic()
        diagnostic_start = len(diagnostic_before)
        statuses = diagnostic_before.count("Word edit dispatch status=1")
        self.send_chord(())
        self.pump_until(
            lambda: self.harness.diagnostic().count(
                "Word edit dispatch status=1") > statuses,
            "queued selection failure completed",
        )
        diagnostic = self.harness.diagnostic()[diagnostic_start:]
        refusals = [line for line in diagnostic.splitlines()
                    if line.startswith("[punto] Selection preparation refused ")]
        self.assertEqual(len(refusals), 1, "one fresh selection preparation refusal")
        self.assertRegex(
            refusals[0],
            r"^\[punto\] Selection preparation refused phase=tap stroke=[0-9]+ "
            r"context_failure=[0-9]+ prepared=1 cancelled=[01] connection_open=[01] "
            r"elapsed_us=[0-9]+ remaining_us=-?[0-9]+$",
        )
        self.assertLess(diagnostic.index(refusals[0]),
                        diagnostic.index("Word edit dispatch status=1"))
        self.assertNotIn("ghbdtn", diagnostic, "diagnostics do not expose source text")
        self.pump_until(lambda: self.entry.get_selection_bounds() == (),
                        "queued partial selection collapsed")
        self.assertFalse(marker.exists(), "fault was consumed in the actual selection phase")
        self.assertEqual(self.entry.get_text(), "ghbdtn")
        self.harness.send_key(KEY_SPACE)
        self.harness.type_word("hello")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn hello",
                        "fresh input appends after queued selection failure")
        self.send_chord(())
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn руддщ",
                        "correction recovers after queued selection failure")

    def test_first_queued_selection_failure_collapses_and_recovers(self) -> None:
        self.assert_queued_selection_failure_collapses_and_recovers(
            "punto-e2e-fail-first-selection-stroke-request"
        )

    def test_middle_queued_selection_failure_collapses_and_recovers(self) -> None:
        self.assert_queued_selection_failure_collapses_and_recovers(
            "punto-e2e-fail-middle-selection-stroke-request"
        )

    def test_preexisting_same_client_selection_is_not_cleanup_authority(self) -> None:
        self.prepare_word_editor()
        self.harness.type_word("hello")
        self.pump_until(lambda: self.entry.get_text() == "hello",
                        "tracked word before selection baseline")
        self.entry.select_region(0, 2)
        self.pump_until(lambda: self.entry.get_selection_bounds() == (0, 2),
                        "preexisting same-client selection")
        statuses = self.harness.diagnostic().count(
            "Word edit dispatch status=0"
        )
        self.send_chord((KEY_LEFTCTRL,))
        self.pump_until(
            lambda: self.harness.diagnostic().count(
                "Word edit dispatch status=0") > statuses,
            "baseline-only correction rejected",
        )
        self.assertEqual(self.entry.get_text(), "hello")
        self.assertEqual(self.entry.get_selection_bounds(), (0, 2))
        self.assertFalse(any(name == "Right" for _, name, _ in self.key_events))

    def assert_selection_correction(self, source: str, replacement: str,
                                    modifiers: tuple[int, ...]) -> None:
        self.prepare_word_editor()
        clipboard_owner = self.selection_owner("CLIPBOARD")
        pastes = []
        handler = self.entry.connect("paste-clipboard", lambda _editor: pastes.append(True))
        self.addCleanup(self.entry.disconnect, handler)
        original = "before " + source + " after"
        self.entry.set_text(original)
        self.entry.select_region(7, 7 + len(source))
        self.pump_until(lambda: self.selection_text(Gdk.SELECTION_PRIMARY) == source,
                        "selected source text")
        self.send_chord(modifiers)
        self.pump_until(lambda: self.entry.get_text() == "before " + replacement + " after",
                        "selection transformation changes only selected range")
        self.assertEqual(self.entry.get_selection_bounds(), ())
        self.assertEqual(self.entry.get_position(), 7 + len(replacement))
        self.assertEqual(self.output_count(KEY_PAUSE), 0)
        self.assertFalse(pastes, "representable selection uses no paste chord")
        self.assertEqual(self.selection_owner("CLIPBOARD"), clipboard_owner)
        for key in (KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_LEFTALT):
            self.assertFalse(self.key_is_down(key))
        self.pump_until(lambda: self.selection_text(Gdk.SELECTION_CLIPBOARD) == "startup clipboard baseline",
                        "clipboard preserved by direct selection replay")

    def test_selection_layout_hotkey_changes_real_editor_text(self) -> None:
        paste_presses = sum(
            phase == "press" and name in {"v", "V"}
            for phase, name, _ in self.key_events
        )
        self.assert_selection_correction("ghbdtn", "привет", (KEY_LEFTSHIFT,))
        self.assertEqual(
            sum(
                phase == "press" and name in {"v", "V"}
                for phase, name, _ in self.key_events
            ),
            paste_presses,
        )
        self.harness.type_word("f")
        self.pump_until(lambda: self.entry.get_text() == "before привета after",
                        "next typed symbol uses the selected target layout")

    def test_selection_hotkeys_without_visible_selection_preserve_receipt(self) -> None:
        self.prepare_word_editor()
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn", "physical source word")
        self.harness.send_key(KEY_PAUSE)
        self.pump_until(lambda: self.entry.get_text() == "привет", "first word conversion")
        self.wait_word_dispatches(1, "first word dispatched")
        clipboard_owner = self.selection_owner("CLIPBOARD")
        daemon_pid = self.harness.process.pid
        for modifiers in ((KEY_LEFTSHIFT,), (KEY_LEFTALT,),
                          (KEY_LEFTCTRL, KEY_LEFTALT)):
            with self.subTest(modifiers=modifiers):
                rejected = self.harness.diagnostic().count("rejection_stage=selection_range_empty")
                self.send_chord(modifiers)
                self.pump_until(
                    lambda: self.harness.diagnostic().count(
                        "rejection_stage=selection_range_empty") > rejected,
                    "selection-only operation requires a visible nonempty range",
                )
                self.assertEqual(self.entry.get_text(), "привет")
                self.assertEqual(self.entry.get_selection_bounds(), ())
                self.assertEqual(self.entry.get_position(), 6)
                self.assertEqual(self.stats_fields()[1]["word_dispatches"], "1")
                self.assertEqual(self.keyboard_group(), 1)
                self.assertEqual(self.selection_owner("CLIPBOARD"), clipboard_owner)
                for key in (*modifiers, KEY_PAUSE):
                    self.assertFalse(self.key_is_down(key))
        self.harness.send_key(KEY_PAUSE)
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn",
                        "ordinary word reversal survives selection refusal")
        self.wait_word_dispatches(2, "word reversal dispatched")
        self.entry.select_region(0, 6)
        self.pump_until(lambda: self.selection_text(Gdk.SELECTION_PRIMARY) == "ghbdtn",
                        "fresh real selection after refusal and word recovery")
        self.send_chord((KEY_LEFTSHIFT,))
        self.pump_until(lambda: self.entry.get_text() == "привет",
                        "fresh selection succeeds without restart")
        self.wait_word_dispatches(3, "fresh selection dispatched")
        self.harness.type_word("f")
        self.pump_until(lambda: self.entry.get_text() == "привета",
                        "next ordinary symbol follows selected target layout")
        self.assertEqual(self.selection_owner("CLIPBOARD"), clipboard_owner)
        self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD), "startup clipboard baseline")
        self.assertEqual(self.output_count(KEY_PAUSE), 0)
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertIsNone(self.harness.process.poll())

    def test_selection_paste_accepts_maximum_utf8_source_range(self) -> None:
        self.prepare_word_editor()
        source = "ф" * 2046 + "🙂"
        replacement = "a" * 2046 + "🙂"
        self.assertEqual(len(source.encode("utf-8")), 4096)
        self.entry.set_text("before " + source + " after")
        self.entry.select_region(7, 7 + len(source))
        self.pump_until(lambda: self.selection_text(Gdk.SELECTION_PRIMARY) == source,
                        "maximum-byte Unicode selection exceeds old proof limits")
        pastes = []
        handler = self.entry.connect("paste-clipboard", lambda _editor: pastes.append(True))
        self.addCleanup(self.entry.disconnect, handler)
        self.send_chord((KEY_LEFTSHIFT,))
        self.pump_until(lambda: self.entry.get_text() == "before " + replacement + " after",
                        "maximum-byte selected source uses guarded Unicode paste")
        self.wait_word_dispatches(1, "large selected operation dispatched")
        self.assertEqual(len(pastes), 1)
        self.assertEqual(self.entry.get_selection_bounds(), ())
        self.assertEqual(self.entry.get_position(), 7 + len(replacement))
        self.pump_until(lambda: self.selection_text(Gdk.SELECTION_CLIPBOARD)
                        == "startup clipboard baseline", "large selection restores clipboard")
        self.harness.type_word("f")
        self.pump_until(lambda: self.entry.get_text() == "before " + replacement + "f after",
                        "next physical key follows the large selection's target source")
        self.assertEqual(self.keyboard_group(), 0)
        self.assertEqual(self.output_count(KEY_PAUSE), 0)
        for key in (KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_LEFTALT):
            self.assertFalse(self.key_is_down(key))

        oversized = source + "x"
        self.assertEqual(len(oversized.encode("utf-8")), 4097)
        self.entry.set_text(oversized)
        self.entry.select_region(0, len(oversized))
        self.pump_until(lambda: self.selection_text(Gdk.SELECTION_PRIMARY) == oversized,
                        "one byte beyond the selection proof budget")
        rejected = self.harness.diagnostic().count("Word edit dispatch status=0")
        self.send_chord((KEY_LEFTSHIFT,))
        self.pump_until(lambda: self.harness.diagnostic().count(
            "Word edit dispatch status=0") > rejected, "over-limit selection refuses")
        self.assertEqual(self.entry.get_text(), oversized)
        self.assertEqual(self.entry.get_selection_bounds(), (0, len(oversized)))
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "1")
        self.assertEqual(len(pastes), 1)
        self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD), "startup clipboard baseline")
        self.entry.set_text("ghbdtn")
        self.entry.select_region(0, 6)
        self.pump_until(lambda: self.selection_text(Gdk.SELECTION_PRIMARY) == "ghbdtn",
                        "fresh valid selection after over-limit refusal")
        self.send_chord((KEY_LEFTSHIFT,))
        self.pump_until(lambda: self.entry.get_text() == "привет", "post-limit recovery")
        self.wait_word_dispatches(2, "post-limit recovery dispatched")

    def assert_selection_uses_reasserting_desktop_owner(self, source_group: int) -> None:
        self.prepare_word_editor()
        self.set_desktop_layout(source_group)
        self.enable_desktop_reassertion()
        activations = self.desktop_activation_count()
        source, replacement = (("ghbdtn", "привет") if source_group == 0
                               else ("руддщ", "hello"))
        self.entry.set_text(source + " suffix")
        self.entry.select_region(0, len(source))
        self.pump_until(lambda: self.selection_text(Gdk.SELECTION_PRIMARY) == source,
                        "exact subset under independently owned desktop source")
        clipboard_owner = self.selection_owner("CLIPBOARD")
        daemon_pid = self.harness.process.pid
        daemon_start = process_start_time(daemon_pid)
        self.send_chord((KEY_LEFTSHIFT,))
        self.pump_until(lambda: self.entry.get_text() == replacement + " suffix",
                        "subset correction survives desktop source reassertion")
        self.wait_word_dispatches(1, "selection completed under desktop authority")
        self.assertEqual(self.entry.get_selection_bounds(), ())
        self.assertEqual(self.entry.get_position(), len(replacement))
        self.assertEqual(self.desktop_source_role(), 1 - source_group)
        self.assertEqual(self.keyboard_group(), 1 - source_group)
        self.assertEqual(self.desktop_activation_count(), activations + 1)
        self.assertEqual(self.desktop_owner().source_reassertions, 0,
                         "product must not write an independently owned raw group")
        self.harness.type_word("a")
        suffix = "ф" if source_group == 0 else "a"
        self.pump_until(lambda: self.entry.get_text() == replacement + suffix + " suffix",
                        "next physical key follows the published target source")
        self.assertEqual(self.selection_owner("CLIPBOARD"), clipboard_owner)
        self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD),
                         "startup clipboard baseline")
        for key in (KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_LEFTALT):
            self.assertFalse(self.key_is_down(key))
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertEqual(process_start_time(daemon_pid), daemon_start)

    def test_selection_ru_to_en_uses_reasserting_desktop_owner(self) -> None:
        self.assert_selection_uses_reasserting_desktop_owner(1)

    def test_selection_en_to_ru_uses_reasserting_desktop_owner(self) -> None:
        self.assert_selection_uses_reasserting_desktop_owner(0)

    def test_native_desktop_source_reasserts_unpublished_raw_group(self) -> None:
        self.prepare_word_editor()
        self.set_desktop_layout(0)
        activations = self.desktop_activation_count()
        self.enable_desktop_reassertion()
        self.lock_keyboard_group(1)
        self.pump_until(lambda: self.desktop_owner().source_reassertions > 0,
                        "independent native source rejects an unpublished raw group")
        self.assertEqual(self.desktop_source_role(), 0)
        self.assertEqual(self.keyboard_group(), 0)
        self.assertEqual(self.desktop_activation_count(), activations)

    def test_plain_paste_accelerator_keeps_ru_layout(self) -> None:
        self.prepare_word_editor()
        self.set_desktop_layout(1)
        self.layout_shortcut.enabled = False
        self.layout_shortcut.reassert_source = True
        self.entry.set_text("source suffix")
        self.entry.select_region(0, 6)
        self.set_selection(Gdk.SELECTION_CLIPBOARD, "replacement")
        self.send_chord((KEY_LEFTCTRL,), KEY_V)
        self.pump_until(lambda: self.entry.get_text() == "replacement suffix",
                        "physical Ctrl+V resolves GTK accelerator in RU")
        self.assertEqual(self.desktop_source_role(), 1)
        self.assertEqual(self.keyboard_group(), 1)
        self.assertEqual(self.layout_shortcut.activations, 0)
        self.assertEqual(self.layout_shortcut.source_reassertions, 0)
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "0")
        self.assertFalse(self.key_is_down(KEY_LEFTCTRL))

    def test_mixed_word_bridge_hop_never_writes_into_another_child(self) -> None:
        self.prepare_word_editor()
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        self.window.remove(self.entry)
        other = Gtk.Entry(text="destination sentinel")
        box.pack_start(self.entry, True, True, 0)
        box.pack_start(other, True, True, 0)
        self.window.add(box)
        self.window.show_all()
        self.entry.grab_focus()
        self.entry.set_text("helloпривет")
        self.entry.set_position(-1)
        activations = self.desktop_activation_count()
        self.desktop_owner().before_activation = other.grab_focus
        self.send_chord((KEY_LEFTCTRL,))
        self.pump_until(lambda: self.desktop_activation_count() == activations + 1,
                        "mixed replay awaits its intermediate desktop hop")
        self.pump_until(lambda: "Word edit dispatch status=3" in self.harness.diagnostic(),
                        "lost child authority stops already-started replay")
        self.assertEqual(other.get_text(), "destination sentinel")
        self.assertEqual(self.entry.get_text(), "HELLO")
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "0")
        self.assertEqual(self.desktop_source_role(), 1)
        self.assertEqual(self.keyboard_group(), 1)
        self.assertFalse(self.key_is_down(KEY_LEFTCTRL))
        self.assertIsNone(self.harness.process.poll())

    def assert_selection_change_before_first_write_refuses_and_recovers(
            self, boundary: str, paste: bool = True, word: bool = False) -> None:
        self.prepare_word_editor()
        if boundary in ("after-replay-layout", "before-paste-publication"):
            self.set_desktop_layout(1)
        status = 1 if word else (0 if boundary in ("after-replay-layout", "before-paste-publication") else 1)
        stage = "word_source_before_replay" if word else (
            ("paste_source_before_publication" if paste else "selection_source_primary_identity")
            if status == 0 else ("paste_context_before_dispatch" if boundary == "after-paste-receipt"
                                 else "paste_submission"))
        markers = {
            name: pathlib.Path("/run", f"punto-e2e-{name}-{boundary}")
            for name in ("arm", "release")
        }
        markers["reached"] = pathlib.Path("/run", f"punto-e2e-{boundary}")
        markers["no_expiry"] = pathlib.Path("/run/punto-e2e-paste-chord-without-expiry")
        for marker in markers.values():
            marker.unlink(missing_ok=True)
            self.addCleanup(marker.unlink, missing_ok=True)
        self.addCleanup(markers["release"].touch,
                        mode=0o600, exist_ok=True)
        source = "ghbdtn🙂" if paste else ("AbC" if word else "руддщ")
        self.entry.set_text(f"before {source} after")
        if word:
            self.entry.set_position(7 + len(source))
        else:
            self.entry.select_region(7, 7 + len(source))
            self.pump_until(
                lambda: self.selection_text(Gdk.SELECTION_PRIMARY) == source,
                "operation A selection",
            )
        refused = self.harness.diagnostic().count(f"Word edit dispatch status={status}")
        clipboard_owner = self.selection_owner("CLIPBOARD")
        pastes = []
        handler = self.entry.connect("paste-clipboard", lambda _editor: pastes.append(True))
        self.addCleanup(self.entry.disconnect, handler)
        paste_presses = sum(phase == "press" and name in {"v", "V"}
                            for phase, name, _ in self.key_events)
        daemon_pid = self.harness.process.pid
        if boundary == "before-paste-chord":
            markers["no_expiry"].touch(mode=0o600)
        markers["arm"].touch(mode=0o600)
        modifier = KEY_LEFTCTRL if word else KEY_LEFTSHIFT
        self.send_chord((modifier,))
        self.pump_until(
            markers["reached"].exists,
            "operation A reaches the real pre-mutation boundary",
        )
        self.assertFalse(markers["arm"].exists())
        self.assertEqual(self.entry.get_text(), f"before {source} after")
        if boundary == "after-replay-layout":
            self.assertEqual(self.selection_owner("CLIPBOARD"), clipboard_owner,
                             "pre-publication refusal must not replace the external owner")
        self.entry.set_text("field B source")
        self.entry.select_region(0, 7)
        self.pump_until(lambda: self.selection_text(Gdk.SELECTION_PRIMARY) == "field B",
                        "actual foreign PRIMARY range before dispatch resumes")
        markers["release"].touch(mode=0o600)
        self.pump_until(
            lambda: self.harness.diagnostic().count(
                f"Word edit dispatch status={status}") > refused,
            "operation A safely refuses before dispatch in changed context",
        )
        self.assertIn(f"rejection_stage={stage}", self.harness.diagnostic())
        if not paste and not word:
            self.assertIn(
                "selection admission stage=selection_source_primary_identity "
                "context_result=0 xkb_result=0 group=0 expected_group=0",
                self.harness.diagnostic(),
            )
        self.assertFalse(markers["no_expiry"].exists())
        self.assertFalse(pastes)
        self.assertEqual(sum(phase == "press" and name in {"v", "V"}
                             for phase, name, _ in self.key_events), paste_presses)
        self.assertEqual(self.entry.get_text(), "field B source")
        self.assertEqual(self.entry.get_selection_bounds(), (0, 7))
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "0")
        self.pump_until(lambda: self.selection_text(Gdk.SELECTION_CLIPBOARD)
                        == "startup clipboard baseline", "unsent operation restores baseline")
        if boundary == "after-replay-layout":
            self.assertEqual(self.selection_owner("CLIPBOARD"), clipboard_owner)

        recovery = "привет🙂" if paste else "привет"
        replacement = "ghbdtn🙂" if paste else "ghbdtn"
        self.entry.set_text(f"before {recovery} after")
        self.entry.select_region(7, 7 + len(recovery))
        self.pump_until(
            lambda: self.selection_text(Gdk.SELECTION_PRIMARY) == recovery,
            "fresh recovery selection",
        )
        self.send_chord((KEY_LEFTSHIFT,))
        self.pump_until(
            lambda: self.entry.get_text() == f"before {replacement} after",
            "fresh operation after stale rejection",
        )
        self.assertEqual(self.entry.get_selection_bounds(), ())
        self.assertEqual(self.entry.get_position(), 7 + len(replacement))
        self.assertEqual(self.keyboard_group(), 0)
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "1")
        self.harness.type_word("f")
        self.pump_until(lambda: self.entry.get_text() == f"before {replacement}f after", "recovery next key uses EN")
        self.pump_until(lambda: self.selection_text(Gdk.SELECTION_CLIPBOARD)
                        == "startup clipboard baseline", "fresh paste restores baseline")
        for key in (KEY_LEFTCTRL, KEY_LEFTSHIFT):
            self.assertFalse(self.key_is_down(key))
        self.assertFalse(self.key_is_down(KEY_LEFTALT))
        self.assertEqual(self.output_count(KEY_PAUSE), 0)
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertIsNone(self.harness.process.poll())

    def test_selection_change_before_paste_publication_refuses_and_recovers(self) -> None:
        self.assert_selection_change_before_first_write_refuses_and_recovers("before-paste-publication")

    def test_selection_context_change_before_paste_refuses_and_recovers(self) -> None:
        self.assert_selection_change_before_first_write_refuses_and_recovers("after-paste-receipt")

    def test_selection_change_before_paste_chord_refuses_and_recovers(self) -> None:
        self.assert_selection_change_before_first_write_refuses_and_recovers("before-paste-chord")

    def test_selection_change_before_direct_replay_refuses_and_recovers(self) -> None:
        self.assert_selection_change_before_first_write_refuses_and_recovers("after-replay-layout", paste=False)

    def test_selection_change_before_word_replay_refuses_and_recovers(self) -> None:
        self.assert_selection_change_before_first_write_refuses_and_recovers(
            "after-replay-layout", paste=False, word=True)

    def test_selection_layout_stall_refuses_before_replay(self) -> None:
        self.prepare_word_editor()
        self.entry.set_text("ghbdtn")
        self.entry.select_region(0, 6)
        self.pump_until(
            lambda: self.selection_text(Gdk.SELECTION_PRIMARY) == "ghbdtn",
            "selected layout source before stalled desktop shortcut",
        )
        self.layout_shortcut.enabled = False
        rejected = self.harness.diagnostic().count("Word edit dispatch status=0")
        self.send_chord((KEY_LEFTSHIFT,))
        self.pump_until(
            lambda: self.harness.diagnostic().count(
                "Word edit dispatch status=0") > rejected,
            "selection layout final transition failure",
        )
        self.assertEqual(self.entry.get_text(), "ghbdtn")
        self.assertEqual(self.entry.get_selection_bounds(), (0, 6))
        self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD),
                         "startup clipboard baseline")
        self.assertEqual(self.layout_shortcut.activations, 0)

    def test_selection_paste_same_layout_does_not_require_desktop_toggle(self) -> None:
        self.prepare_word_editor()
        self.set_desktop_layout(1)
        self.entry.set_text("AbC")
        self.entry.select_region(0, 3)
        self.pump_until(lambda: self.selection_text(Gdk.SELECTION_PRIMARY) == "AbC",
                        "selected case source in Russian layout")
        self.layout_shortcut.enabled = False
        self.send_chord((KEY_LEFTALT,))
        self.pump_until(lambda: self.entry.get_text() == "aBc",
                        "same-layout selection paste without desktop toggle")
        self.assertEqual(self.entry.get_selection_bounds(), ())
        self.pump_for(0.35)
        self.pump_until(
            lambda: self.selection_text(Gdk.SELECTION_CLIPBOARD)
            == "startup clipboard baseline",
            "same-layout selection restores clipboard after quiet grace",
        )
        self.assertEqual(self.layout_shortcut.activations, 0)
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "1")

    def real_layout_bridge_selection_tolerates_slow_desktop_transitions(self) -> None:
        self.prepare_word_editor()
        daemon_pid = self.harness.process.pid
        self.entry.set_text("ghbdtn")
        self.entry.select_region(0, 6)
        self.pump_until(
            lambda: self.selection_text(Gdk.SELECTION_PRIMARY) == "ghbdtn",
            "selected source before delayed layout preflight",
        )
        activations = self.desktop_activation_count()
        self.assertIsNotNone(self.real_layout_bridge)
        self.real_layout_bridge.before_activation = lambda: time.sleep(0.25)
        self.addCleanup(setattr, self.real_layout_bridge, "before_activation", None)
        self.send_chord((KEY_LEFTSHIFT,))
        self.pump_until(lambda: self.entry.get_text() == "привет",
                        "selection corrected after slow desktop transitions")
        self.pump_until(lambda: self.desktop_activation_count() == activations + 1,
                        "slow selection final desktop activation")
        self.pump_until(lambda: self.stats_fields()[1]["word_dispatches"] == "1",
                        "slow selection final layout dispatch")
        self.assertEqual(self.entry.get_selection_bounds(), ())
        self.assertEqual(self.entry.get_position(), 6)
        self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD),
                         "startup clipboard baseline")
        self.assertEqual(self.desktop_activation_count(), activations + 1)
        self.assertEqual(self.keyboard_group(), 1)
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "1")
        self.harness.type_word("f")
        self.pump_until(lambda: self.entry.get_text() == "привета",
                        "next key uses RU after slow desktop transition")
        self.assertEqual(self.entry.get_position(), 7)
        self.assertEqual(self.entry.get_selection_bounds(), ())
        self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD),
                         "startup clipboard baseline")
        self.assertEqual(self.desktop_activation_count(), activations + 1)
        for key in (KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_LEFTALT):
            self.assertFalse(self.key_is_down(key))
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertIsNone(self.harness.process.poll())

    def test_selection_layout_legacy_grab_disturbance_refuses(self) -> None:
        self.prepare_word_editor()
        self.entry.set_text("ghbdtn")
        self.entry.select_region(0, 6)
        self.pump_until(
            lambda: self.selection_text(Gdk.SELECTION_PRIMARY) == "ghbdtn",
            "selected source before desktop selection disturbance",
        )
        self.layout_shortcut.selection_disturbance = True
        daemon_pid = self.harness.process.pid
        daemon_start = process_start_time(daemon_pid)
        self.send_chord((KEY_LEFTSHIFT,))
        self.pump_until(lambda: self.layout_shortcut.selection_disturbances == 1,
                        "desktop transition disturbs original selection before replay")
        self.pump_until(lambda: "Word edit dispatch status=0" in self.harness.diagnostic(),
                        "changed selection refuses before the first replacement")
        self.assertEqual(self.entry.get_text(), "ghbdtn")
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "0")
        self.assertEqual(self.layout_shortcut.selection_disturbances, 1)
        self.assertEqual(self.desktop_source_role(), 1)
        self.assertEqual(self.keyboard_group(), 1)
        self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD),
                         "startup clipboard baseline")
        self.layout_shortcut.selection_disturbance = False
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertEqual(process_start_time(daemon_pid), daemon_start)
        for key in (KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_LEFTALT):
            self.assertFalse(self.key_is_down(key))

    def real_layout_bridge_selection_disturbance_refuses_and_recovers(self) -> None:
        self.prepare_word_editor()
        self.entry.set_text("ghbdtn suffix")
        self.entry.select_region(0, 6)
        self.pump_until(lambda: self.selection_text(Gdk.SELECTION_PRIMARY) == "ghbdtn",
                        "original range before native bridge callback")
        activations = self.desktop_activation_count()
        daemon_pid = self.harness.process.pid
        daemon_start = process_start_time(daemon_pid)

        def disturb_range():
            self.entry.select_region(1, 6)

        self.real_layout_bridge.before_activation = disturb_range
        self.send_chord((KEY_LEFTSHIFT,))
        self.pump_until(lambda: "Word edit dispatch status=0" in self.harness.diagnostic(),
                        "native bridge range change refuses before replacement")
        self.assertEqual(self.entry.get_text(), "ghbdtn suffix")
        self.assertEqual(self.entry.get_selection_bounds(), (1, 6))
        self.assertEqual(self.selection_text(Gdk.SELECTION_PRIMARY), "hbdtn")
        self.assertEqual(self.desktop_activation_count(), activations + 1)
        self.assertEqual(self.desktop_source_role(), 1)
        self.assertEqual(self.keyboard_group(), 1)
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "0")
        self.real_layout_bridge.before_activation = None
        self.entry.set_text("руддщ suffix")
        self.entry.select_region(0, 5)
        self.pump_until(lambda: self.selection_text(Gdk.SELECTION_PRIMARY) == "руддщ",
                        "fresh subset after bridge disturbance refusal")
        self.send_chord((KEY_LEFTSHIFT,))
        self.pump_until(lambda: self.entry.get_text() == "hello suffix",
                        "fresh selection recovers on the same daemon")
        self.wait_word_dispatches(1, "fresh selection dispatch after refusal")
        self.assertEqual(self.desktop_activation_count(), activations + 2)
        self.assertEqual(self.desktop_source_role(), 0)
        self.assertEqual(self.keyboard_group(), 0)
        self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD),
                         "startup clipboard baseline")
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertEqual(process_start_time(daemon_pid), daemon_start)
        for key in (KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_LEFTALT):
            self.assertFalse(self.key_is_down(key))


    def test_selection_layout_never_retargets_adjacent_duplicate(self) -> None:
        self.prepare_word_editor()
        self.entry.set_text("aa suffix")
        self.entry.select_region(0, 1)
        self.pump_until(
            lambda: self.selection_text(Gdk.SELECTION_PRIMARY) == "a",
            "left duplicate selected",
        )
        self.layout_shortcut.selection_disturbance = True
        self.send_chord((KEY_LEFTSHIFT,))
        self.pump_until(
            lambda: "Word edit dispatch status=0" in self.harness.diagnostic(),
            "selection correction kept the original duplicate range",
        )
        self.assertEqual(self.entry.get_position(), 1)
        self.assertEqual(self.entry.get_selection_bounds(), ())
        self.assertEqual(self.entry.get_text(), "aa suffix")
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "0")

    def test_selection_layout_activation_limit_refuses_before_replay(self) -> None:
        self.prepare_word_editor()
        self.entry.set_text("ghbdtn")
        self.entry.select_region(0, 6)
        self.pump_until(
            lambda: self.selection_text(Gdk.SELECTION_PRIMARY) == "ghbdtn",
            "selected source before post-paste layout failure",
        )
        self.layout_shortcut.activation_limit = 0
        self.send_chord((KEY_LEFTSHIFT,))
        self.pump_until(
            lambda: "Word edit dispatch status=0" in self.harness.diagnostic(),
            "honest selection post-replay partial failure",
        )
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn",
                        "replay applied before final layout failure")
        self.assertEqual(self.keyboard_group(), 0)
        self.assertEqual(self.layout_shortcut.activations, 0)
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "0")

    def test_selection_case_hotkey_changes_real_editor_text(self) -> None:
        self.assert_selection_correction("sEleCt", "SeLEcT", (KEY_LEFTALT,))
        self.harness.type_word("f")
        self.pump_until(lambda: self.entry.get_text() == "before SeLEcTf after",
                        "case correction leaves caret after replacement")

    def test_selection_case_paste_expiry_preserves_published_source(self) -> None:
        self.prepare_plain_case_paste_from_russian()
        marker = pathlib.Path("/run/punto-e2e-expire-before-paste-publication")
        marker.unlink(missing_ok=True)
        marker.touch(mode=0o600)
        self.addCleanup(marker.unlink, missing_ok=True)
        rejected = self.harness.diagnostic().count("Word edit dispatch status=0")
        self.send_chord((KEY_LEFTALT,))
        self.pump_until(
            lambda: self.harness.diagnostic().count(
                "Word edit dispatch status=0"
            ) > rejected,
            "paste publication deadline rejection",
        )
        self.assertFalse(marker.exists())
        self.assertEqual(self.entry.get_text(), PASTE_CASE_ORIGINAL)
        self.assertEqual(
            self.selection_text(Gdk.SELECTION_CLIPBOARD), "plain clipboard"
        )
        self.assertEqual(self.desktop_source_role(), 1)
        self.assertEqual(self.keyboard_group(), 1, self.harness.diagnostic())
        self.entry.set_position(len(self.entry.get_text()))
        self.harness.type_word("a")
        self.pump_until(
            lambda: self.entry.get_text() == PASTE_CASE_ORIGINAL + "ф",
            "next physical key uses unchanged Russian source",
        )

    def prepare_plain_case_paste_from_russian(self) -> None:
        self.prepare_selected_case_paste()
        self.set_desktop_layout(1)
        self.set_selection(Gdk.SELECTION_CLIPBOARD, "plain clipboard")

    def prepare_selected_case_paste(self) -> None:
        self.prepare_word_editor()
        self.entry.set_text(PASTE_CASE_ORIGINAL)
        self.entry.select_region(*PASTE_CASE_BOUNDS)
        self.pump_until(
            lambda: self.selection_text(Gdk.SELECTION_PRIMARY) == PASTE_CASE_SOURCE,
            "actual unrepresentable selection requires plaintext paste",
        )

    def paste_publication_hold_markers(self) -> dict[str, pathlib.Path]:
        markers = {
            name: pathlib.Path("/run", "punto-e2e-" + name)
            for name in (
                "arm-before-paste-publication",
                "before-paste-publication",
                "release-before-paste-publication",
            )
        }
        for marker in markers.values():
            marker.unlink(missing_ok=True)
            self.addCleanup(marker.unlink, missing_ok=True)
        self.addCleanup(
            markers["release-before-paste-publication"].touch,
            mode=0o600,
            exist_ok=True,
        )
        markers["arm-before-paste-publication"].touch(mode=0o600)
        return markers

    def assert_plain_case_rejected_with_source_preserved(self) -> None:
        self.pump_until(
            lambda: "Word edit dispatch status=0" in self.harness.diagnostic(),
            "plain case paste rejected",
        )
        self.assertEqual(self.entry.get_text(), PASTE_CASE_ORIGINAL)
        self.assertEqual(self.desktop_source_role(), 1)
        self.assertEqual(self.keyboard_group(), 1, self.harness.diagnostic())

    def test_selection_case_clipboard_takeover_preserves_published_source(self) -> None:
        self.prepare_plain_case_paste_from_russian()
        markers = self.paste_publication_hold_markers()
        self.send_chord((KEY_LEFTALT,))
        self.pump_until(
            markers["before-paste-publication"].exists,
            "paste publication reached before clipboard takeover",
        )
        self.set_selection(Gdk.SELECTION_CLIPBOARD, "new user copy")
        markers["release-before-paste-publication"].touch(mode=0o600)
        self.assert_plain_case_rejected_with_source_preserved()
        self.assertEqual(
            self.selection_text(Gdk.SELECTION_CLIPBOARD), "new user copy"
        )

    def test_selection_case_cancellation_preserves_published_source(self) -> None:
        self.prepare_plain_case_paste_from_russian()
        markers = self.paste_publication_hold_markers()
        admitted = pathlib.Path("/run/punto-e2e-macro-ipc-admitted")
        admitted.unlink(missing_ok=True)
        self.addCleanup(admitted.unlink, missing_ok=True)
        self.send_chord((KEY_LEFTALT,))
        self.pump_until(
            markers["before-paste-publication"].exists,
            "paste publication reached before cancellation",
        )
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
            client.settimeout(EVENT_TIMEOUT)
            client.connect("/run/punto.sock")
            client.sendall(b"SET_STATUS 0\n")
            self.pump_until(
                admitted.exists,
                "status cancellation admitted while paste remains held",
            )
            markers["release-before-paste-publication"].touch(mode=0o600)
            self.assertEqual(client.recv(512), b"OK DISABLED\n")
        self.assert_plain_case_rejected_with_source_preserved()
        self.assertEqual(
            self.selection_text(Gdk.SELECTION_CLIPBOARD), "plain clipboard"
        )








    def test_selection_transliteration_has_modifier_priority(self) -> None:
        self.assert_selection_correction("привет", "privet", (KEY_LEFTCTRL, KEY_LEFTALT))
        self.harness.type_word("f")
        self.pump_until(lambda: self.entry.get_text() == "before privetf after",
                        "transliteration leaves caret after replacement")

    def test_context_change_after_paste_arm_cancels_before_dispatch(self) -> None:
        self.prepare_selected_case_paste()
        marker_names = (
            "punto-e2e-arm-after-paste-receipt",
            "punto-e2e-after-paste-receipt",
            "punto-e2e-release-after-paste-receipt",
        )
        markers = {name: pathlib.Path("/run", name) for name in marker_names}
        for marker in markers.values():
            marker.unlink(missing_ok=True)
            self.addCleanup(marker.unlink, missing_ok=True)
        self.addCleanup(markers["punto-e2e-release-after-paste-receipt"].touch,
                        exist_ok=True)
        original_pointer = self.pointer_position()
        paste_presses = sum(
            phase == "press" and name in {"v", "V"}
            for phase, name, _ in self.key_events
        )
        markers["punto-e2e-arm-after-paste-receipt"].touch(mode=0o600)
        statuses = self.harness.diagnostic().count("Word edit dispatch status=1")
        self.send_chord((KEY_LEFTALT,))
        self.pump_until(markers["punto-e2e-after-paste-receipt"].exists,
                        "paste receipt armed before dispatch")
        self.xdo("mousemove", str(original_pointer[0] + 40),
                 str(original_pointer[1]))
        markers["punto-e2e-release-after-paste-receipt"].touch(mode=0o600)
        self.pump_until(
            lambda: self.harness.diagnostic().count(
                "Word edit dispatch status=1") > statuses,
            "post-arm context rejection",
        )
        self.assertEqual(
            sum(
                phase == "press" and name in {"v", "V"}
                for phase, name, _ in self.key_events
            ),
            paste_presses,
        )
        self.assertEqual(self.entry.get_text(), PASTE_CASE_ORIGINAL)
        self.assertEqual(self.entry.get_selection_bounds(), PASTE_CASE_BOUNDS)
        self.pump_until(
            lambda: self.selection_text(Gdk.SELECTION_CLIPBOARD)
            == "startup clipboard baseline",
            "unsent context rejection restores original clipboard", timeout=2,
        )
        self.assertEqual(
            sum(
                phase == "press" and name in {"v", "V"}
                for phase, name, _ in self.key_events
            ),
            paste_presses,
        )
        self.assertEqual(self.entry.get_text(), PASTE_CASE_ORIGINAL)
        self.assertEqual(self.entry.get_selection_bounds(), PASTE_CASE_BOUNDS)

        self.xdo("mousemove", str(original_pointer[0]), str(original_pointer[1]))
        self.send_chord((KEY_LEFTALT,))
        self.pump_until(lambda: self.entry.get_text() == PASTE_CASE_RESULT,
                        "selection correction after post-arm rejection")
        self.pump_until(
            lambda: sum(
                phase == "press" and name in {"v", "V"}
                for phase, name, _ in self.key_events
            ) > paste_presses,
            "GTK observes recovery paste chord",
        )
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "1")

    def delay_selection_paste(self):
        self.prepare_selected_case_paste()
        intercepted = []

        def delay_paste(editor):
            intercepted.append(True)
            editor.stop_emission_by_name("paste-clipboard")

        handler = self.entry.connect("paste-clipboard", delay_paste)
        self.send_chord((KEY_LEFTALT,))
        self.pump_until(lambda: len(intercepted) == 1, "application receives paste chord")
        self.pump_until(lambda: "Word edit dispatch status=3" in self.harness.diagnostic(),
                        "bounded paste transfer wait expires")
        self.assertEqual(self.entry.get_text(), PASTE_CASE_ORIGINAL)
        self.assertEqual(self.entry.get_selection_bounds(), PASTE_CASE_BOUNDS)
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "0")
        self.assertEqual(ipc_request(b"GET_STATUS\n", timeout=0.2), b"OK ENABLED\n")
        self.entry.disconnect(handler)

    def test_late_paste_receipt_restores_clipboard_without_false_undo(self) -> None:
        self.delay_selection_paste()
        # Only the application's late request may acknowledge payload delivery.
        self.entry.emit("paste-clipboard")
        self.pump_until(lambda: self.entry.get_text() == PASTE_CASE_RESULT, "late real GTK paste receives retained payload")
        self.pump_for(0.35)
        self.pump_until(
            lambda: self.selection_text(Gdk.SELECTION_CLIPBOARD)
            == "startup clipboard baseline",
            "late transfer restores original clipboard",
        )
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "0")
        self.send_chord((KEY_LEFTCTRL,), KEY_Z)
        self.pump_until(lambda: self.output_count(KEY_Z, 0) == 1, "partial operation has no Punto undo")
        self.assertEqual(self.output_count(KEY_Z, 1), 1)

    def test_same_client_probe_does_not_consume_delayed_paste_payload(self) -> None:
        self.delay_selection_paste()
        self.assertEqual(
            self.selection_text(Gdk.SELECTION_CLIPBOARD),
            PASTE_CASE_REPLACEMENT,
            "same-client clipboard probe receives the prepared payload",
        )
        self.pump_for(0.1)
        self.entry.emit("paste-clipboard")
        self.pump_until(
            lambda: self.entry.get_text() == PASTE_CASE_RESULT,
            "delayed paste still receives payload after an early same-client probe",
        )
        self.pump_for(0.35)
        self.pump_until(
            lambda: self.selection_text(Gdk.SELECTION_CLIPBOARD)
            == "startup clipboard baseline",
            "quiet delivery window restores the original clipboard",
        )

    def test_original_delayed_paste_cannot_mutate_new_selection_context(self) -> None:
        self.prepare_selected_case_paste()
        daemon_pid = self.harness.process.pid
        self.set_selection(Gdk.SELECTION_CLIPBOARD, "operation A clipboard")
        markers = {
            name: pathlib.Path("/run", name)
            for name in (
                "punto-e2e-arm-delay-clipboard-payload",
                "punto-e2e-clipboard-payload-blocked",
                "punto-e2e-release-clipboard-payload",
            )
        }
        for marker in markers.values():
            marker.unlink(missing_ok=True)
            self.addCleanup(marker.unlink, missing_ok=True)
        self.addCleanup(
            markers["punto-e2e-release-clipboard-payload"].touch,
            exist_ok=True,
        )
        markers["punto-e2e-arm-delay-clipboard-payload"].touch(mode=0o600)

        self.send_chord((KEY_LEFTALT,))
        self.pump_until(
            markers["punto-e2e-clipboard-payload-blocked"].exists,
            "original operation A clipboard request blocked before transfer",
        )
        self.assertEqual(self.entry.get_text(), PASTE_CASE_ORIGINAL)
        self.assertEqual(self.entry.get_selection_bounds(), PASTE_CASE_BOUNDS)
        # Expire the real request budget before replacing its source context.
        self.pump_for(0.04)

        self.entry.set_text("field B source")
        self.entry.select_region(0, 7)
        self.pump_until(
            lambda: self.selection_text(Gdk.SELECTION_PRIMARY) == "field B",
            "new selection context B",
        )
        markers["punto-e2e-release-clipboard-payload"].touch(mode=0o600)
        self.pump_for(0.3)
        self.assertEqual(self.entry.get_text(), "field B source")
        self.assertEqual(self.entry.get_selection_bounds(), (0, 7))
        self.pump_for(2.1)
        self.pump_until(
            lambda: self.selection_text(Gdk.SELECTION_CLIPBOARD)
            == "operation A clipboard",
            "operation A clipboard restored after rejected late request",
        )
        self.assertEqual(
            self.selection_text(Gdk.SELECTION_CLIPBOARD),
            "operation A clipboard",
        )
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "0")
        self.send_chord((KEY_LEFTCTRL,), KEY_Z)
        self.pump_until(lambda: self.output_count(KEY_Z, 0) == 1,
                        "uncertain paste has no Punto undo")
        self.assertEqual(self.output_count(KEY_Z, 1), 1)
        self.entry.select_region(0, 7)
        self.pump_until(lambda: self.selection_text(Gdk.SELECTION_PRIMARY) == "field B",
                        "fresh selection after late-request refusal")
        self.send_chord((KEY_LEFTALT,))
        self.pump_until(lambda: self.entry.get_text() == "FIELD b source",
                        "fresh selection succeeds without restarting Punto")
        self.pump_until(lambda: self.stats_fields()[1]["word_dispatches"] == "1",
                        "fresh selection dispatch")
        self.harness.type_word("f")
        self.pump_until(lambda: self.entry.get_text() == "FIELD bf source",
                        "next physical key follows the recovered range")
        self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD), "operation A clipboard")
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertIsNone(self.harness.process.poll())
        for code in (KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_LEFTALT):
            self.assertFalse(self.key_is_down(code))

    def test_same_client_probe_cannot_authorize_original_paste_in_new_context(
        self,
    ) -> None:
        self.prepare_selected_case_paste()
        self.set_selection(Gdk.SELECTION_CLIPBOARD, "operation A clipboard")
        observed: list[str | None] = []

        def change_context_before_original_paste(editor):
            observed.append(self.selection_text(Gdk.SELECTION_CLIPBOARD))
            editor.set_text("field B source")
            editor.select_region(0, 7)

        handler = self.entry.connect(
            "paste-clipboard", change_context_before_original_paste
        )
        self.addCleanup(self.entry.disconnect, handler)
        self.send_chord((KEY_LEFTALT,))
        self.pump_until(lambda: bool(observed), "same-client pre-paste probe")
        self.pump_for(0.3)
        self.assertEqual(observed, [PASTE_CASE_REPLACEMENT])
        self.assertEqual(self.entry.get_text(), "field B source")
        self.assertEqual(self.entry.get_selection_bounds(), (0, 7))
        self.pump_for(2.1)
        self.pump_until(
            lambda: self.selection_text(Gdk.SELECTION_CLIPBOARD)
            == "operation A clipboard",
            "clipboard restored after original paste rejects context B",
        )

    def test_same_client_probes_cannot_retain_clipboard_forever(self) -> None:
        self.delay_selection_paste()
        deadline = time.monotonic() + 1.5
        observed = None
        while time.monotonic() < deadline:
            observed = self.selection_text(Gdk.SELECTION_CLIPBOARD)
            if observed == "startup clipboard baseline":
                break
            self.assertEqual(observed, PASTE_CASE_REPLACEMENT)
            self.pump_for(0.08)
        self.assertEqual(observed, "startup clipboard baseline")

    def test_same_client_probes_extend_quiet_grace(self) -> None:
        self.delay_selection_paste()
        deadline = time.monotonic() + 0.4
        while time.monotonic() < deadline:
            self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD), PASTE_CASE_REPLACEMENT)
            self.pump_for(0.08)
        self.pump_for(0.35)
        self.pump_until(
            lambda: self.selection_text(Gdk.SELECTION_CLIPBOARD)
            == "startup clipboard baseline",
            "clipboard restores after the same-client quiet period",
        )

    def test_foreign_probes_do_not_extend_same_client_quiet_grace(self) -> None:
        self.delay_selection_paste()
        self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD), PASTE_CASE_REPLACEMENT)
        deadline = time.monotonic() + 0.65
        observed = None
        while time.monotonic() < deadline:
            observed = self.external_selection_text()
            if observed == "startup clipboard baseline":
                break
            self.assertEqual(observed, PASTE_CASE_REPLACEMENT)
            self.pump_for(0.08)
        self.assertEqual(observed, "startup clipboard baseline")

    def test_missing_receipt_expires_and_reenables_selection_correction(self) -> None:
        self.delay_selection_paste()
        self.pump_for(1.0)
        self.assertEqual(
            self.external_selection_text(),
            PASTE_CASE_REPLACEMENT,
            "unused paste payload remains available before its deadline",
        )
        self.pump_for(1.3)
        self.assertEqual(
            self.external_selection_text(),
            "startup clipboard baseline",
            "unused paste payload expires and restores the clipboard",
        )
        self.send_chord((KEY_LEFTALT,))
        self.pump_until(
            lambda: self.entry.get_text() == PASTE_CASE_RESULT,
            "selection correction is admitted after unused paste expiry",
        )

    def test_foreign_copy_after_paste_timeout_is_preserved_and_recovers(self) -> None:
        self.delay_selection_paste()
        self.set_selection(Gdk.SELECTION_CLIPBOARD, "new user copy")
        self.wait_event_loop_idle("foreign clipboard ownership observed")
        self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD), "new user copy")
        self.send_chord((KEY_LEFTALT,))
        self.pump_until(lambda: self.entry.get_text() == PASTE_CASE_RESULT, "new correction after clipboard owner recovery")
        self.pump_until(lambda: self.selection_text(Gdk.SELECTION_CLIPBOARD) == "new user copy",
                        "new copy preserved after later successful correction")
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "1")

    def test_missing_paste_receipt_refuses_manual_then_recovers_after_expiry(self) -> None:
        self.delay_selection_paste()
        self.pump_for(0.7)

        self.entry.set_text("")
        self.entry.set_position(0)
        Gtk.Clipboard.get(Gdk.SELECTION_PRIMARY).clear()
        self.pump_until(
            lambda: self.selection_text(Gdk.SELECTION_PRIMARY) is None,
            "old primary selection released",
        )
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn", "fresh source word")
        self.harness.send_key(KEY_PAUSE)
        self.pump_until(
            lambda: "rejection_stage=busy" in self.harness.diagnostic(),
            "unknown manual range safely refuses while old paste remains pending",
        )
        self.assertEqual(self.entry.get_text(), "ghbdtn")
        self.assertEqual(self.entry.get_selection_bounds(), ())
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "0")
        self.assertEqual(self.output_count(KEY_PAUSE), 0)

        z_presses = self.output_count(KEY_Z)
        z_releases = self.output_count(KEY_Z, 0)
        self.send_chord((KEY_LEFTCTRL,), KEY_Z)
        self.pump_until(
            lambda: self.output_count(KEY_Z) == z_presses + 1
            and self.output_count(KEY_Z, 0) == z_releases + 1,
            "physical undo passes through while an old paste is pending",
        )
        self.pump_until(
            lambda: self.selection_text(Gdk.SELECTION_CLIPBOARD)
            == "startup clipboard baseline",
            "unused receipt expiry restores baseline and releases admission",
        )
        self.assertEqual(self.entry.get_text(), "ghbdtn")
        self.harness.send_key(KEY_PAUSE)
        self.pump_until(lambda: self.entry.get_text() == "привет",
                        "fresh manual operation succeeds after pending expiry")
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "1")
        self.harness.type_word("f")
        self.pump_until(lambda: self.entry.get_text() == "привета",
                        "next character uses corrected RU source after recovery")

    def test_full_stdout_stops_buffered_macro_drain_after_first_failure(self) -> None:
        self.prepare_selected_case_paste()
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
        self.prepare_selected_case_paste()
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

    def assert_layout_follows_actual_source(self, source: str, desktop_group: int,
                                          expected: str, target_group: int,
                                          *, selected: bool = False) -> None:
        self.prepare_word_editor()
        self.assertEqual(ipc_request(b"SET_STATUS 0\n"), b"OK DISABLED\n")
        self.entry.set_text(source)
        self.entry.set_position(-1)
        self.set_desktop_layout(desktop_group)
        self.assertEqual(self.keyboard_group(), desktop_group)
        self.assertEqual(self.desktop_source_role(), desktop_group)
        if selected:
            self.entry.select_region(0, -1)
            self.pump_until(lambda: self.selection_text(Gdk.SELECTION_PRIMARY) == source,
                            "actual user selection without daemon typing history")
            self.send_chord((KEY_LEFTSHIFT,))
        else:
            self.harness.send_key(KEY_PAUSE)
        self.pump_until(lambda: self.entry.get_text() == expected,
                        "actual source determines word and selection direction")
        self.pump_until(lambda: self.stats_fields()[1]["word_dispatches"] == "1",
                        "actual-source operation dispatched")
        self.assertEqual(self.entry.get_selection_bounds(), ())
        self.assertEqual(self.entry.get_position(), len(expected))
        self.assertEqual(self.keyboard_group(), target_group)
        self.assertEqual(self.desktop_source_role(), target_group)
        self.harness.type_word("f")
        self.pump_until(lambda: self.entry.get_text() ==
                        expected + ("а" if target_group else "f"),
                        "next physical key uses the transformed source layout")
        self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD),
                         "startup clipboard baseline")
        self.assertEqual(self.output_count(KEY_PAUSE), 0)
        for key in (KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_LEFTALT):
            self.assertFalse(self.key_is_down(key))

    def test_extensionless_current_ru_word_with_en_desktop(self) -> None:
        self.assert_layout_follows_actual_source("привет", 0, "ghbdtn", 0)

    def test_extensionless_current_en_word_with_ru_desktop(self) -> None:
        self.assert_layout_follows_actual_source("ghbdtn", 1, "привет", 1)

    def test_extensionless_selected_ru_word_with_en_desktop(self) -> None:
        self.assert_layout_follows_actual_source("привет", 0, "ghbdtn", 0, selected=True)

    def test_extensionless_selected_en_word_with_ru_desktop(self) -> None:
        self.assert_layout_follows_actual_source("ghbdtn", 1, "привет", 1, selected=True)

    def test_first_word_after_focus_switch_between_fields_is_independent(self) -> None:
        daemon_pid = self.harness.process.pid
        daemon_start = process_start_time(daemon_pid)
        helper_pid = (self.session_bridge_process.pid
                      if self.session_bridge_process is not None else None)
        helper_start = process_start_time(helper_pid) if helper_pid is not None else None
        parent_xid = self.window.get_window().get_xid()
        self.window.remove(self.entry)
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8)
        other = Gtk.Entry()
        box.pack_start(self.entry, False, False, 0)
        box.pack_start(other, False, False, 0)
        self.window.add(box)
        self.window.set_default_size(400, 100)
        self.window.show_all()
        self.entry.grab_focus()
        self.pump_until(self.entry.has_focus, "first field focused")
        self.prepare_word_editor()
        warm_marker = b"[punto-e2e] editor_warmed\n"
        self.pump_until(lambda: warm_marker in bytes(self.harness.stderr),
                        "first empty field idle preparation completed")
        self.assertEqual(self.stats_fields()[1]["history_words"], "0")

        other.grab_focus()
        self.pump_until(other.has_focus, "second empty field focused")
        self.assertEqual(self.window.get_window().get_xid(), parent_xid)
        self.assertEqual(self.stats_fields()[1]["history_words"], "0")
        completed_before = bytes(self.harness.stderr).count(warm_marker)
        # One synchronous read for A may finish after B gains focus. Requiring
        # two new completions excludes that read from satisfying preparation.
        self.pump_until(
            lambda: bytes(self.harness.stderr).count(warm_marker) >= completed_before + 2,
            "second empty field idle preparation completed after focus switch",
        )
        self.assertEqual(self.entry.get_text(), "")
        self.assertEqual(other.get_text(), "")
        self.assertEqual(self.stats_fields()[1]["history_words"], "0")

        self.entry.grab_focus()
        self.pump_until(self.entry.has_focus, "first field refocused before physical word")
        self.harness.type_word("hELLo")
        self.pump_until(lambda: self.entry.get_text() == "hELLo",
                        "first field physical word")
        self.send_chord((KEY_LEFTCTRL,))
        self.pump_until(lambda: self.entry.get_text() == "HellO",
                        "first field correction")

        other.grab_focus()
        self.pump_until(other.has_focus, "second field focused")
        self.assertEqual(self.window.get_window().get_xid(), parent_xid)

        self.harness.type_word("hELLo")
        self.pump_until(lambda: other.get_text() == "hELLo",
                        "second field first physical word")
        self.send_chord((KEY_LEFTCTRL,))
        try:
            self.pump_until(lambda: other.get_text() == "HellO",
                            "first correction in the second field")
        except AssertionError as error:
            self.fail(f"{error}\nfirst={self.entry.get_text()!r} "
                      f"second={other.get_text()!r} focus={other.has_focus()}")
        self.assertEqual(self.entry.get_text(), "HellO")
        self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD),
                         "startup clipboard baseline")
        self.harness.type_word("f")
        self.pump_until(lambda: other.get_text() == "HellOf",
                        "next physical key stays in second field")
        self.assertEqual(self.entry.get_text(), "HellO")
        self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD),
                         "startup clipboard baseline")
        for key in (KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_LEFTALT):
            self.assertFalse(self.key_is_down(key))
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertEqual(process_start_time(daemon_pid), daemon_start)
        self.assertIsNone(self.harness.process.poll())
        if helper_pid is not None:
            self.assertEqual(self.session_bridge_process.pid, helper_pid)
            self.assertEqual(process_start_time(helper_pid), helper_start)
            self.assertIsNone(self.session_bridge_process.poll())

    def test_first_word_after_reparent_and_focus_switch_is_independent(self) -> None:
        self.prepare_word_editor()
        self.harness.type_word("hELLo")
        self.pump_until(lambda: self.entry.get_text() == "hELLo",
                        "original field physical word before reparent")
        self.send_chord((KEY_LEFTCTRL,))
        self.pump_until(lambda: self.entry.get_text() == "HellO",
                        "original field correction before reparent")

        self.window.remove(self.entry)
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8)
        other = Gtk.Entry()
        box.pack_start(self.entry, False, False, 0)
        box.pack_start(other, False, False, 0)
        self.window.add(box)
        self.window.show_all()
        other.grab_focus()
        self.pump_until(other.has_focus, "new field focused after reparent")

        self.harness.type_word("hELLo")
        self.pump_until(lambda: other.get_text() == "hELLo",
                        "new field physical word after reparent")
        self.send_chord((KEY_LEFTCTRL,))
        self.pump_until(lambda: other.get_text() == "HellO",
                        "first correction in new field after reparent")
        self.assertEqual(self.entry.get_text(), "HellO")
        self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD),
                         "startup clipboard baseline")

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

    def test_textview_editor_transaction_word_and_selection(self) -> None:
        self.prepare_word_editor()
        self.window.remove(self.entry)
        editor = Gtk.TextView()
        self.window.add(editor)
        self.window.show_all()
        editor.grab_focus()
        self.pump_until(editor.has_focus, "TextView transaction focus")
        document = editor.get_buffer()

        self.harness.type_word("hELLo")
        self.pump_until(
            lambda: document.get_text(*document.get_bounds(), True) == "hELLo",
            "TextView physical source word",
        )
        self.send_chord((KEY_LEFTCTRL,))
        self.pump_until(
            lambda: document.get_text(*document.get_bounds(), True) == "HellO",
            "TextView editor-owned word change",
        )
        caret = document.get_iter_at_mark(document.get_insert()).get_offset()
        self.assertEqual(caret, 5)

        document.set_text("before ghbdtn after")
        document.select_range(
            document.get_iter_at_offset(7), document.get_iter_at_offset(13))
        self.pump_until(
            lambda: self.selection_text(Gdk.SELECTION_PRIMARY) == "ghbdtn",
            "TextView editor-owned selection source",
        )
        self.send_chord((KEY_LEFTSHIFT,))
        self.pump_until(
            lambda: document.get_text(*document.get_bounds(), True)
            == "before привет after",
            "TextView editor-owned selection change",
        )
        self.assertEqual(document.get_selection_bounds(), ())
        self.assertEqual(document.get_iter_at_mark(document.get_insert()).get_offset(), 13)
        self.harness.type_word("f")
        self.pump_until(lambda: document.get_text(*document.get_bounds(), True)
                        == "before привета after", "next key preserves TextView prefix and suffix")
        self.assertEqual(
            self.selection_text(Gdk.SELECTION_CLIPBOARD),
            "startup clipboard baseline",
        )

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
        self.pump_until(lambda: self.output_count(KEY_TAB, 0) == 1,
                        "original TextView Tab release")
        self.assertEqual(self.output_count(KEY_TAB, 0), 1)
        self.assertEqual(self.output_count(KEY_PAUSE), 0)
        self.pump_until(lambda: self.selection_text(Gdk.SELECTION_CLIPBOARD) == "startup clipboard baseline",
                        "automatic tab replacement restores clipboard")
        self.harness.type_word("f")
        self.pump_until(lambda: visible_text() == "привет\tа", "automatic Tab retains target layout")

    def test_auto_entry_tab_corrects_before_focus_transfer(self) -> None:
        self.prepare_word_editor()
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        next_editor = Gtk.TextView()
        self.window.remove(self.entry)
        box.pack_start(self.entry, False, False, 0)
        box.pack_start(next_editor, True, True, 0)
        self.window.add(box)
        self.window.show_all()
        self.entry.grab_focus()
        self.pump_until(self.entry.has_focus, "source Entry focus before Tab")

        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn", "source word")
        sent_at = time.monotonic()
        self.harness.send_key(KEY_TAB)
        self.pump_until(lambda: self.output_count(KEY_TAB, 1) == 1,
                        "original Tab output")
        output_ms = round((time.monotonic() - sent_at) * 1000)
        self.assertLess(output_ms, 1000)
        print(json.dumps({"r37_upstream_tab_release_ms": output_ms,
                          "path": "EventLoop_to_XTEST_relay_to_GTK"}), flush=True)
        self.pump_until(next_editor.has_focus, "original Tab focus transfer")
        self.assertEqual(self.entry.get_text(), "привет", self.harness.diagnostic())
        self.assertEqual(self.output_count(KEY_TAB, 1), 1)
        self.pump_until(lambda: self.output_count(KEY_TAB, 0) == 1,
                        "original Tab release")
        self.assertEqual(self.output_count(KEY_TAB, 0), 1)
        self.pump_until(lambda: self.selection_text(Gdk.SELECTION_CLIPBOARD)
                        == "startup clipboard baseline", "original clipboard restoration")
        self.harness.type_word("f")
        document = next_editor.get_buffer()
        self.pump_until(lambda: document.get_text(*document.get_bounds(), True) == "а",
                        "first key in corrected layout after focus transfer")

    def test_auto_literal_tab_keeps_punto_undo(self) -> None:
        self.prepare_word_editor()
        self.window.remove(self.entry)
        editor = Gtk.TextView()
        self.window.add(editor)
        self.window.show_all()
        editor.grab_focus()
        self.pump_until(editor.has_focus, "literal Tab undo source field")
        document = editor.get_buffer()
        text = lambda: document.get_text(*document.get_bounds(), True)
        self.harness.type_word("ghbdtn")
        self.harness.send_key(KEY_TAB)
        self.pump_until(lambda: text() == "привет\t", "actual automatic correction plus literal Tab")
        self.pump_until(lambda: self.selection_text(Gdk.SELECTION_CLIPBOARD) == "startup clipboard baseline",
                        "clipboard settled before immediate Punto undo")
        self.send_chord((KEY_LEFTCTRL,), KEY_Z)
        self.pump_until(lambda: text() == "ghbdtn\t", "exact Punto inverse includes original literal Tab")
        self.assertEqual(self.output_count(KEY_Z), 0)
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "2")
        self.assertEqual(self.output_count(KEY_TAB, 1), 1)
        self.assertEqual(self.output_count(KEY_TAB, 0), 1)
        self.harness.type_word("f")
        self.pump_until(lambda: text() == "ghbdtn\tf", "undo restores EN for the next ordinary key")

    def test_auto_literal_tab_three_backspaces_learn_once(self) -> None:
        self.prepare_word_editor()
        self.window.remove(self.entry)
        editor = Gtk.TextView()
        self.window.add(editor)
        self.window.show_all()
        editor.grab_focus()
        self.pump_until(editor.has_focus, "literal Tab learning source field")
        document = editor.get_buffer()
        text = lambda: document.get_text(*document.get_bounds(), True)
        self.harness.type_word("ghbdtn")
        self.harness.send_key(KEY_TAB)
        self.pump_until(lambda: text() == "привет\t", "automatic Tab correction before learning")
        self.pump_until(lambda: self.selection_text(Gdk.SELECTION_CLIPBOARD) == "startup clipboard baseline",
                        "clipboard settled before Backspace learning")
        exclusions = pathlib.Path("/etc/punto/undo_exclusions.txt")
        baseline_generation = self.stats_fields()[1]["learning_generation"]
        self.assertEqual(self.stats_fields()[1]["exclusions"], "0")
        self.assertFalse(exclusions.exists() and "ghbdtn\n" in exclusions.read_text(),
                         "correction alone must not teach an exclusion")
        for number, expected in enumerate(("привет", "приве", "прив"), 1):
            self.harness.send_key(KEY_BACKSPACE)
            self.pump_until(lambda: text() == expected, f"actual Backspace {number}")
            if number < 3:
                fields = self.stats_fields()[1]
                self.assertEqual(fields["exclusions"], "0")
                self.assertEqual(fields["learning_generation"], baseline_generation)
                self.assertFalse(exclusions.exists() and "ghbdtn\n" in exclusions.read_text(),
                                 "fewer than three Backspaces must not teach an exclusion")
        self.pump_until(lambda: text() == "прив", "three actual Backspaces include the literal Tab")
        self.pump_until(lambda: exclusions.exists() and "ghbdtn\n" in exclusions.read_text(),
                        "automatic Tab uses the shared persistent learning consumer")
        self.assertEqual(exclusions.read_text().splitlines().count("ghbdtn"), 1)
        self.assertEqual(self.output_count(KEY_BACKSPACE, 1), 3)
        self.assertEqual(self.output_count(KEY_BACKSPACE, 0), 3)

    def test_auto_entry_tab_read_deadline_releases_original_tab(self) -> None:
        self.prepare_word_editor()
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        next_editor = Gtk.TextView()
        self.window.remove(self.entry)
        box.pack_start(self.entry, False, False, 0)
        box.pack_start(next_editor, True, True, 0)
        self.window.add(box)
        self.window.show_all()
        self.entry.grab_focus()
        self.pump_until(self.entry.has_focus, "source Entry before deadline control")
        daemon_pid = self.harness.process.pid
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn", "source word")
        marker = pathlib.Path("/run/punto-e2e-withhold-primary-conversion-barrier")
        marker.touch(mode=0o600)
        sent_at = time.monotonic()
        self.harness.send_key(KEY_TAB)
        self.pump_until(lambda: self.output_count(KEY_TAB, 0) == 1,
                        "original Tab pair after bounded read refusal")
        self.assertLess(time.monotonic() - sent_at, 1.0)
        self.assertFalse(marker.exists())
        self.assertTrue(pathlib.Path(
            "/run/punto-e2e-primary-conversion-barrier-submitted").exists())
        self.pump_until(next_editor.has_focus, "unchanged Tab reaches destination")
        self.assertEqual(self.entry.get_text(), "ghbdtn")
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "0")
        self.assertEqual(self.output_count(KEY_TAB, 1), 1)
        self.assertEqual(self.output_count(KEY_TAB, 0), 1)
        self.harness.type_word("f")
        document = next_editor.get_buffer()
        self.pump_until(lambda: document.get_text(*document.get_bounds(), True) == "f",
                        "ordinary next key stays in original EN source")
        self.pump_for(0.1)
        self.assertEqual(self.entry.get_text(), "ghbdtn")
        self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD),
                         "startup clipboard baseline")
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertIsNone(self.harness.process.poll())
        for key in (KEY_TAB, KEY_LEFTCTRL, KEY_LEFTSHIFT):
            self.assertFalse(self.key_is_down(key))

    def test_auto_entry_tab_reader_loss_collapses_exact_source_range(self) -> None:
        self.prepare_word_editor()
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        destination = Gtk.TextView()
        self.window.remove(self.entry)
        box.pack_start(self.entry, False, False, 0)
        box.pack_start(destination, True, True, 0)
        self.window.add(box)
        self.window.show_all()
        self.entry.grab_focus()
        self.pump_until(self.entry.has_focus, "source before PRIMARY reader loss")
        daemon_pid = self.harness.process.pid
        names = ("fail-next-primary-transport",)
        markers = [pathlib.Path("/run", f"punto-e2e-{name}") for name in names]
        for marker in markers:
            marker.unlink(missing_ok=True)
            self.addCleanup(marker.unlink, missing_ok=True)
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn", "real typed source")
        prepared = []

        def close_prepared_reader(_entry, _property):
            if not prepared and self.entry.get_selection_bounds() == (0, 6):
                prepared.append(self.output_count(KEY_TAB, 1))
                markers[0].touch(mode=0o600)

        self.entry.connect("notify::selection-bound", close_prepared_reader)
        self.entry.connect("notify::cursor-position", close_prepared_reader)
        started = time.monotonic()
        self.harness.send_key(KEY_TAB)
        self.pump_until(lambda: self.output_count(KEY_TAB, 0) == 1,
                        "unchanged Tab after bounded reader failure")
        self.assertLess(time.monotonic() - started, 1.0)
        self.assertEqual(prepared, [0], "actual selection was prepared before original Tab")
        self.assertFalse(markers[0].exists(), "real PRIMARY read consumed the connection-failure seam")
        rights = [phase for phase, name, _ in self.key_events if name == "Right"]
        self.assertEqual(rights, ["press", "release"],
                         "exact continuous accessible range authorizes one collapse pair")
        self.assertEqual(self.entry.get_text(), "ghbdtn")
        self.assertEqual(self.entry.get_selection_bounds(), ())
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "0")
        self.assertEqual(self.output_count(KEY_TAB, 1), 1)
        self.assertEqual(self.output_count(KEY_TAB, 0), 1)
        self.assertEqual(self.output_count(KEY_V), 0)
        self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD), "startup clipboard baseline")
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertIsNone(self.harness.process.poll())
        for key in (KEY_TAB, KEY_RIGHT, KEY_LEFTCTRL, KEY_LEFTSHIFT):
            self.assertFalse(self.key_is_down(key))

    def test_auto_entry_tab_prepared_cleanup_respects_hard_expiry(self) -> None:
        self.prepare_word_editor()
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        destination = Gtk.TextView()
        self.window.remove(self.entry)
        box.pack_start(self.entry, False, False, 0)
        box.pack_start(destination, True, True, 0)
        self.window.add(box)
        self.window.show_all()
        self.entry.grab_focus()
        self.pump_until(self.entry.has_focus, "source Entry before prepared expiry")
        daemon_pid = self.harness.process.pid
        observations = []

        def observe_selection(_entry, _property):
            if self.entry.get_selection_bounds() == (0, 6):
                observations.append(self.output_count(KEY_TAB, 1))

        self.entry.connect("notify::selection-bound", observe_selection)
        self.entry.connect("notify::cursor-position", observe_selection)
        names = ("arm-fail-selection-revalidation",
                 "selection-revalidation-fault-armed",
                 "primary-payload-read-failed",
                 "expire-prepared-cleanup-hard-deadline",
                 "prepared-cleanup-hard-deadline-expired")
        markers = [pathlib.Path("/run", f"punto-e2e-{name}") for name in names]
        for marker in markers:
            marker.unlink(missing_ok=True)
            self.addCleanup(marker.unlink, missing_ok=True)
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn", "physical source")
        markers[0].touch(mode=0o600)
        markers[3].touch(mode=0o600)
        sent_at = time.monotonic()
        self.harness.send_key(KEY_TAB)
        self.pump_until(lambda: self.output_count(KEY_TAB, 0) == 1,
                        "original Tab after injected hard expiry")
        self.assertLess(time.monotonic() - sent_at, 1.0)
        self.assertTrue(all(markers[index].exists() for index in (1, 2, 4)))
        self.assertIn(0, observations, "real prepared range before Tab dispatch")
        self.assertFalse(any(phase == "press" and name == "Right"
                             for phase, name, _time in self.key_events))
        self.assertEqual(self.entry.get_text(), "ghbdtn")
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "0")
        self.assertEqual(self.output_count(KEY_TAB, 1), 1)
        self.assertEqual(self.output_count(KEY_TAB, 0), 1)
        self.pump_until(destination.has_focus, "original Tab transfers focus")
        self.harness.type_word("f")
        document = destination.get_buffer()
        self.pump_until(lambda: document.get_text(*document.get_bounds(), True) == "а",
                        "next physical key follows observed target RU layout")
        self.pump_for(0.1)
        self.assertEqual(self.entry.get_text(), "ghbdtn")
        self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD),
                         "startup clipboard baseline")
        for key in (KEY_TAB, KEY_RIGHT, KEY_LEFTCTRL, KEY_LEFTSHIFT):
            self.assertFalse(self.key_is_down(key))
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertIsNone(self.harness.process.poll())

    def test_auto_entry_tab_nested_delete_preserves_following_input(self) -> None:
        self.prepare_word_editor()
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        next_editor = Gtk.TextView()
        self.window.remove(self.entry)
        box.pack_start(self.entry, False, False, 0)
        box.pack_start(next_editor, True, True, 0)
        self.window.add(box)
        self.window.show_all()
        self.entry.grab_focus()
        self.pump_until(self.entry.has_focus, "source Entry focus")
        observations: list[tuple[str, bool, str, int]] = []
        deletions: list[tuple[int, int]] = []
        insertions: list[str] = []
        armed = {"value": True}
        release = {"value": False}

        def on_delete(_widget, _start, _end):
            if not armed["value"]:
                return
            armed["value"] = False
            deletions.append((_start, _end))

            def send_following_input():
                self.harness.type_word("f")
                return False

            def release_from_control_source():
                observations.append(("before_release", self.entry.has_focus(),
                                     self.entry.get_text(),
                                     self.output_count(KEY_TAB, 1)))
                release["value"] = True
                return False

            GLib.timeout_add(5, send_following_input)
            GLib.timeout_add(40, release_from_control_source)
            until = time.monotonic() + 2.0
            while not release["value"] and time.monotonic() < until:
                if Gtk.events_pending():
                    Gtk.main_iteration_do(False)
                else:
                    time.sleep(0.002)

        self.entry.connect("delete-text", on_delete)
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn", "source word")
        self.entry.get_buffer().connect(
            "inserted-text", lambda _buffer, _position, text, _length:
            insertions.append(text))
        self.harness.send_key(KEY_TAB)
        self.pump_until(lambda: bool(observations), "nested delete control callback")
        self.assertEqual(observations, [("before_release", True, "ghbdtn", 0)],
                         self.harness.diagnostic())
        self.pump_until(next_editor.has_focus, "original Tab focus after edit")
        self.assertEqual(self.entry.get_text(), "привет", self.harness.diagnostic())
        self.assertEqual(deletions, [(0, 6)])
        self.assertEqual(insertions, ["привет"])
        document = next_editor.get_buffer()
        self.pump_until(lambda: document.get_text(*document.get_bounds(), True) == "а",
                        "queued following key after original Tab")
        self.assertEqual(self.output_count(KEY_TAB, 1), 1)
        self.pump_until(lambda: self.output_count(KEY_TAB, 0) == 1,
                        "original Tab release")
        self.pump_until(lambda: self.selection_text(Gdk.SELECTION_CLIPBOARD)
                        == "startup clipboard baseline", "original clipboard restoration")
        for code in (KEY_TAB, KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_LEFTALT):
            self.assertFalse(self.key_is_down(code))

    def test_auto_tab_portable_clipboard_preserves_all_representations(self) -> None:
        self.prepare_word_editor()
        self.window.remove(self.entry)
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        destination = Gtk.Entry()
        box.pack_start(self.entry, False, False, 0)
        box.pack_start(destination, False, False, 0)
        self.window.add(box)
        self.window.show_all()
        self.entry.grab_focus()
        self.pump_until(self.entry.has_focus, "portable clipboard source focus")
        clipboard = Gtk.Clipboard.get(Gdk.SELECTION_CLIPBOARD)
        payloads = (
            ("UTF8_STRING", "UTF8_STRING", 8, b"portable clipboard sentinel"),
            ("text/html", "text/html", 8, b"<b>portable</b>\x00tail"),
            ("application/x-punto-fixture", "application/octet-stream", 8,
             b"\x00\xff\x01\x00\x7f\x80\x00"),
        )
        class TargetEntry(ctypes.Structure):
            _fields_ = [("target", ctypes.c_char_p), ("flags", ctypes.c_uint),
                        ("info", ctypes.c_uint)]
        get_type = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p,
                                   ctypes.c_uint, ctypes.c_void_p)
        clear_type = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p)
        library = ctypes.CDLL(ctypes.util.find_library("gtk-3"))
        gdk = ctypes.CDLL(ctypes.util.find_library("gdk-3"))
        gdk.gdk_atom_intern.argtypes = (ctypes.c_char_p, ctypes.c_int)
        gdk.gdk_atom_intern.restype = ctypes.c_void_p
        library.gtk_selection_data_set.argtypes = (
            ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_int)
        def provide(_clipboard, selection, info, _data):
            _, data_type, format_bits, data = payloads[info]
            storage = ctypes.create_string_buffer(data)
            library.gtk_selection_data_set(selection,
                gdk.gdk_atom_intern(data_type.encode("ascii"), False),
                format_bits, storage, len(data))
        get = get_type(provide)
        clear = clear_type(lambda *_args: None)
        targets = (TargetEntry * len(payloads))(*(
            TargetEntry(name.encode("ascii"), 0, index)
            for index, (name, *_rest) in enumerate(payloads)))
        capsule_pointer = ctypes.pythonapi.PyCapsule_GetPointer
        capsule_pointer.argtypes = (ctypes.py_object, ctypes.c_char_p)
        capsule_pointer.restype = ctypes.c_void_p
        library.gtk_clipboard_set_with_data.argtypes = (
            ctypes.c_void_p, ctypes.POINTER(TargetEntry), ctypes.c_uint,
            get_type, clear_type, ctypes.c_void_p)
        library.gtk_clipboard_set_with_data.restype = ctypes.c_int
        self.assertTrue(library.gtk_clipboard_set_with_data(
            capsule_pointer(clipboard.__gpointer__, None), targets, len(targets),
            get, clear, None))
        self._portable_clipboard_callbacks = (get, clear, targets, library, gdk)
        self.addCleanup(clipboard.set_text, "retired portable fixture", -1)
        def snapshot():
            available, atoms = clipboard.wait_for_targets()
            self.assertTrue(available)
            names = sorted(atom.name() for atom in atoms if atom.name() not in
                           ("TARGETS", "TIMESTAMP", "MULTIPLE", "SAVE_TARGETS"))
            data = []
            for name in names:
                value = clipboard.wait_for_contents(Gdk.atom_intern(name, False))
                self.assertIsNotNone(value, name)
                data.append((name, value.get_data_type().name(), value.get_format(),
                             value.get_data()))
            return data
        before = snapshot()
        self.assertEqual(before, sorted(payloads))
        daemon_pid = self.harness.process.pid
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn", "portable source word")
        self.harness.send_key(KEY_TAB)
        self.pump_until(destination.has_focus, "original Tab after portable correction")
        self.assertEqual(self.entry.get_text(), "привет", self.harness.diagnostic())
        self.pump_until(lambda: self.selection_text(Gdk.SELECTION_CLIPBOARD)
                        == "portable clipboard sentinel", "complete baseline restoration")
        self.assertEqual(snapshot(), before)
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertIsNone(self.harness.process.poll())
        self.assertEqual(self.output_count(KEY_TAB, 1), 1)
        self.assertEqual(self.output_count(KEY_TAB, 0), 1)
        self.harness.type_word("f")
        self.pump_until(lambda: destination.get_text() == "а", "next key in RU after Tab")
        for code in (KEY_TAB, KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_LEFTALT):
            self.assertFalse(self.key_is_down(code))

    def test_auto_tab_rich_clipboard_refuses_and_recovers(self) -> None:
        self.assert_auto_tab_rich_clipboard_refuses_and_recovers()

    def test_auto_tab_fresh_origin_with_renewed_none_identity_recovers(self) -> None:
        self.assert_auto_tab_rich_clipboard_refuses_and_recovers(renew_primary=True)

    def assert_auto_tab_rich_clipboard_refuses_and_recovers(self, renew_primary=False) -> None:
        self.prepare_word_editor()
        ownership = []
        primary = Gtk.Clipboard.get(Gdk.SELECTION_PRIMARY)
        handler = primary.connect(
            "owner-change", lambda _clipboard, event:
            ownership.append((event.owner, event.selection_time)))
        self.addCleanup(primary.disconnect, handler)
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        next_editor = Gtk.TextView()
        self.window.remove(self.entry)
        box.pack_start(self.entry, False, False, 0)
        box.pack_start(next_editor, True, True, 0)
        self.window.add(box)
        self.window.show_all()
        self.entry.grab_focus()
        self.pump_until(self.entry.has_focus, "source Entry focus")
        daemon_pid = self.harness.process.pid

        rich_document = Gtk.TextBuffer()
        rich_document.set_text("formatted clipboard sentinel")
        rich_document.apply_tag(rich_document.create_tag("emphasis", weight=700),
                                *rich_document.get_bounds())
        rich_document.select_range(*rich_document.get_bounds())
        clipboard = Gtk.Clipboard.get(Gdk.SELECTION_CLIPBOARD)
        rich_document.copy_clipboard(clipboard)
        Gdk.flush()

        def snapshot():
            available, atoms = clipboard.wait_for_targets()
            self.assertTrue(available)
            names = sorted(atom.name() for atom in atoms)
            self.assertTrue(any("rich-text" in name for name in names), names)
            representations = []
            for name in names:
                if name in ("TARGETS", "MULTIPLE", "SAVE_TARGETS"):
                    continue
                payload = clipboard.wait_for_contents(Gdk.atom_intern(name, False))
                self.assertIsNotNone(payload, name)
                representations.append((name, payload.get_data_type().name(),
                                        payload.get_format(), payload.get_data()))
            return self.selection_owner("CLIPBOARD"), names, representations

        before = snapshot()
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn", "source word")
        self.harness.send_key(KEY_TAB)
        self.pump_until(next_editor.has_focus, "original Tab after safe clipboard refusal")
        self.pump_until(lambda: self.output_count(KEY_TAB, 0) == 1,
                        "original refused Tab release")
        self.assertEqual(self.entry.get_text(), "ghbdtn", self.harness.diagnostic())
        self.assertEqual(self.entry.get_selection_bounds(), ())
        self.assertEqual(snapshot(), before)
        self.assertEqual(self.output_count(KEY_TAB, 1), 1)
        self.assertIn("r37_tab_edit status=0 stage=clipboard_snapshot_targets",
                      self.harness.diagnostic())
        self.assertEqual(self.output_count(KEY_V), 0)
        self.assertFalse(any(phase == "press" and name in ("v", "V")
                             for phase, name, _ in self.key_events))

        self.set_selection(Gdk.SELECTION_CLIPBOARD, "startup clipboard baseline")
        self.entry.grab_focus_without_selecting()
        self.pump_until(self.entry.has_focus, "fresh source focus without restart")
        self.assertEqual(self.entry.get_selection_bounds(), ())
        self.assertEqual(self.entry.get_position(), len("ghbdtn"))
        if renew_primary:
            owner = self.selection_owner()
            self.assertNotEqual(owner, 0)
            self.pump_until(
                lambda: any(window and window.get_xid() == owner
                            for window, _stamp in ownership),
                "retained GTK PRIMARY ownership event")
            owner_window, old_stamp = next(
                (window, stamp) for window, stamp in reversed(ownership)
                if window and window.get_xid() == owner)
            self.assertIsNone(self.selection_text(Gdk.SELECTION_PRIMARY))
            self.assertTrue(Gdk.selection_owner_set_for_display(
                Gdk.Display.get_default(), owner_window, Gdk.SELECTION_PRIMARY,
                Gdk.CURRENT_TIME, False))
            Gdk.flush()
            self.pump_until(
                lambda: any(window and window.get_xid() == owner and
                            stamp != old_stamp for window, stamp in ownership),
                "fresh same-owner server PRIMARY identity")
            self.assertEqual(self.selection_owner(), owner)
            self.assertIsNone(self.selection_text(Gdk.SELECTION_PRIMARY))
        self.set_desktop_layout(0)
        self.harness.send_key(KEY_SPACE)
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn ghbdtn",
                        "fresh input without bypassing PRIMARY admission")
        self.harness.send_key(KEY_TAB)
        self.pump_until(next_editor.has_focus, "fresh original Tab focus transfer")
        self.assertEqual(self.entry.get_text(), "ghbdtn привет", self.harness.diagnostic())
        self.pump_until(lambda: self.output_count(KEY_TAB, 0) == 2,
                        "fresh original Tab release")
        self.assertEqual(self.output_count(KEY_TAB, 1), 2)
        self.harness.type_word("f")
        document = next_editor.get_buffer()
        self.pump_until(lambda: document.get_text(*document.get_bounds(), True) == "а",
                        "fresh destination input uses recovered RU source")
        self.pump_until(lambda: self.selection_text(Gdk.SELECTION_CLIPBOARD)
                        == "startup clipboard baseline", "recovered clipboard restoration")
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertIsNone(self.harness.process.poll())
        for code in (KEY_TAB, KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_LEFTALT):
            self.assertFalse(self.key_is_down(code))

    def test_auto_tab_preflight_refuses_before_paste_and_recovers(self) -> None:
        self.prepare_word_editor()
        fault = pathlib.Path("/run/punto-e2e-withhold-post-bridge-pointer")
        consumed = pathlib.Path("/run/punto-e2e-post-bridge-pointer-reply-withheld")
        for marker in (fault, consumed):
            marker.unlink(missing_ok=True)
            self.addCleanup(marker.unlink, missing_ok=True)
        fault.touch(mode=0o600)
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        next_editor = Gtk.TextView()
        self.window.remove(self.entry)
        box.pack_start(self.entry, False, False, 0)
        box.pack_start(next_editor, True, True, 0)
        self.window.add(box)
        self.window.show_all()
        self.entry.grab_focus()
        self.pump_until(self.entry.has_focus, "source Entry focus")
        daemon_pid = self.harness.process.pid
        daemon_start = process_start_time(daemon_pid)
        activations = self.desktop_activation_count()
        clipboard_owner = self.selection_owner("CLIPBOARD")
        self.addCleanup(self.set_desktop_layout, 0)
        if self.real_layout_bridge is not None:
            self.addCleanup(setattr, self.real_layout_bridge, "suppress_xkb_once", False)
            self.real_layout_bridge.suppress_xkb_once = True
        else:
            self.layout_shortcut.arm_source_only_activation()
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn", "source word")
        started = time.monotonic()
        self.harness.send_key(KEY_TAB)
        self.pump_until(next_editor.has_focus, "original Tab after preflight refusal")
        self.pump_until(lambda: self.output_count(KEY_TAB, 0) == 1,
                        "refused original Tab release")
        self.assertFalse(fault.exists())
        self.assertTrue(consumed.exists(), "genuine pointer reply captured before deadline")
        self.assertIn("r37_tab_edit status=0 stage=layout_bridge_xkb_timeout",
                      self.harness.diagnostic())
        tab_times = [at for at, kind, code, _ in self.harness.relay.snapshot()
                     if kind == EV_KEY and code == KEY_TAB]
        self.assertEqual(len(tab_times), 2)
        self.assertLessEqual(max(tab_times) - started, 1.0,
                             self.harness.diagnostic())
        self.assertEqual(self.desktop_activation_count(), activations + 1)
        self.assertEqual(self.desktop_source_role(), 1)
        if self.real_layout_bridge is not None:
            self.assertFalse(self.real_layout_bridge.suppress_xkb_once)
        self.assertEqual(self.entry.get_text(), "ghbdtn", self.harness.diagnostic())
        self.assertEqual(self.entry.get_selection_bounds(), ())
        self.assertEqual(self.entry.get_position(), len("ghbdtn"))
        self.assertEqual(self.keyboard_group(), 0)
        self.assertEqual(self.output_count(KEY_TAB, 1), 1)
        self.assertEqual(self.selection_owner("CLIPBOARD"), clipboard_owner)
        self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD),
                         "startup clipboard baseline")
        self.assertFalse(any(phase == "press" and name in ("v", "V")
                             for phase, name, _ in self.key_events))
        for code in (KEY_TAB, KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_LEFTALT):
            self.assertFalse(self.key_is_down(code))

        self.set_desktop_layout(0)
        self.entry.grab_focus_without_selecting()
        self.pump_until(self.entry.has_focus, "fresh source focus without restart")
        self.assertEqual(self.entry.get_selection_bounds(), ())
        self.assertEqual(self.entry.get_position(), len("ghbdtn"))
        self.harness.send_key(KEY_SPACE)
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn ghbdtn",
                        "fresh input after desktop mismatch without PRIMARY reset")
        self.harness.send_key(KEY_TAB)
        self.pump_until(next_editor.has_focus, "fresh original Tab focus transfer")
        self.assertEqual(self.entry.get_text(), "ghbdtn привет", self.harness.diagnostic())
        self.pump_until(lambda: self.output_count(KEY_TAB, 0) == 2,
                        "fresh original Tab release")
        self.assertEqual(self.output_count(KEY_TAB, 1), 2)
        self.assertEqual(self.desktop_activation_count(), activations + 2)
        self.assertEqual(self.desktop_source_role(), 1)
        self.assertEqual(self.keyboard_group(), 1)
        self.harness.type_word("f")
        document = next_editor.get_buffer()
        self.pump_until(lambda: document.get_text(*document.get_bounds(), True) == "а",
                        "fresh destination input uses recovered RU source")
        self.pump_until(lambda: self.selection_text(Gdk.SELECTION_CLIPBOARD)
                        == "startup clipboard baseline", "recovered clipboard restoration")
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertEqual(process_start_time(daemon_pid), daemon_start)
        self.assertIsNone(self.harness.process.poll())
        for code in (KEY_TAB, KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_LEFTALT):
            self.assertFalse(self.key_is_down(code))

    def begin_blocked_tab_input(
            self, probe_events: list[tuple[int, int, int]]) -> tuple[float, int]:
        self.wait_event_loop_idle("source ingestion before held Tab")
        self.pump_for(0.01)
        if self.real_layout_bridge is not None:
            fixture = self.real_layout_bridge
            activations = fixture.switches
            self.addCleanup(fixture.release_result_loss)
            fixture.arm_result_loss()
        else:
            self.layout_shortcut.arm_blocked_activation()
            self.addCleanup(self.layout_shortcut.permit_activation.set)
        baseline = len(self.harness.relay.snapshot())
        started = time.monotonic()
        self.harness.send_key(KEY_TAB)
        if self.real_layout_bridge is not None:
            self.pump_until(fixture.switch_committed.is_set,
                            "committed desktop activation has no available reply")
            self.assertGreater(fixture.switch_committed_us, 0)
            self.assertLess(fixture.switch_committed_us, fixture.switch_deadline_us)
            self.assertEqual(fixture.switches, activations + 1)
            self.assertEqual(self.desktop_source_role(), 1)
            self.assertEqual(self.keyboard_group(), 1)
            self.assertTrue(fixture.hold_switch_reply)
            self.assertTrue(fixture.pending_invocations)
        else:
            self.pump_until(self.layout_shortcut.activation_started.is_set,
                            "real desktop activation holds the Tab edit")
            self.assertFalse(self.layout_shortcut.block_next_activation.is_set(),
                             "blocked activation admission was not consumed")
            self.assertEqual(self.layout_shortcut.activations, 0)
            self.assertFalse(self.layout_shortcut.permit_activation.is_set())
        self.assertNotIn("r37_tab_edit status=", self.harness.diagnostic())
        self.assertEqual(self.harness.relay.snapshot()[baseline:], [])

        self.harness.send_events(probe_events)
        probe = os.open(f"/proc/{self.harness.process.pid}/fd/0",
                        os.O_RDONLY | os.O_NONBLOCK)
        try:
            def probe_consumed():
                available = array.array("i", [0])
                fcntl.ioctl(probe, termios.FIONREAD, available, True)
                return available[0] == 0

            self.pump_until(probe_consumed, "busy macro consumed complete SYN probe",
                            timeout=0.1)
        finally:
            os.close(probe)
        if self.real_layout_bridge is not None:
            self.assertTrue(fixture.hold_switch_reply)
            self.assertEqual(fixture.switches, activations + 1)
        else:
            self.assertFalse(self.layout_shortcut.permit_activation.is_set())
            self.assertEqual(self.layout_shortcut.activations, 0)
        self.assertNotIn("r37_tab_edit status=", self.harness.diagnostic())
        self.assertEqual(self.harness.relay.snapshot()[baseline:], [],
                         "accepted probe escaped while the Tab edit was busy")
        return started, baseline

    def test_auto_tab_release_survives_non_nested_delete(self) -> None:
        self.prepare_word_editor()
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        next_editor = Gtk.TextView()
        self.window.remove(self.entry)
        box.pack_start(self.entry, False, False, 0)
        box.pack_start(next_editor, True, True, 0)
        self.window.add(box)
        self.window.show_all()
        self.entry.grab_focus()
        self.pump_until(self.entry.has_focus, "source Entry focus")
        snapshots: list[tuple[float, float, bool]] = []
        armed = {"value": True}
        delete_entered = threading.Event()
        delete_completed = threading.Event()

        def check_active():
            if not delete_entered.wait(timeout=2.0):
                return
            until = time.monotonic() + 1.05
            while time.monotonic() < until:
                tab = [(at, value) for at, kind, code, value
                       in self.harness.relay.snapshot()
                       if kind == EV_KEY and code == KEY_TAB]
                if [value for _, value in tab] == [1, 0]:
                    snapshots.append((tab[0][0], tab[1][0],
                                      delete_completed.is_set()))
                    return
                time.sleep(0.002)

        observer = threading.Thread(target=check_active, daemon=True)
        observer.start()

        def on_delete(_widget, _start, _end):
            if not armed["value"]:
                return
            armed["value"] = False

            delete_entered.set()
            time.sleep(1.25)
            delete_completed.set()

        self.entry.connect("delete-text", on_delete)
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn", "source word")
        started = time.monotonic()
        self.harness.send_key(KEY_TAB)
        self.pump_until(lambda: bool(snapshots), "bounded Tab lifecycle snapshot")
        observer.join(timeout=1.0)
        self.assertEqual(len(snapshots), 1, self.harness.diagnostic())
        press_at, release_at, callback_completed = snapshots[0]
        self.assertLessEqual(press_at - started, 1.0, self.harness.diagnostic())
        self.assertLessEqual(release_at - started, 1.0, self.harness.diagnostic())
        self.assertLessEqual(press_at, release_at)
        self.assertFalse(callback_completed,
                         "Tab was not released until GTK unblocked")
        self.pump_until(next_editor.has_focus, "original Tab focus after edit")
        self.assertEqual(self.entry.get_text(), "привет", self.harness.diagnostic())
        self.assertEqual(self.output_count(KEY_TAB, 1), 1)
        self.assertEqual(self.output_count(KEY_TAB, 0), 1)
        restoration_started = time.monotonic()
        restoration_observations = []

        def clipboard_restored():
            text = self.selection_text(Gdk.SELECTION_CLIPBOARD)
            restored = text == "startup clipboard baseline"
            restoration_observations.append({
                "elapsed_ms": round((time.monotonic() - restoration_started) * 1000),
                "owner": self.selection_owner("CLIPBOARD"),
                "baseline": restored,
                "none": text is None,
            })
            return restored

        self.pump_until(clipboard_restored,
                        "non-nested paste restores the exact clipboard baseline")
        self.assertEqual(self.external_selection_text(),
                         "startup clipboard baseline", self.harness.diagnostic())
        print(json.dumps({"non_nested_clipboard": restoration_observations}))
        self.harness.type_word("f")
        document = next_editor.get_buffer()
        self.pump_until(lambda: document.get_text(*document.get_bounds(), True) == "а",
                        "next physical character follows the corrected RU layout")
        for code in (KEY_TAB, KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_LEFTALT):
            self.assertFalse(self.key_is_down(code))

    def test_auto_tab_respects_learned_exclusion(self) -> None:
        self.prepare_word_editor()
        self.send_text_batch("ghbdtn ")
        self.pump_until(lambda: self.entry.get_text() == "привет ",
                        "automatic correction before exclusion")
        self.send_chord((KEY_LEFTCTRL,), KEY_Z)
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn ",
                        "undo records exclusion")
        exclusions = pathlib.Path("/etc/punto/undo_exclusions.txt")
        self.pump_until(lambda: exclusions.exists() and
                        "ghbdtn\n" in exclusions.read_text(),
                        "exclusion persisted")
        self.harness.send_key(KEY_LEFT)
        self.entry.set_text("")
        self.entry.set_position(0)
        self.set_desktop_layout(0)
        Gtk.Clipboard.get(Gdk.SELECTION_PRIMARY).clear()
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn",
                        "excluded source before Tab")
        self.harness.send_key(KEY_TAB)
        self.pump_until(lambda: self.output_count(KEY_TAB, 0) == 1,
                        "original excluded Tab release")
        self.assertEqual(self.entry.get_text(), "ghbdtn",
                         self.harness.diagnostic())

    def test_auto_tab_following_window_backpressures_without_loss(self) -> None:
        self.prepare_word_editor()
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        next_editor = Gtk.TextView()
        self.window.remove(self.entry)
        box.pack_start(self.entry, False, False, 0)
        box.pack_start(next_editor, True, True, 0)
        self.window.add(box)
        self.window.show_all()
        self.entry.grab_focus()
        self.pump_until(self.entry.has_focus, "source Entry focus")
        writer_done = threading.Event()
        writer_errors: list[BaseException] = []
        cycles = 1200
        following = [(kind, code, value) for _ in range(cycles)
                     for kind, code, value in
                     ((EV_KEY, KEY_LEFT, 1), (EV_SYN, SYN_REPORT, 0),
                      (EV_KEY, KEY_LEFT, 0), (EV_SYN, SYN_REPORT, 0))]

        def send_following():
            try:
                self.harness.send_events(following[4:])
            except BaseException as error:
                writer_errors.append(error)
            finally:
                writer_done.set()

        daemon_pid = self.harness.process.pid
        daemon_start = process_start_time(daemon_pid)
        activations = self.desktop_activation_count()
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn", "source word")
        started, baseline = self.begin_blocked_tab_input(following[:4])
        writer = threading.Thread(target=send_following, daemon=True)
        writer.start()
        try:
            self.pump_until(lambda: self.output_count(KEY_TAB, 0) == 1,
                            "saturated busy edit releases original Tab")
            tab_times = [at for at, kind, code, _ in self.harness.relay.snapshot()
                         if kind == EV_KEY and code == KEY_TAB]
            self.assertEqual(len(tab_times), 2)
            self.assertLessEqual(max(tab_times) - started, 1.0,
                                 self.harness.diagnostic())
            self.pump_until(writer_done.is_set, "saturated upstream writer drains")
            self.assertEqual(writer_errors, [])
            self.pump_until(lambda: self.output_count(KEY_LEFT, 0) == cycles,
                            "all accepted key releases forwarded", timeout=8.0)
            delivered = [(kind, code, value) for _, kind, code, value
                         in self.harness.relay.snapshot()[baseline:]]
            expected = [(EV_KEY, KEY_TAB, 1), (EV_SYN, SYN_REPORT, 0),
                        (EV_KEY, KEY_TAB, 0), (EV_SYN, SYN_REPORT, 0), *following]
            self.assertEqual(delivered, expected, self.harness.diagnostic())
            self.assertEqual(self.entry.get_text(), "ghbdtn",
                             self.harness.diagnostic())
            self.assertEqual(self.stats_fields()[1]["word_dispatches"], "0")
            if self.real_layout_bridge is not None:
                self.real_layout_bridge.release_result_loss()
            else:
                self.layout_shortcut.permit_activation.set()
            self.pump_until(lambda: self.desktop_activation_count() == activations + 1
                            and self.keyboard_group() == 1,
                            "cancelled activation actually settles before recovery")
            self.pump_until(next_editor.has_focus, "cancelled original Tab focus")
            self.harness.type_word("f")
            document = next_editor.get_buffer()
            self.pump_until(lambda: document.get_text(*document.get_bounds(), True) == "а",
                            "following physical input drains before fresh recovery")
            self.harness.send_key(KEY_SPACE)
            self.harness.type_word("hello")
            self.pump_until(lambda: document.get_text(*document.get_bounds(), True) == "а руддщ",
                            "fresh recovery uses actual settled RU source")
            self.harness.send_key(KEY_TAB)
            self.pump_until(lambda: document.get_text(*document.get_bounds(), True) == "а hello\t",
                            "fresh automatic correction recovers without restart")
            self.assertTrue(next_editor.has_focus())
            self.harness.type_word("f")
            self.pump_until(lambda: document.get_text(*document.get_bounds(), True) == "а hello\tf",
                            "next physical character follows recovered EN source")
            self.assertEqual(self.entry.get_text(), "ghbdtn")
            self.assertEqual(self.keyboard_group(), 0)
            self.assertEqual(self.desktop_source_role(), 0)
            self.assertFalse(document.get_has_selection())
            self.assertEqual(document.get_iter_at_mark(document.get_insert()).get_offset(),
                             len("а hello\tf"))
            self.assertEqual(self.output_count(KEY_TAB, 1), 2)
            self.assertEqual(self.output_count(KEY_TAB, 0), 2)
            self.assertEqual(self.stats_fields()[1]["word_dispatches"], "1")
            self.assertEqual(self.desktop_activation_count(), activations + 2)
            self.pump_until(lambda: self.selection_text(Gdk.SELECTION_CLIPBOARD)
                            == "startup clipboard baseline", "recovered clipboard restoration")
            self.assertEqual(self.harness.process.pid, daemon_pid)
            self.assertEqual(process_start_time(daemon_pid), daemon_start)
            self.assertIsNone(self.harness.process.poll())
            for code in (KEY_TAB, KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_LEFTALT):
                self.assertFalse(self.key_is_down(code))
        finally:
            if self.real_layout_bridge is not None:
                self.real_layout_bridge.release_result_loss()
            else:
                self.layout_shortcut.permit_activation.set()
            writer.join(timeout=1.0)

    def _capture_capacity_snapshot(
        self,
        entered: threading.Event,
        release: threading.Event,
        events: list[tuple[int, int, int]],
        baseline: int,
        minimum_committed_count: int,
        retained_frame_events: int,
        description: str,
    ) -> tuple[list[tuple[int, int, int]], threading.Event,
               threading.Thread, threading.Thread]:
        capacity = 4096
        queued_tab_release_events = 2
        capacity_marker = (
            f"[punto-e2e] macro_event_capacity reached={capacity}".encode())
        self.assertNotIn(capacity_marker, self.harness.stderr)
        drain_marker = (
            "[punto-e2e] macro_capacity_drained retained_frame_events="
            f"{retained_frame_events}\n".encode())
        self.assertNotIn(drain_marker, self.harness.stderr)
        prefix_count = capacity - queued_tab_release_events
        prefix = events[:prefix_count]
        tail = events[prefix_count:]
        self.assertEqual(len(prefix), prefix_count)
        self.assertTrue(tail, "capacity fixture must retain an upstream tail")

        allow_tail = threading.Event()
        sent = threading.Event()
        snapshots: list[list[tuple[int, int, int]]] = []
        errors: list[BaseException] = []

        def send_events() -> None:
            try:
                if not entered.wait(timeout=2.0):
                    raise RuntimeError("Tab edit did not enter the active window")
                self.harness.send_events(prefix)
                if not allow_tail.wait(timeout=EVENT_TIMEOUT):
                    raise RuntimeError("capacity snapshot did not release the upstream tail")
                self.harness.send_events(tail)
                sent.set()
            except BaseException as error:
                errors.append(error)

        def capture_snapshot() -> None:
            try:
                if not entered.wait(timeout=2.0):
                    raise RuntimeError("Tab edit did not enter the active window")
                deadline = time.monotonic() + EVENT_TIMEOUT
                while time.monotonic() < deadline:
                    if (capacity_marker in self.harness.stderr
                            and drain_marker in self.harness.stderr):
                        current = [(kind, code, value)
                                   for _, kind, code, value
                                   in self.harness.relay.snapshot()]
                        if len(current) - baseline >= minimum_committed_count:
                            # The relay reads one event at a time; a later 4ms
                            # incomplete-frame flush is not a capacity drain.
                            snapshots.append(current[:baseline + minimum_committed_count])
                            return
                    if self.harness.relay.error is not None:
                        raise self.harness.relay.error
                    time.sleep(0.002)
                raise RuntimeError("capacity output did not commit before the deadline")
            except BaseException as error:
                errors.append(error)
            finally:
                allow_tail.set()
                release.set()

        writer = threading.Thread(target=send_events, daemon=True)
        observer = threading.Thread(target=capture_snapshot, daemon=True)
        writer.start()
        observer.start()
        self.harness.send_key(KEY_TAB)
        self.pump_until(lambda: bool(snapshots) or bool(errors), description)
        self.assertFalse(
            errors, f"{description}: {errors!r}\n{self.harness.diagnostic()}")
        self.assertIn(capacity_marker, self.harness.stderr)
        self.assertIn(drain_marker, self.harness.stderr)
        return snapshots[0], sent, writer, observer

    def test_auto_tab_active_window_preserves_mixed_key_lifecycles(self) -> None:
        self.prepare_word_editor()
        entered = threading.Event()
        release = threading.Event()
        armed = {"value": True}
        following: list[tuple[int, int, int]] = []
        following.extend(((EV_KEY, KEY_TAB, 0), (EV_SYN, SYN_REPORT, 0)))
        following.extend(((EV_KEY, KEY_PAUSE, 1), (EV_SYN, SYN_REPORT, 0),
                          (EV_KEY, KEY_PAUSE, 2), (EV_SYN, SYN_REPORT, 0),
                          (EV_KEY, KEY_PAUSE, 0), (EV_SYN, SYN_REPORT, 0)))
        for _ in range(1020):
            following.extend(((EV_KEY, KEY_LEFT, 1), (EV_SYN, SYN_REPORT, 0),
                              (EV_KEY, KEY_LEFT, 0), (EV_SYN, SYN_REPORT, 0)))
        following.extend(((EV_KEY, KEY_RIGHT, 1), (EV_SYN, SYN_REPORT, 0),
                          (EV_KEY, KEY_RIGHT, 2), (EV_SYN, SYN_REPORT, 0),
                          (EV_KEY, KEY_RIGHT, 0), (EV_SYN, SYN_REPORT, 0),
                          (EV_KEY, KEY_LEFTSHIFT, 1), (EV_SYN, SYN_REPORT, 0),
                          (EV_KEY, KEY_LEFTSHIFT, 0), (EV_SYN, SYN_REPORT, 0)))

        def on_delete(_widget, _start, _end):
            if armed["value"]:
                armed["value"] = False
                entered.set()
                release.wait(timeout=EVENT_TIMEOUT + 1.0)

        self.entry.connect("delete-text", on_delete)
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn", "source word")
        baseline = len(self.harness.relay.snapshot())
        during, sent, writer, observer = self._capture_capacity_snapshot(
            entered, release, following[2:], baseline, 4095, 0,
            "active mixed-key window snapshot")
        snapshot = during[baseline:]
        self.assertEqual(snapshot, [
            (EV_KEY, KEY_TAB, 1), (EV_SYN, SYN_REPORT, 0),
            (EV_KEY, KEY_TAB, 0), (EV_SYN, SYN_REPORT, 0),
            *(event for event in following[2:4096]
              if event[:2] != (EV_KEY, KEY_PAUSE))])
        self.assertGreaterEqual(len(snapshot), 4000,
                                "active window never approached its quota")
        rights = [(code, value) for kind, code, value in snapshot
                  if kind == EV_KEY and code == KEY_RIGHT]
        self.assertEqual(sum(value == 1 for _, value in rights),
                         sum(value == 0 for _, value in rights),
                         "active window stranded KEY_RIGHT before its accepted release")
        self.assertEqual(snapshot[-1], (EV_SYN, SYN_REPORT, 0))
        self.pump_until(sent.is_set, "upstream mixed sequence sent", timeout=8.0)
        self.pump_until(lambda: self.output_count(KEY_LEFTSHIFT, 0) == 1,
                        "mixed sequence flushed", timeout=8.0)
        self.assertEqual(self.output_count(KEY_PAUSE), 0,
                         "active Tab leaked Pause to the native output")
        expected = [(EV_KEY, KEY_TAB, 1), (EV_SYN, SYN_REPORT, 0),
                    *(event for event in following
                      if event[:2] != (EV_KEY, KEY_PAUSE))]
        self.pump_until(lambda: len(self.harness.relay.snapshot()) >= baseline + len(expected),
                        "complete mixed stream including final SYN")
        delivered = [(kind, code, value) for _, kind, code, value
                     in self.harness.relay.snapshot()[baseline:]]
        self.assertEqual(delivered, expected, self.harness.diagnostic())
        self.assertEqual(self.entry.get_text(), "привет", self.harness.diagnostic())
        assert self.harness.process.stdin is not None
        self.harness.process.stdin.close()
        self.pump_until(lambda: self.harness.process.poll() is not None,
                        "complete mixed sequence EOF")
        self.assertEqual(self.harness.process.returncode, 0,
                         self.harness.diagnostic())
        writer.join(timeout=1.0)
        observer.join(timeout=1.0)

    def test_auto_tab_active_window_never_splits_syn_frame(self) -> None:
        self.prepare_word_editor()
        entered = threading.Event()
        release = threading.Event()
        armed = {"value": True}
        following: list[tuple[int, int, int]] = []
        for _ in range(1022):
            following.extend(((EV_KEY, KEY_LEFT, 1), (EV_SYN, SYN_REPORT, 0),
                              (EV_KEY, KEY_LEFT, 0), (EV_SYN, SYN_REPORT, 0)))
        following.extend(((EV_KEY, KEY_RIGHT, 1), (EV_KEY, KEY_RIGHT, 2),
                          (EV_KEY, KEY_RIGHT, 0), (EV_KEY, KEY_LEFTSHIFT, 1),
                          (EV_SYN, SYN_REPORT, 0),
                          (EV_KEY, KEY_LEFTSHIFT, 0), (EV_SYN, SYN_REPORT, 0)))

        def on_delete(_widget, _start, _end):
            if armed["value"]:
                armed["value"] = False
                entered.set()
                release.wait(timeout=EVENT_TIMEOUT + 1.0)

        self.entry.connect("delete-text", on_delete)
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn", "source word")
        baseline = len(self.harness.relay.snapshot())
        snapshot, sent, writer, observer = self._capture_capacity_snapshot(
            entered, release, following, baseline, 4097, 1,
            "active frame-boundary snapshot")
        self.assertGreaterEqual(len(snapshot[baseline:]), 4000,
                                "active window never approached its quota")
        self.assertEqual(snapshot[baseline:], [
            (EV_KEY, KEY_TAB, 1), (EV_SYN, SYN_REPORT, 0),
            (EV_KEY, KEY_TAB, 0), (EV_SYN, SYN_REPORT, 0), *following[:4093]])
        self.assertEqual(snapshot[-1], (EV_SYN, SYN_REPORT, 0),
                         "active quota split a committed SYN frame")
        self.pump_until(sent.is_set, "upstream frame-boundary sequence sent",
                        timeout=8.0)
        self.pump_until(lambda: self.output_count(KEY_LEFTSHIFT, 0) == 1,
                        "frame-boundary sequence flushed", timeout=8.0)
        expected = [(EV_KEY, KEY_TAB, 1), (EV_SYN, SYN_REPORT, 0),
                    (EV_KEY, KEY_TAB, 0), (EV_SYN, SYN_REPORT, 0), *following]
        self.pump_until(lambda: len(self.harness.relay.snapshot()) >= baseline + len(expected),
                        "complete frame-boundary stream including final SYN")
        delivered = [(kind, code, value) for _, kind, code, value
                     in self.harness.relay.snapshot()[baseline:]]
        self.assertEqual(delivered, expected, self.harness.diagnostic())
        assert self.harness.process.stdin is not None
        self.harness.process.stdin.close()
        self.pump_until(lambda: self.harness.process.poll() is not None,
                        "complete frame-boundary EOF")
        self.assertEqual(self.harness.process.returncode, 0,
                         self.harness.diagnostic())
        writer.join(timeout=1.0)
        observer.join(timeout=1.0)

    def test_auto_tab_repeated_corrections_preserve_release_and_next_input(self) -> None:
        self.prepare_word_editor()
        daemon_pid = self.harness.process.pid
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        next_editor = Gtk.TextView()
        self.window.remove(self.entry)
        box.pack_start(self.entry, False, False, 0)
        box.pack_start(next_editor, True, True, 0)
        self.window.add(box)
        self.window.show_all()
        self.entry.grab_focus()
        self.pump_until(self.entry.has_focus, "source Entry focus")

        def completed_tab_releases() -> int:
            events = [(kind, code, value) for _, kind, code, value
                      in self.harness.relay.snapshot()]
            return sum(event == (EV_KEY, KEY_TAB, 0) and
                       events[index + 1] == (EV_SYN, SYN_REPORT, 0)
                       for index, event in enumerate(events[:-1]))

        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn", "source word")
        self.harness.send_key(KEY_TAB)
        self.pump_until(next_editor.has_focus, "original Tab focus transfer")
        self.pump_until(lambda: self.entry.get_text() == "привет",
                        "first automatic correction settled")
        self.pump_until(lambda: self.stats_fields()[1]["word_dispatches"] == "1",
                        "first operation dispatched once")
        self.pump_until(lambda: completed_tab_releases() >= 1,
                        "first original Tab release frame delivered")
        self.assertEqual(self.output_count(KEY_TAB, 1), 1)
        self.assertEqual(self.output_count(KEY_TAB, 0), 1)
        self.assertEqual(completed_tab_releases(), 1)
        self.entry.set_text("")
        self.entry.set_position(0)
        self.set_desktop_layout(0)
        self.entry.grab_focus()
        self.pump_until(self.entry.has_focus, "fresh source focus")
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn", "fresh word")
        self.harness.send_key(KEY_TAB)
        self.pump_until(lambda: self.entry.get_text() == "привет",
                        "fresh automatic correction")
        self.pump_until(lambda: self.stats_fields()[1]["word_dispatches"] == "2",
                        "fresh operation dispatched once")
        self.pump_until(lambda: completed_tab_releases() >= 2,
                        "second original Tab release frame delivered")
        self.pump_until(next_editor.has_focus, "second Tab destination focus")
        self.assertEqual(self.output_count(KEY_TAB, 1), 2)
        self.assertEqual(self.output_count(KEY_TAB, 0), 2)
        self.assertEqual(completed_tab_releases(), 2)
        self.pump_until(lambda: self.selection_text(Gdk.SELECTION_CLIPBOARD)
                        == "startup clipboard baseline", "repeated clipboard restoration")
        for code in (KEY_TAB, KEY_LEFTCTRL, KEY_RIGHTCTRL, KEY_LEFTSHIFT,
                     KEY_RIGHTSHIFT, KEY_LEFTALT, KEY_RIGHTALT,
                     KEY_LEFTMETA, KEY_RIGHTMETA):
            self.assertFalse(self.key_is_down(code), f"released key {code}")
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertIsNone(self.harness.process.poll())
        self.harness.type_word("f")
        document = next_editor.get_buffer()
        self.pump_until(lambda: document.get_text(*document.get_bounds(), True) == "а",
                        "next physical key reaches the destination in RU")
        self.assertEqual(self.entry.get_text(), "привет")

    def test_auto_tab_incomplete_following_frame_reports_eof(self) -> None:
        self.prepare_word_editor()
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn", "source word")
        self.harness.send_key(KEY_TAB)
        self.harness.send_events([(EV_KEY, KEY_LEFT, 1)])
        assert self.harness.process.stdin is not None
        self.harness.process.stdin.close()
        self.pump_until(lambda: self.harness.process.poll() is not None,
                        "incomplete following frame reports EOF")
        self.assertEqual(self.harness.process.returncode, 3,
                         self.harness.diagnostic())
        self.assertEqual(self.output_count(KEY_TAB, 1), 1)
        self.assertEqual(self.output_count(KEY_TAB, 0), 1)

    def test_auto_tab_active_incomplete_frame_eof_exits(self) -> None:
        self.prepare_word_editor()
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn", "source word")
        started, baseline = self.begin_blocked_tab_input([(EV_SYN, SYN_REPORT, 0)])
        try:
            self.harness.send_events([(EV_KEY, KEY_LEFT, 1)])
            assert self.harness.process.stdin is not None
            self.harness.process.stdin.close()
            self.pump_until(lambda: self.harness.process.poll() is not None,
                            "busy incomplete frame EOF exits", timeout=3.0)
            self.assertEqual(self.harness.process.returncode, 3,
                             self.harness.diagnostic())
            self.assertIn("stdin closed with an incomplete input frame",
                          self.harness.diagnostic())
            self.pump_until(lambda: self.output_count(KEY_LEFT, 1) == 1,
                            "accepted incomplete key reaches native output")
            delivered = [(kind, code, value) for _, kind, code, value
                         in self.harness.relay.snapshot()[baseline:]]
            self.assertEqual(delivered,
                             [(EV_KEY, KEY_TAB, 1), (EV_SYN, SYN_REPORT, 0),
                              (EV_KEY, KEY_TAB, 0), (EV_SYN, SYN_REPORT, 0),
                              (EV_SYN, SYN_REPORT, 0), (EV_KEY, KEY_LEFT, 1)],
                             self.harness.diagnostic())
            tab_times = [at for at, kind, code, _ in self.harness.relay.snapshot()
                         if kind == EV_KEY and code == KEY_TAB]
            self.assertLessEqual(max(tab_times) - started, 1.0,
                                 self.harness.diagnostic())
            self.assertEqual(self.entry.get_text(), "ghbdtn", self.harness.diagnostic())
            self.assertNotIn("r37_tab_edit status=2", self.harness.diagnostic())
            self.assertEqual(self.output_count(KEY_TAB, 1), 1)
            self.assertEqual(self.output_count(KEY_TAB, 0), 1)
            self.assertEqual(self.output_count(KEY_LEFT, 1), 1)
            self.assertEqual(self.output_count(KEY_LEFT, 0), 0)
            self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD),
                             "startup clipboard baseline")
        finally:
            if self.real_layout_bridge is not None:
                self.real_layout_bridge.release_result_loss()
            else:
                self.layout_shortcut.permit_activation.set()

    def test_auto_tab_partial_following_event_reports_eof(self) -> None:
        self.prepare_word_editor()
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn", "source word")
        self.harness.send_key(KEY_TAB)
        assert self.harness.process.stdin is not None
        os.write(self.harness.process.stdin.fileno(), b"\0")
        self.harness.process.stdin.close()
        self.pump_until(lambda: self.harness.process.poll() is not None,
                        "partial following event reports EOF")
        self.assertEqual(self.harness.process.returncode, 3,
                         self.harness.diagnostic())
        self.assertEqual(self.output_count(KEY_TAB, 1), 1)
        self.assertEqual(self.output_count(KEY_TAB, 0), 1)

    def test_auto_tab_pre_ready_saturation_preserves_input(self) -> None:
        self.prepare_word_editor()
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn", "source word")
        gtk_blocked = threading.Event()
        writer_done = threading.Event()
        writer_elapsed: list[float] = []
        cycles = 2200

        def block_gtk():
            gtk_blocked.set()
            time.sleep(1.3)
            return False

        def send_while_blocked():
            if not gtk_blocked.wait(timeout=2.0):
                return
            started = time.monotonic()
            events = [(EV_KEY, KEY_TAB, 1), (EV_SYN, SYN_REPORT, 0),
                      (EV_KEY, KEY_TAB, 0), (EV_SYN, SYN_REPORT, 0)]
            for _ in range(cycles):
                events.extend(((EV_KEY, KEY_LEFT, 1), (EV_SYN, SYN_REPORT, 0),
                               (EV_KEY, KEY_LEFT, 0), (EV_SYN, SYN_REPORT, 0)))
            self.harness.send_events(events)
            assert self.harness.process.stdin is not None
            self.harness.process.stdin.close()
            writer_elapsed.append(time.monotonic() - started)
            writer_done.set()

        writer = threading.Thread(target=send_while_blocked, daemon=True)
        writer.start()
        GLib.idle_add(block_gtk)
        self.pump_until(writer_done.is_set, "pre-READY upstream writer drains",
                        timeout=5.0)
        self.pump_until(lambda: self.output_count(KEY_LEFT, 0) == cycles,
                        "all pre-READY accepted releases delivered", timeout=8.0)
        self.pump_until(lambda: self.harness.process.poll() is not None,
                        "complete pre-READY stream exits")
        self.assertEqual(self.harness.process.returncode, 0,
                         self.harness.diagnostic())
        self.assertEqual(self.entry.get_text(), "ghbdtn",
                         self.harness.diagnostic())
        self.assertEqual(self.output_count(KEY_TAB, 1), 1)
        self.assertEqual(self.output_count(KEY_TAB, 0), 1)
        self.assertEqual(self.output_count(KEY_LEFT, 1), cycles)
        self.assertLess(writer_elapsed[0], 0.7,
                        "capacity fallback should not wait for the blocked editor")
        self.assertIn("r37_tab_fallback deadline_or_capacity",
                      self.harness.diagnostic())
        self.assertNotIn("r38_tab_quiescent", self.harness.diagnostic())
        writer.join(timeout=1.0)

    def test_auto_entry_tab_reverse_ru_to_en(self) -> None:
        self.prepare_word_editor()
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        next_editor = Gtk.TextView()
        self.window.remove(self.entry)
        box.pack_start(self.entry, False, False, 0)
        box.pack_start(next_editor, True, True, 0)
        self.window.add(box)
        self.window.show_all()
        self.entry.grab_focus()
        self.pump_until(self.entry.has_focus, "source Entry focus")
        self.set_desktop_layout(1)
        self.harness.type_word("hello")
        self.pump_until(lambda: self.entry.get_text() == "руддщ", "RU wrong-layout source")
        self.harness.send_key(KEY_TAB)
        self.pump_until(next_editor.has_focus, "original Tab focus transfer")
        self.assertEqual(self.entry.get_text(), "hello", self.harness.diagnostic())
        self.harness.type_word("f")
        document = next_editor.get_buffer()
        self.pump_until(lambda: document.get_text(*document.get_bounds(), True) == "f",
                        "first key in corrected English layout")
        self.assertEqual(self.output_count(KEY_TAB, 1), 1)
        self.pump_until(lambda: self.output_count(KEY_TAB, 0) == 1,
                        "original Tab release")
        self.pump_until(lambda: self.selection_text(Gdk.SELECTION_CLIPBOARD)
                        == "startup clipboard baseline", "reverse Entry clipboard restoration")

    def test_auto_textview_tab_reverse_ru_to_en(self) -> None:
        self.prepare_word_editor()
        self.window.remove(self.entry)
        editor = Gtk.TextView()
        self.window.add(editor)
        self.window.show_all()
        editor.grab_focus()
        self.pump_until(editor.has_focus, "TextView focus")
        document = editor.get_buffer()

        def visible_text():
            return document.get_text(*document.get_bounds(), True)

        self.set_desktop_layout(1)
        self.harness.type_word("hello")
        self.pump_until(lambda: visible_text() == "руддщ", "RU wrong-layout source")
        self.harness.send_key(KEY_TAB)
        self.pump_until(lambda: visible_text() == "hello\t",
                        "TextView correction and literal Tab")
        self.assertEqual(self.output_count(KEY_TAB, 1), 1)
        self.pump_until(lambda: self.output_count(KEY_TAB, 0) == 1,
                        "original TextView Tab release")
        self.harness.type_word("f")
        self.pump_until(lambda: visible_text() == "hello\tf",
                        "first key in corrected English layout")
        self.pump_until(lambda: self.selection_text(Gdk.SELECTION_CLIPBOARD)
                        == "startup clipboard baseline", "reverse TextView clipboard restoration")

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

    def test_mixed_history_common_symbols_do_not_oscillate_layout(self) -> None:
        self.prepare_word_editor()
        activations = self.desktop_activation_count()
        clipboard_owner = self.selection_owner("CLIPBOARD")
        daemon_pid = self.harness.process.pid
        daemon_start = process_start_time(daemon_pid)
        self.send_text_batch("ghbdtn foo bar")
        self.pump_until(lambda: self.entry.get_text() == "привет foo bar",
                        "mixed-layout history correction")
        self.wait_word_dispatches(1, "mixed history completes once under native desktop authority")
        # RU prefix and EN suffix need two real hops; common spaces inherit
        # the preceding group instead of causing additional transitions.
        self.assertEqual(self.desktop_activation_count(), activations + 2)
        self.assertEqual(self.entry.get_position(), len("привет foo bar"))
        self.assertEqual(self.entry.get_selection_bounds(), ())
        self.assertEqual(self.desktop_source_role(), 0)
        self.assertEqual(self.keyboard_group(), 0)
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "1")
        self.harness.type_word("a")
        self.pump_until(lambda: self.entry.get_text() == "привет foo bara",
                        "next physical key follows final EN source")
        self.pump_for(0.08)
        self.assertEqual(self.desktop_activation_count(), activations + 2,
                         "no late layout hop after completion")
        self.assertEqual(self.selection_owner("CLIPBOARD"), clipboard_owner)
        self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD), "startup clipboard baseline")
        for key in (KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_LEFTALT):
            self.assertFalse(self.key_is_down(key))
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertEqual(process_start_time(daemon_pid), daemon_start)

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
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn",
                        "first physical word delivered before boundary probe")
        ready, release = self.arm_keyboard_observation(hold=True)
        self.harness.send_key(KEY_SPACE)
        self.pump_until(
            lambda: ready.exists()
            and (marker := ready.read_text()).endswith("\n")
            and marker.strip().isdecimal(),
            "complete identity of the held keyboard reply",
        )
        held_request = int(ready.read_text())
        self.assertGreater(held_request, 0, "held reply belongs to a keyboard worker")
        self.pump_until(
            lambda: f"[punto-e2e] automatic_boundary id={held_request}\n"
            in self.harness.diagnostic(),
            "held reply matches the actual automatic boundary request",
        )
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn ",
                        "first physical word delivered before layout change")
        self.set_desktop_layout(1)
        self.send_text_batch("world ")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn цщкдв ", "second physical word in Russian layout")
        release.touch(mode=0o600)
        self.pump_until(
            lambda: f"[punto-e2e] keyboard_completed id={held_request} group=0 "
            in self.harness.diagnostic(),
            "the same boundary reply completed after release",
        )
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

    def prepare_keyboard_punctuation_word(self, source: str = ";tcn") -> int:
        from unittest.mock import patch

        self.prepare_word_editor()
        self.assertEqual(ipc_request(b"SET_STATUS 0\n"), b"OK DISABLED\n")
        with patch.dict(LETTER_CODES, {";": 39}):
            self.send_text_batch(source)
        self.pump_until(lambda: self.entry.get_text() == source,
                        "physical keyboard-mapped punctuation prefix")
        return self.harness.process.pid

    def test_manual_word_layout_includes_proven_keyboard_punctuation_prefix(self) -> None:
        daemon_pid = self.prepare_keyboard_punctuation_word()
        self.harness.send_key(KEY_PAUSE)
        self.pump_until(lambda: self.entry.get_text() == "жест",
                        "two proven native units include the punctuation key")
        self.assertEqual(self.entry.get_position(), 4)
        self.assertEqual(self.entry.get_selection_bounds(), ())
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "1")
        self.assertEqual(self.keyboard_group(), 1)
        self.harness.type_word("f")
        self.pump_until(lambda: self.entry.get_text() == "жеста", "next key uses RU after prefix correction")
        self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD), "startup clipboard baseline")
        self.assertEqual(self.output_count(KEY_PAUSE), 0)
        for key in (KEY_LEFTCTRL, KEY_LEFTSHIFT):
            self.assertFalse(self.key_is_down(key))
        self.assertEqual(self.harness.process.pid, daemon_pid)

    def test_manual_word_layout_includes_multiple_proven_prefix_keys_after_word(self) -> None:
        daemon_pid = self.prepare_keyboard_punctuation_word("hello ;;tcn ")
        self.harness.send_key(KEY_PAUSE)
        self.pump_until(lambda: self.entry.get_text() == "hello жжест ",
                        "bounded prefix steps include both keys without selecting the previous word")
        self.assertEqual(self.entry.get_position(), len("hello жжест "))
        self.assertEqual(self.entry.get_selection_bounds(), ())
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "1")
        self.assertEqual(self.keyboard_group(), 1)
        self.harness.type_word("f")
        self.pump_until(lambda: self.entry.get_text() == "hello жжест а", "next key after multiple prefix steps")
        self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD), "startup clipboard baseline")
        self.assertEqual(self.output_count(KEY_PAUSE), 0)
        for key in (KEY_LEFTCTRL, KEY_LEFTSHIFT):
            self.assertFalse(self.key_is_down(key))
        self.assertEqual(self.harness.process.pid, daemon_pid)

    def assert_keyboard_prefix_refusal_recovers(self, mode: str, prefix: str = ";") -> None:
        daemon_pid = self.prepare_keyboard_punctuation_word(prefix + "tcn")
        text = "before tcn" if mode == "mismatch" else "before " + prefix + "tcn"
        self.entry.set_text(text)
        self.entry.set_position(-1)
        native_units = []
        native_controls = []
        late_selection = []

        def native_left(_editor, event):
            modifiers = Gdk.ModifierType.SHIFT_MASK
            if event.keyval != Gdk.KEY_Left or event.state & modifiers != modifiers:
                return False
            native_units.append(self.entry.get_chars(*self.entry.get_selection_bounds())
                                if self.entry.get_selection_bounds() else "")
            native_controls.append(bool(event.state & Gdk.ModifierType.CONTROL_MASK))
            if len(native_units) != 2 or mode == "mismatch":
                return False
            if mode == "foreign":
                self.entry.select_region(0, 2)
            elif mode == "timeout":
                def deliver_late_range():
                    self.entry.select_region(7, len(text))
                    late_selection.append(True)
                    return False
                GLib.timeout_add(450, deliver_late_range)
            return True

        handler = self.entry.connect("key-press-event", native_left)
        self.addCleanup(self.entry.disconnect, handler)
        attempts = self.harness.diagnostic().count("Word edit dispatch status=")
        activations = self.desktop_activation_count()
        started = time.monotonic()
        self.harness.send_key(KEY_PAUSE)
        self.pump_until(lambda: self.harness.diagnostic().count(
            "Word edit dispatch status=") > attempts, "bounded prefix refusal")
        self.assertEqual(len(native_units), 2, "one initial unit and one bounded prefix probe")
        self.assertEqual(native_controls, [True, False], "prefix selection is one character, not another word unit")
        self.assertEqual(native_units[1], "tcn", "first native unit matches the hinted suffix")
        self.assertEqual(self.entry.get_text(), text)
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "0")
        self.assertEqual(self.keyboard_group(), 0)
        self.assertEqual(self.desktop_activation_count(), activations)
        if mode == "foreign":
            self.assertEqual(self.entry.get_selection_bounds(), (0, 2))
            self.assertEqual(self.selection_text(Gdk.SELECTION_PRIMARY), "be")
        else:
            self.assertEqual(self.entry.get_selection_bounds(), ())
            self.assertEqual(self.entry.get_position(), len(text))
        if mode == "mismatch":
            self.assertIn("rejection_stage=current_word_prefix_range", self.harness.diagnostic())
        if mode == "timeout":
            self.assertGreaterEqual(time.monotonic() - started, .3)
            self.pump_until(lambda: bool(late_selection), "late native selection arrives after refusal")
            self.pump_for(.05)
            self.assertEqual(len(native_units), 2, "late selection cannot queue the remaining prefix step")
            self.assertEqual(self.entry.get_selection_bounds(), (7, len(text)))
            self.assertEqual(self.entry.get_text(), text)
            self.assertEqual(self.stats_fields()[1]["word_dispatches"], "0")
        self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD), "startup clipboard baseline")
        self.entry.set_text("before ghbdtn after")
        self.entry.select_region(7, 13)
        self.pump_until(lambda: self.selection_text(Gdk.SELECTION_PRIMARY) == "ghbdtn",
                        "fresh user range after prefix refusal")
        self.send_chord((KEY_LEFTSHIFT,))
        self.pump_until(lambda: self.entry.get_text() == "before привет after",
                        "fresh selection succeeds after refused prefix without restart")
        self.assertEqual(self.entry.get_selection_bounds(), ())
        self.assertEqual(self.entry.get_position(), 13)
        self.harness.type_word("f")
        self.pump_until(lambda: self.entry.get_text() == "before привета after",
                        "next key preserves prefix and suffix after recovery")
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "1")
        self.assertEqual(self.output_count(KEY_PAUSE), 0)
        for key in (KEY_LEFTCTRL, KEY_LEFTSHIFT):
            self.assertFalse(self.key_is_down(key))
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertIsNone(self.harness.process.poll())

    def test_keyboard_prefix_stale_hint_refuses_foreign_prefix_and_recovers(self) -> None:
        self.assert_keyboard_prefix_refusal_recovers("mismatch")

    def test_keyboard_prefix_unchanged_native_range_refuses_and_recovers(self) -> None:
        self.assert_keyboard_prefix_refusal_recovers("unchanged")

    def test_keyboard_prefix_new_user_range_is_not_collapsed(self) -> None:
        self.assert_keyboard_prefix_refusal_recovers("foreign")

    def test_keyboard_prefix_late_native_range_cannot_resume_old_mutation(self) -> None:
        self.assert_keyboard_prefix_refusal_recovers("timeout")

    def test_multiple_keyboard_prefix_stops_before_new_user_range_is_changed(self) -> None:
        self.assert_keyboard_prefix_refusal_recovers("foreign", ";;")

    def test_multiple_keyboard_prefix_stops_when_first_step_does_not_grow(self) -> None:
        self.assert_keyboard_prefix_refusal_recovers("unchanged", ";;")

    def test_multiple_keyboard_prefix_late_first_step_cannot_queue_remaining_step(self) -> None:
        self.assert_keyboard_prefix_refusal_recovers("timeout", ";;")

    def test_clear_exclusions_restores_automatic_correction(self) -> None:
        self.prepare_word_editor()
        self.send_text_batch("ghbdtn ")
        self.pump_until(lambda: self.entry.get_text() == "привет ",
                        "automatic correction before learning reset")
        self.send_chord((KEY_LEFTCTRL,), KEY_Z)
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn ",
                        "undo records learned exclusion")
        exclusions = pathlib.Path("/etc/punto/undo_exclusions.txt")
        self.pump_until(lambda: exclusions.exists() and "ghbdtn\n" in exclusions.read_text(),
                        "learned exclusion persisted before reset")
        daemon_epoch = self.stats_fields()[1]["daemon_epoch"]
        response = ipc_request(b"CLEAR_EXCLUSIONS\n")
        self.assertRegex(
            response,
            rb"^OK EXCLUSIONS SCHEDULED [1-9][0-9]* [1-9][0-9]*\n$",
        )
        self.assertEqual(response.decode("ascii").split()[3], daemon_epoch)
        self.pump_until(lambda: not exclusions.exists() or not exclusions.read_text(),
                        "cleared exclusion store persisted")
        self.harness.send_key(KEY_LEFT)
        self.entry.set_text("")
        self.entry.set_position(0)
        self.set_desktop_layout(0)
        Gtk.Clipboard.get(Gdk.SELECTION_PRIMARY).clear()
        self.pump_until(
            lambda: self.selection_text(Gdk.SELECTION_PRIMARY) is None,
            "previous correction selection released after learning reset",
        )
        self.send_text_batch("ghbdtn ")
        self.pump_until(lambda: self.entry.get_text() == "привет ",
                        "automatic correction restored after learning reset")

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
        self.assertEqual(self.entry.get_position(), len(source + " " + corrected + " "))
        self.assertEqual(self.entry.get_selection_bounds(), ())
        self.assertEqual(self.keyboard_group(), 1)
        self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD), "startup clipboard baseline")
        self.assertEqual(self.output_count(KEY_PAUSE), 0)
        for key in (KEY_LEFTCTRL, KEY_LEFTSHIFT):
            self.assertFalse(self.key_is_down(key))
        self.harness.type_word("f")
        self.pump_until(lambda: self.entry.get_text() == source + " " + corrected + " а",
                        "manual correction of learned exclusion establishes next RU key")
        self.harness.stop()
        self.entry.set_text("")
        self.set_desktop_layout(0)
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
        self.set_desktop_layout(0)
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
            # A layout mismatch alone permits native undo in the same editor.
            self.window.remove(self.entry)
            self.entry.destroy()
            self.entry = Gtk.Entry()
            self.entry.connect("key-press-event", self._record_key, "press")
            self.entry.connect("key-release-event", self._record_key, "release")
            self.entry.set_text("other")
            self.entry.set_position(-1)
            self.window.add(self.entry)
            self.entry.show()
            self.entry.grab_focus()
            self.pump_until(self.entry.has_focus, "new editor rejects old undo authority")
            self.entry.set_position(-1)
            self.assertEqual(self.entry.get_selection_bounds(), ())
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
        self.set_desktop_layout(0)
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

    def test_gui_word_case_does_not_require_terminal_insert_binding(self) -> None:
        self.prepare_word_editor()
        subprocess.run(["xmodmap", "-display", self.x11.display,
                        "-e", "keycode 118 = F12 F12 F12 F12"],
                       check=True, timeout=3)
        self.harness.type_word("hELLo")
        self.pump_until(lambda: self.entry.get_text() == "hELLo", "GUI source with remapped Insert")
        self.harness.hotkey(KEY_LEFTCTRL, repeat=False)
        self.pump_until(lambda: self.entry.get_text() == "HellO",
                        "GUI case editing ignores the unused terminal paste key")
        self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD), "startup clipboard baseline")

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
        self.key_events.clear()
        self.window.connect("key-press-event", self._record_key, "press")
        self.window.connect("key-release-event", self._record_key, "release")
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
        self.harness.type_word("f")
        self.pump_until(lambda: "привета" in terminal.get_text_format(Vte.Format.TEXT),
                        "terminal next key uses the corrected desktop layout")
        self.harness.send_key(KEY_ENTER)
        self.pump_until(lambda: received.exists() and received.read_bytes() == "привета\n".encode(),
                        "exact corrected canonical PTY input")
        self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD), "startup clipboard baseline")

    def test_vte_remapped_insert_refuses_before_delete_and_recovers(self) -> None:
        terminal, received = self.prepare_vte_line_editor()
        daemon_pid = self.harness.process.pid
        subprocess.run(["xmodmap", "-display", self.x11.display,
                        "-e", "keycode 118 = F12 F12 F12 F12"],
                       check=True, timeout=3)
        self.harness.type_word("hELLo")
        self.pump_until(lambda: terminal.get_text_format(Vte.Format.TEXT).strip().splitlines()[-1] == "hELLo",
                        "terminal source with unsupported paste key")
        keys = []
        terminal.connect("key-press-event", lambda _widget, event:
                         keys.append(Gdk.keyval_name(event.keyval)) or False)
        self.send_chord((KEY_LEFTCTRL,))
        self.pump_until(lambda: "rejection_stage=terminal_paste_keymap" in self.harness.diagnostic(),
                        "terminal-specific Insert admission refuses")
        self.assertEqual(terminal.get_text_format(Vte.Format.TEXT).strip().splitlines()[-1], "hELLo")
        self.assertNotIn("BackSpace", keys)
        self.assertFalse(received.exists())
        self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD), "startup clipboard baseline")
        subprocess.run(["xmodmap", "-display", self.x11.display,
                        "-e", "keycode 118 = Insert Insert Insert Insert"],
                       check=True, timeout=3)
        self.send_chord((KEY_LEFTCTRL,))
        self.pump_until(lambda: self.stats_fields()[1]["word_dispatches"] == "1"
                        and terminal.get_text_format(Vte.Format.TEXT).strip().splitlines()[-1] == "HellO",
                        "restored terminal binding works without daemon restart")
        self.harness.send_key(KEY_ENTER)
        self.pump_until(lambda: received.exists() and received.read_bytes() == b"HellO\n",
                        "exact recovered canonical terminal input")
        self.assertEqual(self.harness.process.pid, daemon_pid)

    def test_vte_repeated_layout_case_and_undo_preserve_visible_history(self) -> None:
        terminal, received = self.prepare_vte_line_editor()
        daemon_pid = self.harness.process.pid
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: "ghbdtn" in terminal.get_text_format(Vte.Format.TEXT),
                        "terminal source for repeated correction")
        for number, expected in enumerate(("привет", "ghbdtn", "привет"), 1):
            self.harness.send_key(KEY_PAUSE)
            self.pump_until(lambda: int(self.stats_fields()[1]["word_dispatches"]) == number
                            and terminal.get_text_format(Vte.Format.TEXT).strip().splitlines()[-1] == expected,
                            f"terminal reversible correction {number}")
        self.send_chord((KEY_LEFTCTRL,))
        self.pump_until(lambda: "ПРИВЕТ" in terminal.get_text_format(Vte.Format.TEXT),
                        "terminal case correction uses visible history")
        self.send_chord((KEY_LEFTCTRL,), KEY_Z)
        self.pump_until(lambda: int(self.stats_fields()[1]["word_dispatches"]) == 5,
                        "terminal exact inverse undo")
        self.assertEqual(self.output_count(KEY_Z), 0,
                         "Punto undo must not send Ctrl+Z to the PTY")
        self.harness.type_word("f")
        self.harness.send_key(KEY_ENTER)
        self.pump_until(lambda: received.exists() and received.read_bytes() == "привета\n".encode(),
                        "repeated transforms and inverse reach exact PTY bytes")
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertIsNone(self.harness.process.poll())
        self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD), "startup clipboard baseline")

    def test_vte_function_key_invalidates_history_and_fresh_word_recovers(self) -> None:
        terminal, received = self.prepare_vte_line_editor()
        daemon_pid = self.harness.process.pid
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: "ghbdtn" in terminal.get_text_format(Vte.Format.TEXT),
                        "terminal word before function-key invalidation")
        self.harness.send_key(59)  # Physical F1 can change a terminal editor's state.
        self.wait_event_loop_idle("function key committed before stale Pause")
        self.pump_for(0.03)
        rejected = self.harness.diagnostic().count("Word edit dispatch status=0")
        before_keys = list(self.key_events)
        self.harness.send_key(KEY_PAUSE)
        self.pump_until(lambda: self.harness.diagnostic().count(
            "Word edit dispatch status=0") > rejected, "stale terminal word refused")
        self.wait_event_loop_idle("stale terminal operation completed")
        self.pump_for(0.03)
        self.assertEqual(self.key_events, before_keys,
                         "stale Pause sends no text or control keys to VTE")
        self.assertFalse(received.exists(), "stale Pause must not submit input")
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "0")
        self.harness.send_key(KEY_ENTER)
        self.pump_until(lambda: received.exists(), "invalidated line submitted by the user")
        prefix = received.read_bytes()
        self.assertEqual(prefix, b"ghbdtn\x1bOP\n", "only the native F1 sequence accompanies source input")
        self.harness.type_word("ghbdtn")
        self.harness.send_key(KEY_PAUSE)
        self.pump_until(lambda: self.stats_fields()[1]["word_dispatches"] == "1",
                        "fresh terminal history correction without restart")
        self.harness.send_key(KEY_ENTER)
        self.pump_until(lambda: received.read_bytes() == prefix + "привет\n".encode(),
                        "fresh terminal correction exact PTY suffix")
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD), "startup clipboard baseline")

    def test_vte_word_rejects_before_backspace_when_layout_shortcut_stalls(
            self) -> None:
        terminal, received = self.prepare_vte_line_editor()
        self.layout_shortcut.enabled = False
        key_events = []
        terminal.connect(
            "key-press-event",
            lambda _widget, event: key_events.append(
                Gdk.keyval_name(event.keyval)) or False,
        )
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: "ghbdtn" in terminal.get_text_format(Vte.Format.TEXT),
                        "terminal physical source before stalled layout")
        rejected = self.harness.diagnostic().count("Word edit dispatch status=0")
        self.harness.send_key(KEY_PAUSE)
        self.pump_until(
            lambda: self.harness.diagnostic().count(
                "Word edit dispatch status=0") > rejected,
            "terminal stalled layout rejection",
        )
        self.assertIn("ghbdtn", terminal.get_text_format(Vte.Format.TEXT))
        self.assertNotIn("BackSpace", key_events)
        self.assertFalse(received.exists())
        self.assertEqual(self.layout_shortcut.activations, 0)
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "0")
        self.layout_shortcut.enabled = True
        daemon_pid = self.harness.process.pid
        self.harness.send_key(KEY_ENTER)
        self.pump_until(lambda: received.exists() and received.read_bytes() == b"ghbdtn\n",
                        "user submits unchanged word after rejected correction")
        self.harness.type_word("ghbdtn")
        self.harness.send_key(KEY_PAUSE)
        self.pump_until(lambda: self.stats_fields()[1]["word_dispatches"] == "1",
                        "fresh terminal operation recovers after layout refusal")
        self.harness.send_key(KEY_ENTER)
        self.pump_until(lambda: received.read_bytes() == "ghbdtn\nпривет\n".encode(),
                        "fresh canonical input after layout recovery")
        self.assertEqual(self.harness.process.pid, daemon_pid)

    def test_vte_space_automatic_correction_and_immediate_inverse(self) -> None:
        terminal, received = self.prepare_vte_line_editor()
        self.harness.type_word("ghbdtn")
        self.harness.send_key(KEY_SPACE)
        self.pump_until(lambda: self.stats_fields()[1]["word_dispatches"] == "1"
                        and terminal.get_text_format(Vte.Format.TEXT).strip().splitlines()[-1] == "привет",
                        "terminal automatic Space correction")
        self.send_chord((KEY_LEFTCTRL,), KEY_Z)
        self.pump_until(lambda: self.stats_fields()[1]["word_dispatches"] == "2"
                        and terminal.get_text_format(Vte.Format.TEXT).strip().splitlines()[-1] == "ghbdtn",
                        "terminal automatic correction inverse")
        self.assertFalse(received.exists())
        self.assertEqual(self.output_count(KEY_Z), 0)
        self.harness.send_key(KEY_ENTER)
        self.pump_until(lambda: received.exists() and received.read_bytes() == b"ghbdtn \n",
                        "terminal inverse restores exact original word and Space")
        self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD), "startup clipboard baseline")

    def test_vte_reverse_space_inverse_restores_exact_utf8_and_layout(self) -> None:
        terminal, received = self.prepare_vte_line_editor()
        daemon_pid = self.harness.process.pid
        self.set_desktop_layout(1)
        self.harness.type_word("hello")
        self.pump_until(lambda: terminal.get_text_format(Vte.Format.TEXT).strip().splitlines()[-1] == "руддщ",
                        "physical RU source before terminal automatic correction")
        self.harness.send_key(KEY_SPACE)
        self.pump_until(lambda: self.stats_fields()[1]["word_dispatches"] == "1"
                        and terminal.get_text_format(Vte.Format.TEXT).strip().splitlines()[-1] == "hello",
                        "terminal reverse automatic Space correction")
        self.send_chord((KEY_LEFTCTRL,), KEY_Z)
        self.pump_until(lambda: self.stats_fields()[1]["word_dispatches"] == "2"
                        and terminal.get_text_format(Vte.Format.TEXT).strip().splitlines()[-1] == "руддщ",
                        "terminal reverse inverse preserves every original UTF-8 character")
        self.assertEqual(self.desktop_source_role(), 1)
        self.assertEqual(self.keyboard_group(), 1)
        self.assertFalse(received.exists(), "inverse must not submit terminal input")
        self.assertEqual(self.output_count(KEY_Z), 0, "inverse must not suspend the PTY child")
        self.harness.type_word("f")
        self.pump_until(lambda: terminal.get_text_format(Vte.Format.TEXT).strip().splitlines()[-1] == "руддщ а",
                        "next ordinary key uses restored RU layout")
        self.harness.send_key(KEY_ENTER)
        self.pump_until(lambda: received.exists() and received.read_bytes() == "руддщ а\n".encode(),
                        "exact reverse inverse word, delimiter and subsequent PTY character")
        self.pump_until(lambda: self.selection_text(Gdk.SELECTION_CLIPBOARD)
                        == "startup clipboard baseline", "reverse inverse restores clipboard")
        for key in (KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_LEFTALT):
            self.assertFalse(self.key_is_down(key))
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertIsNone(self.harness.process.poll())

    def test_vte_word_paste_receipt_precedes_counted_delete(self) -> None:
        terminal, received = self.prepare_vte_line_editor()
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: terminal.get_text_format(Vte.Format.TEXT).strip().splitlines()[-1] == "ghbdtn",
                        "live source before terminal transfer admission")
        markers = {
            name: pathlib.Path("/run", f"punto-e2e-{name}")
            for name in ("arm-after-paste-receipt", "after-paste-receipt",
                         "release-after-paste-receipt")
        }
        for marker in markers.values():
            marker.unlink(missing_ok=True)
            self.addCleanup(marker.unlink, missing_ok=True)
        self.addCleanup(markers["release-after-paste-receipt"].touch,
                        mode=0o600, exist_ok=True)
        keys = []
        terminal.connect("key-press-event", lambda _widget, event:
                         keys.append(Gdk.keyval_name(event.keyval)) or False)
        pastes = []
        terminal.connect("paste-clipboard", lambda _terminal: pastes.append(True))
        markers["arm-after-paste-receipt"].touch(mode=0o600)
        self.harness.send_key(KEY_PAUSE)
        self.pump_until(markers["after-paste-receipt"].exists,
                        "terminal replacement has an armed real transfer receipt")
        self.assertEqual(terminal.get_text_format(Vte.Format.TEXT).strip().splitlines()[-1], "ghbdtn")
        self.assertNotIn("BackSpace", keys, "no deletion before complete paste admission")
        self.assertFalse(received.exists())
        markers["release-after-paste-receipt"].touch(mode=0o600)
        self.pump_until(lambda: self.stats_fields()[1]["word_dispatches"] == "1"
                        and terminal.get_text_format(Vte.Format.TEXT).strip().splitlines()[-1] == "привет",
                        "admitted UTF-8 paste replaces the counted terminal word")
        self.assertEqual(pastes, [True], "VTE's native CLIPBOARD binding handles the transfer")
        self.assertIn("Insert", keys, "terminal paste uses a layout-independent key")
        self.harness.send_key(KEY_ENTER)
        self.pump_until(lambda: received.exists() and received.read_bytes() == "привет\n".encode(),
                        "actual terminal reader receives the complete replacement")
        self.pump_until(lambda: self.selection_text(Gdk.SELECTION_CLIPBOARD)
                        == "startup clipboard baseline", "counted terminal paste restores clipboard")

    def test_vte_automatic_inverse_then_manual_uses_current_history(self) -> None:
        terminal, received = self.prepare_vte_line_editor()
        self.harness.type_word("ghbdtn")
        self.harness.send_key(KEY_SPACE)
        self.pump_until(lambda: self.stats_fields()[1]["word_dispatches"] == "1"
                        and terminal.get_text_format(Vte.Format.TEXT).strip().splitlines()[-1] == "привет",
                        "automatic terminal correction before inverse")
        self.send_chord((KEY_LEFTCTRL,), KEY_Z)
        self.pump_until(lambda: self.stats_fields()[1]["word_dispatches"] == "2"
                        and terminal.get_text_format(Vte.Format.TEXT).strip().splitlines()[-1] == "ghbdtn",
                        "terminal inverse updates visible history")
        self.harness.send_key(KEY_PAUSE)
        self.pump_until(lambda: self.stats_fields()[1]["word_dispatches"] == "3"
                        and terminal.get_text_format(Vte.Format.TEXT).strip().splitlines()[-1] == "привет",
                        "next manual correction uses the inverse's current word")
        self.assertFalse(received.exists())
        self.harness.send_key(KEY_ENTER)
        self.pump_until(lambda: received.exists() and received.read_bytes() == "привет \n".encode(),
                        "exact PTY line after automatic inverse then manual correction")
        self.assertEqual(self.output_count(KEY_Z), 0)
        self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD), "startup clipboard baseline")

    def test_stats_wait_serves_actual_post_paste_primary_read(self) -> None:
        terminal, received = self.prepare_vte_line_editor("AbC")
        daemon_pid = self.harness.process.pid
        self.select_vte_source_word(terminal, "AbC")
        names = ("arm-completed-primary-read-observation",
                 "observe-completed-primary-read",
                 "completed-primary-read-submitted",
                 "completed-primary-read-replied")
        markers = [pathlib.Path("/run", f"punto-e2e-{name}") for name in names]
        for marker in markers:
            marker.unlink(missing_ok=True)
            self.addCleanup(marker.unlink, missing_ok=True)
        markers[0].touch(mode=0o600)
        self.send_chord((KEY_LEFTALT,))
        self.pump_until(lambda: "aBc" in terminal.get_text_format(Vte.Format.TEXT),
                        "actual paste before diagnostic owner wait")
        until = time.monotonic() + 0.4
        while not markers[2].exists() and time.monotonic() < until:
            time.sleep(0.001)
        self.assertTrue(markers[2].exists(), "actual post-paste PRIMARY request")
        self.assertFalse(markers[3].exists(), "provider has not replied before STATS")
        owner_thread = threading.get_ident()
        pump_threads = []
        original_pump = self.pump_stats_once

        def observed_pump():
            pump_threads.append(threading.get_ident())
            with self.assertRaisesRegex(RuntimeError, "nested foreground STATS"):
                self.stats_fields()
            original_pump()

        self.pump_stats_once = observed_pump
        started = time.monotonic()
        response, fields = self.stats_fields()
        self.pump_stats_once = original_pump
        self.assertTrue(pump_threads, "foreground wait dispatched the GTK provider")
        self.assertEqual(set(pump_threads), {owner_thread})
        self.assertFalse(self._stats_wait_active)
        self.assertLess(time.monotonic() - started, 0.5)
        self.assertTrue(markers[3].exists(),
                        "real matching positive reply during the same STATS wait")
        self.assertTrue(response.startswith(b"OK "))
        self.assertEqual(fields["word_dispatches"], "1")
        self.harness.type_word("f")
        self.pump_until(lambda: "aBcf" in terminal.get_text_format(Vte.Format.TEXT),
                        "next physical key after diagnostic completion")
        self.harness.send_key(KEY_ENTER)
        self.pump_until(lambda: received.exists() and received.read_bytes() == b"aBcf\n",
                        "exact PTY bytes after the diagnostic owner wait")
        self.pump_until(lambda: self.selection_text(Gdk.SELECTION_CLIPBOARD)
                        == "startup clipboard baseline", "original clipboard survives")
        self.assertEqual(self.keyboard_group(), 0)
        for key in (KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_LEFTALT):
            self.assertFalse(self.key_is_down(key))
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertIsNone(self.harness.process.poll())

    def test_foreground_ipc_partial_reply_does_not_renew_deadline(self) -> None:
        with tempfile.TemporaryDirectory(prefix="punto-ipc-deadline-") as directory:
            path = str(pathlib.Path(directory, "peer.sock"))
            stop = threading.Event()
            commands = []
            errors = []
            pump_threads = []
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as listener:
                listener.bind(path)
                listener.listen(2)
                listener.settimeout(1.0)

                def peer():
                    try:
                        with listener.accept()[0] as client:
                            client.settimeout(1.0)
                            command = bytearray()
                            while b"\n" not in command:
                                command.extend(client.recv(512))
                            commands.append(bytes(command))
                            while not stop.wait(0.05):
                                client.sendall(b"x")
                    except (BrokenPipeError, ConnectionResetError):
                        pass
                    except Exception as error:
                        errors.append(error)

                worker = threading.Thread(target=peer)
                worker.start()
                started = time.monotonic()
                try:
                    with self.assertRaisesRegex(TimeoutError, "foreground IPC deadline"):
                        ipc_request_at(path, b"STATS\n", timeout=0.5,
                                       pump=lambda: pump_threads.append(threading.get_ident()))
                    elapsed = time.monotonic() - started
                    self.assertGreaterEqual(elapsed, 0.49)
                    self.assertLess(elapsed, 0.65)
                    self.assertEqual(commands, [b"STATS\n"])
                    self.assertTrue(pump_threads)
                    self.assertEqual(set(pump_threads), {threading.get_ident()})
                    self.assertFalse(select.select([listener], [], [], 0)[0],
                                     "no second connection was queued")
                finally:
                    stop.set()
                    worker.join(timeout=1.0)
                self.assertFalse(worker.is_alive())
                self.assertEqual(errors, [])

        self.assertEqual(ipc_request(b"SET_STATUS 0\n", timeout=0.5), b"OK DISABLED\n")
        self.assertEqual(self.stats_fields()[1]["enabled"], "0")

    def test_vte_selection_paste_same_layout_needs_no_desktop_toggle(self) -> None:
        terminal, received = self.prepare_vte_line_editor("AbC")
        self.set_desktop_layout(1)
        self.select_vte_source_word(terminal, "AbC")
        self.layout_shortcut.enabled = False
        self.send_chord((KEY_LEFTALT,))
        self.pump_until(lambda: "aBc" in terminal.get_text_format(Vte.Format.TEXT),
                        "VTE same-layout selection paste")
        self.pump_for(0.35)
        self.pump_until(
            lambda: self.selection_text(Gdk.SELECTION_CLIPBOARD)
            == "startup clipboard baseline",
            "VTE same-layout selection restores clipboard after quiet grace",
        )
        self.assertFalse(received.exists())
        self.assertEqual(self.layout_shortcut.activations, 0)
        self.assertEqual(self.desktop_source_role(), 1)
        self.assertEqual(self.keyboard_group(), 1)
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "1")

    def test_vte_plain_paste_accelerator_keeps_ru_layout(self) -> None:
        terminal, received = self.prepare_vte_line_editor()
        self.set_desktop_layout(1)
        self.layout_shortcut.enabled = False
        self.layout_shortcut.reassert_source = True
        self.set_selection(Gdk.SELECTION_CLIPBOARD, "paste probe")
        self.send_chord((KEY_LEFTCTRL, KEY_LEFTSHIFT), KEY_V)
        self.pump_until(lambda: "paste probe" in terminal.get_text_format(Vte.Format.TEXT),
                        "physical Ctrl+Shift+V resolves VTE accelerator in RU")
        self.assertFalse(received.exists(), "paste must not submit terminal input")
        self.assertEqual(self.desktop_source_role(), 1)
        self.assertEqual(self.keyboard_group(), 1)
        self.assertEqual(self.layout_shortcut.activations, 0)
        self.assertEqual(self.layout_shortcut.source_reassertions, 0)
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "0")
        for key in (KEY_LEFTCTRL, KEY_LEFTSHIFT):
            self.assertFalse(self.key_is_down(key))

    def test_vte_selection_layout_ru_stall_refuses_before_insert(self) -> None:
        terminal, received = self.prepare_vte_line_editor("привет")
        daemon_pid = self.harness.process.pid
        self.set_desktop_layout(1)
        self.select_vte_source_word(terminal, "привет")
        self.layout_shortcut.enabled = False
        before = terminal.get_text_format(Vte.Format.TEXT)
        rejected = self.harness.diagnostic().count("Word edit dispatch status=0")
        self.send_chord((KEY_LEFTSHIFT,))
        self.pump_until(
            lambda: self.harness.diagnostic().count(
                "Word edit dispatch status=0") > rejected,
            "VTE RU to EN final layout rejection",
        )
        self.assertEqual(terminal.get_text_format(Vte.Format.TEXT), before)
        self.assertEqual(self.selection_text(Gdk.SELECTION_PRIMARY), "привет")
        self.pump_until(lambda: self.selection_text(Gdk.SELECTION_CLIPBOARD)
                        == "startup clipboard baseline",
                        "partial terminal transfer restores baseline after payload lifetime")
        self.assertFalse(received.exists())
        self.assertEqual(self.desktop_source_role(), 1)
        self.assertEqual(self.keyboard_group(), 1)

        self.layout_shortcut.enabled = True
        self.select_vte_source_word(terminal, "привет")
        self.send_chord((KEY_LEFTSHIFT,))
        self.pump_until(lambda: "ghbdtn" in terminal.get_text_format(Vte.Format.TEXT),
                        "fresh terminal operation after safe layout refusal")
        self.pump_until(lambda: self.stats_fields()[1]["word_dispatches"] == "1",
                        "fresh transfer finalizes its target layout")
        self.assertEqual(self.keyboard_group(), 0)
        self.assertEqual(self.desktop_source_role(), 0)
        self.harness.type_word("f")
        self.pump_until(lambda: "ghbdtnf" in terminal.get_text_format(Vte.Format.TEXT),
                        "next physical EN key after partial recovery")
        self.harness.send_key(KEY_ENTER)
        self.pump_until(lambda: received.exists() and received.read_bytes() == b"ghbdtnf\n",
                        "exact recovered canonical PTY input")
        self.pump_until(lambda: self.selection_text(Gdk.SELECTION_CLIPBOARD)
                        == "startup clipboard baseline", "terminal recovery restores clipboard")
        for key in (KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_LEFTALT):
            self.assertFalse(self.key_is_down(key))
        self.assertEqual(self.output_count(KEY_PAUSE), 0)
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertIsNone(self.harness.process.poll())

    def assert_vte_selection_inserts(self, source: str, transformed: str,
                                     modifiers: tuple[int, ...], recent_clicks: int = 0,
                                     final_group: int = 0,
                                     next_character: str = "f") -> None:
        terminal, received = self.prepare_vte_line_editor(source)
        daemon_pid = self.harness.process.pid
        self.harness.type_word("prefix")
        self.pump_until(lambda: "prefix" in terminal.get_text_format(Vte.Format.TEXT),
                        "nonempty live terminal input")
        self.pump_until(lambda: all(any(phase == expected and name == "p"
                                       for phase, name, _ in self.key_events)
                                   for expected in ("press", "release")),
                        "positive physical VTE window key recorder witness")
        self.select_vte_source_word(terminal, source, recent_clicks)
        self.assertEqual(self.selection_text(Gdk.SELECTION_PRIMARY), source)
        dispatches = int(self.stats_fields()[1]["word_dispatches"])
        self.send_chord(modifiers)
        self.pump_until(lambda: "prefix" + transformed in terminal.get_text_format(Vte.Format.TEXT),
                        "transformed selection inserted without deleting live input")
        self.assertFalse(received.exists(), "selection insertion must not submit a command")
        self.assertIn(source + "\n", terminal.get_text_format(Vte.Format.TEXT))
        self.assertIn("scrollback sentinel", terminal.get_text_format(Vte.Format.TEXT))
        self.pump_until(lambda: self.selection_text(Gdk.SELECTION_CLIPBOARD) == "startup clipboard baseline",
                        "terminal paste restores clipboard after transfer")
        self.pump_until(
            lambda: int(self.stats_fields()[1]["word_dispatches"]) ==
            dispatches + 1,
            "terminal selection reports a complete dispatch",
        )
        self.assertFalse(any(name in {"BackSpace", "Left", "Right", "Return"}
                             for _, name, _ in self.key_events),
                         "selection insertion sends no deletion, navigation or Enter")
        self.assertEqual(self.desktop_source_role(), final_group)
        self.assertEqual(self.keyboard_group(), final_group)
        self.harness.type_word("f")
        self.pump_until(
            lambda: "prefix" + transformed + next_character
            in terminal.get_text_format(Vte.Format.TEXT),
            "next physical terminal key uses final layout",
        )
        self.harness.send_key(KEY_ENTER)
        self.pump_until(lambda: received.exists() and received.read_bytes() ==
                        ("prefix" + transformed + next_character + "\n").encode(),
                        "exact canonical PTY selection insertion")
        self.assertEqual(self.selection_text(Gdk.SELECTION_PRIMARY), source)
        self.assertEqual(self.output_count(KEY_PAUSE), 0)
        for key in (KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_LEFTALT):
            self.assertFalse(self.key_is_down(key))
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertIsNone(self.harness.process.poll())

    def test_vte_selection_case_inserts_without_deleting_scrollback(self) -> None:
        self.assert_vte_selection_inserts("AbC", "aBc", (KEY_LEFTALT,))

    def test_vte_selection_layout_inserts_without_deleting_scrollback(self) -> None:
        self.assert_vte_selection_inserts("ghbdtn", "привет", (KEY_LEFTSHIFT,),
                                          recent_clicks=1, final_group=1,
                                          next_character="а")

    def test_vte_selection_layout_after_recent_click_preserves_exact_selection(self) -> None:
        self.assert_vte_selection_inserts("ghbdtn", "привет", (KEY_LEFTSHIFT,),
                                          recent_clicks=2, final_group=1,
                                          next_character="а")

    def test_vte_selection_translit_has_full_modifier_priority(self) -> None:
        self.assert_vte_selection_inserts("привет", "privet", (KEY_LEFTCTRL, KEY_LEFTALT, KEY_LEFTSHIFT))

    def test_vte_selection_change_before_paste_preserves_input_and_recovers(self) -> None:
        terminal, received = self.prepare_vte_line_editor("AbC")
        daemon_pid = self.harness.process.pid
        self.harness.type_word("prefix")
        self.pump_until(lambda: "prefix" in terminal.get_text_format(Vte.Format.TEXT),
                        "live PTY input before source change")
        self.select_vte_source_word(terminal, "AbC")
        markers = {
            name: pathlib.Path("/run", f"punto-e2e-{name}")
            for name in ("arm-after-paste-receipt", "after-paste-receipt",
                         "release-after-paste-receipt")
        }
        for marker in markers.values():
            marker.unlink(missing_ok=True)
            self.addCleanup(marker.unlink, missing_ok=True)
        self.addCleanup(markers["release-after-paste-receipt"].touch,
                        mode=0o600, exist_ok=True)
        keys = []
        terminal.connect("key-press-event", lambda _widget, event:
                         keys.append(Gdk.keyval_name(event.keyval)) or False)
        refused = self.harness.diagnostic().count("Word edit dispatch status=1")
        markers["arm-after-paste-receipt"].touch(mode=0o600)
        self.send_chord((KEY_LEFTALT,))
        self.pump_until(markers["after-paste-receipt"].exists,
                        "real terminal paste receipt before dispatch")
        self.assertFalse(markers["arm-after-paste-receipt"].exists())
        terminal.select_all()
        self.pump_until(lambda: "\n" in (self.selection_text(Gdk.SELECTION_PRIMARY) or ""),
                        "actual new multiline terminal selection")
        foreign = self.selection_text(Gdk.SELECTION_PRIMARY)
        before = terminal.get_text_format(Vte.Format.TEXT)
        markers["release-after-paste-receipt"].touch(mode=0o600)
        self.pump_until(lambda: self.harness.diagnostic().count(
            "Word edit dispatch status=1") > refused, "terminal source change refuses unsent paste")
        self.assertIn("rejection_stage=paste_context_before_dispatch", self.harness.diagnostic())
        self.assertEqual(terminal.get_text_format(Vte.Format.TEXT), before)
        self.assertEqual(self.selection_text(Gdk.SELECTION_PRIMARY), foreign)
        self.assertFalse(any(key in {"v", "V", "BackSpace", "Left", "Right", "Return"}
                             for key in keys))
        self.assertFalse(received.exists())
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "0")
        self.pump_until(lambda: self.selection_text(Gdk.SELECTION_CLIPBOARD)
                        == "startup clipboard baseline", "unsent terminal paste restores clipboard")

        self.select_vte_source_word(terminal, "AbC")
        self.send_chord((KEY_LEFTALT,))
        self.pump_until(lambda: "prefixaBc" in terminal.get_text_format(Vte.Format.TEXT),
                        "fresh terminal selection succeeds without restart")
        self.pump_until(lambda: self.stats_fields()[1]["word_dispatches"] == "1",
                        "fresh terminal paste dispatch completes")
        self.harness.type_word("f")
        self.pump_until(lambda: "prefixaBcf" in terminal.get_text_format(Vte.Format.TEXT),
                        "next physical key after terminal recovery")
        self.harness.send_key(KEY_ENTER)
        self.pump_until(lambda: received.exists() and received.read_bytes() == b"prefixaBcf\n",
                        "exact recovered PTY input, without deletion")
        self.pump_until(lambda: self.selection_text(Gdk.SELECTION_CLIPBOARD)
                        == "startup clipboard baseline", "terminal recovery restores clipboard")
        self.assertEqual(self.output_count(KEY_PAUSE), 0)
        for key in (KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_LEFTALT):
            self.assertFalse(self.key_is_down(key))
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertIsNone(self.harness.process.poll())

    def test_terminal_control_and_invalid_utf8_sources_refuse_and_recover(self) -> None:
        import gi

        gi.require_version("GdkX11", "3.0")
        from gi.repository import GdkX11

        terminal, received = self.prepare_vte_line_editor("AbC")
        daemon_pid = self.harness.process.pid
        self.harness.type_word("prefix")
        self.pump_until(lambda: "prefix" in terminal.get_text_format(Vte.Format.TEXT),
                        "live PTY prefix before invalid PRIMARY payloads")
        clipboard_owner = self.selection_owner("CLIPBOARD")
        owner = Gtk.Invisible()
        owner.realize()
        self.addCleanup(owner.destroy)
        utf8 = Gdk.Atom.intern("UTF8_STRING", False)
        Gtk.selection_add_target(owner, Gdk.SELECTION_PRIMARY, utf8, 0)
        payload = {"bytes": b""}
        requests = []

        def provide_bytes(_owner, data, _info, _time):
            requests.append(payload["bytes"])
            data.set(utf8, 8, payload["bytes"])

        owner.connect("selection-get", provide_bytes)
        keys = []
        terminal.connect("key-press-event", lambda _widget, event:
                         keys.append(Gdk.keyval_name(event.keyval)) or False)
        for label, raw in (
            ("tab", b"AbC\t"), ("cr", b"AbC\r"), ("escape", b"AbC\x1b[31m"),
            ("c1", "AbC\u009b".encode()), ("lone-c1-byte", b"AbC\x9b"),
            ("invalid-utf8", b"AbC\xf0(\x8c("), ("nul", b"AbC\x00"),
        ):
            with self.subTest(payload=label):
                payload["bytes"] = raw
                requests.clear()
                timestamp = GdkX11.x11_get_server_time(owner.get_window())
                self.assertTrue(Gtk.selection_owner_set(owner, Gdk.SELECTION_PRIMARY, timestamp))
                Gdk.flush()
                self.assertEqual(self.selection_owner(), owner.get_window().get_xid())
                before = terminal.get_text_format(Vte.Format.TEXT)
                rejected = self.harness.diagnostic().count("rejection_stage=terminal_source_text")
                self.send_chord((KEY_LEFTALT,))
                self.pump_until(lambda: self.harness.diagnostic().count(
                    "rejection_stage=terminal_source_text") > rejected,
                    "actual source encoding/control validation before transformation")
                self.assertIn(raw, requests, "daemon read the actual raw UTF8_STRING payload")
                self.assertEqual(terminal.get_text_format(Vte.Format.TEXT), before)
                self.assertEqual(self.selection_owner(), owner.get_window().get_xid())
                self.assertEqual(self.selection_owner("CLIPBOARD"), clipboard_owner)
                self.assertEqual(self.stats_fields()[1]["word_dispatches"], "0")
                self.assertFalse(received.exists())
                self.assertFalse(any(key in {"v", "V", "BackSpace", "Left", "Right", "Return"}
                                     for key in keys))

        owner.destroy()
        self.select_vte_source_word(terminal, "AbC")
        self.send_chord((KEY_LEFTALT,))
        self.pump_until(lambda: "prefixaBc" in terminal.get_text_format(Vte.Format.TEXT),
                        "valid selection after all payload refusals, same daemon")
        self.pump_until(lambda: self.stats_fields()[1]["word_dispatches"] == "1",
                        "valid terminal transfer completes")
        self.harness.type_word("f")
        self.pump_until(lambda: "prefixaBcf" in terminal.get_text_format(Vte.Format.TEXT),
                        "physical key after terminal payload recovery")
        self.harness.send_key(KEY_ENTER)
        self.pump_until(lambda: received.exists() and received.read_bytes() == b"prefixaBcf\n",
                        "exact recovered canonical PTY bytes")
        self.pump_until(lambda: self.selection_text(Gdk.SELECTION_CLIPBOARD)
                        == "startup clipboard baseline", "payload recovery restores clipboard")
        self.assertEqual(self.output_count(KEY_PAUSE), 0)
        for key in (KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_LEFTALT):
            self.assertFalse(self.key_is_down(key))
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertIsNone(self.harness.process.poll())

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


class AccessibleContextGtkE2E(EventLoopGtkE2E):
    def test_accessible_late_keyboard_reply_cannot_revert_manual_layout(self):
        self.prepare_word_editor()
        daemon_pid = self.harness.process.pid
        daemon_start = process_start_time(daemon_pid)
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn", "source word")
        prepared = self.harness.diagnostic().count("[punto-e2e] manual_prepared id=")
        self.harness.send_events([(EV_KEY, KEY_PAUSE, 1), (EV_SYN, SYN_REPORT, 0)])
        self.pump_until(lambda: self.harness.diagnostic().count(
            "[punto-e2e] manual_prepared id=") > prepared,
            "manual candidate prepared before the independent idle read")
        ready, release = self.arm_keyboard_observation(hold=True)
        self.pump_until(lambda: ready.exists()
                        and (marker := ready.read_text()).endswith("\n")
                        and marker.strip().isdecimal(), "held idle keyboard request")
        held_request = int(ready.read_text())
        self.assertGreater(held_request, 0)
        self.assertEqual(self.keyboard_group(), 0)
        self.harness.send_events([(EV_KEY, KEY_PAUSE, 0), (EV_SYN, SYN_REPORT, 0)])
        self.pump_until(lambda: self.entry.get_text() == "привет", "manual RU correction")
        self.assertEqual(self.keyboard_group(), 1)
        release.touch(mode=0o600)
        completed = f"[punto-e2e] keyboard_completed id={held_request} group=0 "
        self.pump_until(lambda: completed in self.harness.diagnostic(),
                        "valid old EN sample delivered after the RU edit")
        self.assertIn(completed + "current=1", self.harness.diagnostic())
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "1")
        self.send_chord((KEY_LEFTCTRL,), KEY_Z)
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn", "same-process exact undo")
        self.assertEqual(self.keyboard_group(), 0)
        self.harness.type_word("f")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtnf", "next EN physical key")
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "2")
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertEqual(process_start_time(daemon_pid), daemon_start)
        self.assertIsNone(self.harness.process.poll())
        self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD), "startup clipboard baseline")
        for key in (KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_LEFTALT, KEY_PAUSE):
            self.assertFalse(self.key_is_down(key))

    def accessible_call(self, method, parameters, reply_type):
        descriptor = json.loads(pathlib.Path(
            "/run/user/0/punto/session-bridge.json").read_text(encoding="ascii"))
        if not hasattr(self, "accessible_peer"):
            self.accessible_peer = Gio.DBusConnection.new_for_address_sync(
                "unix:path=" + descriptor["socket_path"],
                Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT, None, None)
            self.addCleanup(self.accessible_peer.close_sync, None)
        result = []
        def completed(connection, response, _data):
            try:
                result.append(connection.call_finish(response).unpack())
            except GLib.Error as error:
                result.append(error)
        self.accessible_peer.call(None, "/org/punto/Switcher/SessionBridge1",
            "org.punto.Switcher.SessionBridge1", method, parameters,
            GLib.VariantType(reply_type), Gio.DBusCallFlags.NONE, 300, None, completed, None)
        self.pump_until(lambda: result, method + " actual peer reply", timeout=0.4)
        if isinstance(result[0], GLib.Error):
            raise result[0]
        return result[0]

    def observe_editor(self, token="", exact=(-1, -1, ""), pid=None, expired=False):
        helper = self.real_layout_bridge or self.helper_layout_bridge
        reply = self.accessible_call("ObserveEditor", GLib.Variant("(sutsiis)", (
            helper.bound_helper, pid or os.getpid(),
            GLib.get_monotonic_time() + (-1 if expired else 300_000), token, *exact)),
            "(ustssottuiiiiiss)")
        self.assertEqual(reply[1], helper.bound_helper)
        return reply

    def start_accessible_provider(self):
        code = (
            "import gi; gi.require_version('Gtk','3.0'); from gi.repository import Gtk,GdkX11; "
            "Gtk.init([]); w=Gtk.Window(); e=Gtk.Entry(); w.add(e); w.show_all(); e.grab_focus(); "
            "print(w.get_window().get_xid(),flush=True); "
            "e.connect('changed',lambda widget: print(widget.get_text(),flush=True)); Gtk.main()")
        process = subprocess.Popen([sys.executable, "-c", code], stdout=subprocess.PIPE, text=True)
        def stop():
            if process.poll() is None:
                process.terminate()
            process.wait(timeout=2)
        self.addCleanup(stop)
        self.pump_until(lambda: select.select([process.stdout], [], [], 0)[0],
                        "real accessibility provider realization")
        xid = process.stdout.readline().strip()
        self.xdo("windowfocus", "--sync", xid)
        self.publish_active_window(int(xid))
        return process

    def ready_editor(self, pid):
        observations = []
        def registered_and_focused():
            observations.append(self.observe_editor(pid=pid))
            return observations[-1][0] == 0
        self.pump_until(registered_and_focused, "provider registration and settled focus",
                        timeout=1.0)
        return observations[-1]

    def prepare_accessible_range_probe(self):
        if not hasattr(type(self), "_range_probe_entry_type"):
            import gi
            gi.require_version("Atk", "1.0")
            from gi.repository import Atk

            class RangeProbe(Gtk.EntryAccessible, Atk.Text):
                __gtype_name__ = "PuntoE2ERangeProbeAccessible"

                def do_get_text(self, start, end):
                    widget = self.get_widget()
                    if widget is None:
                        return ""
                    text = widget.get_text()
                    result = text[start:] if end < 0 else text[start:end]
                    if widget.range_probe_mode:
                        widget.range_probe_calls += 1
                        call = widget.range_probe_calls
                        if widget.range_probe_mode == "settle" and call <= 3:
                            widget.select_region((1, 2, 0)[call - 1], 4)
                        elif widget.range_probe_mode == "churn":
                            widget.select_region(1 if call % 2 else 0, 4)
                        elif widget.range_probe_mode == "focus" and call == 1:
                            widget.range_probe_other.grab_focus()
                    return result

            class RangeEntry(Gtk.Entry):
                __gtype_name__ = "PuntoE2ERangeProbeEntry"

            RangeEntry.set_accessible_type(RangeProbe)
            type(self)._range_probe_entry_type = RangeEntry
        self.window.remove(self.entry)
        self.entry = type(self)._range_probe_entry_type()
        self.entry.range_probe_mode = ""
        self.entry.range_probe_calls = 0
        other = Gtk.Entry(text="other")
        self.entry.range_probe_other = other
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        box.pack_start(self.entry, True, True, 0)
        box.pack_start(other, True, True, 0)
        self.window.add(box)
        self.entry.set_text("same")
        self.entry.connect("key-press-event", self._record_key, "press")
        self.entry.connect("key-release-event", self._record_key, "release")
        self.window.show_all()
        self.entry.grab_focus()
        self.pump_until(self.entry.has_focus, "range probe actual focus")
        self.entry.select_region(0, 4)
        source = self.ready_editor(os.getpid())
        self.assertEqual(source[10:15], (0, 4, 0, 4, hashlib.sha256(b"same").hexdigest()))
        self.addCleanup(setattr, self.entry, "range_probe_mode", "")
        return source, other

    def assert_accessible_range_probe(self, mode):
        source, other = self.prepare_accessible_range_probe()
        helper = self.real_layout_bridge or self.helper_layout_bridge
        helper_pid = self.session_bridge_process.pid
        helper_start = process_start_time(helper_pid)
        daemon_pid = self.harness.process.pid
        daemon_start = process_start_time(daemon_pid)
        clipboard = self.selection_text(Gdk.SELECTION_CLIPBOARD)
        clipboard_owner = self.selection_owner("CLIPBOARD")
        self.assertIsNotNone(helper_start)
        self.assertIsNotNone(daemon_start)
        self.entry.range_probe_mode = mode
        # The shorter churn deadline remains below the existing 300ms RPC
        # ceiling, so the actual Expired reply can arrive before transport expiry.
        deadline = GLib.get_monotonic_time() + (100_000 if mode == "churn" else 300_000)
        print(f"[punto-e2e] range_probe_begin mode={mode} helper_pid={helper_pid} "
              f"deadline_us={deadline}", flush=True)
        try:
            observed = self.accessible_call("ObserveEditor", GLib.Variant("(sutsiis)", (
                helper.bound_helper, os.getpid(), deadline, source[15], -1, -1, "")),
                "(ustssottuiiiiiss)")
        finally:
            self.entry.range_probe_mode = ""
        print(f"[punto-e2e] range_probe_end mode={mode} helper_pid={helper_pid} "
              f"getter_calls={self.entry.range_probe_calls} status={observed[0]} "
              f"token_match={int(observed[15] == source[15])} "
              f"range_start={observed[12]} range_end={observed[13]} "
              f"range_sha256={observed[14]}", flush=True)
        self.assertEqual(observed[1], helper.bound_helper)
        if mode == "settle":
            self.assertEqual(observed[0], 0)
            self.assertGreaterEqual(self.entry.range_probe_calls, 4)
            self.assertLess(GLib.get_monotonic_time(), deadline)
            self.assertEqual(observed[2:7], source[2:7])
            self.assertEqual(observed[8], source[8])
            self.assertGreaterEqual(observed[7], source[7])
            self.assertLess(observed[7], deadline)
            self.assertEqual(observed[10:15], source[10:15])
            self.assertEqual(observed[15], source[15])
            self.assertEqual(self.entry.get_selection_bounds(), (0, 4))
        elif mode == "churn":
            self.assertEqual((observed[0], observed[15]), (3, ""))
            self.assertGreaterEqual(self.entry.range_probe_calls, 3)
            self.assertGreaterEqual(GLib.get_monotonic_time(), deadline)
        else:
            self.assertIn(observed[0], (2, 3))
            self.assertEqual(observed[15], "")
            self.assertGreaterEqual(self.entry.range_probe_calls, 1)
            self.assertTrue(other.has_focus())
        self.assertEqual((self.entry.get_text(), other.get_text()), ("same", "other"))
        self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD), clipboard)
        self.assertEqual(self.selection_owner("CLIPBOARD"), clipboard_owner)
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "0")
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertEqual(process_start_time(daemon_pid), daemon_start)
        self.assertIsNone(self.harness.process.poll())
        self.assertEqual(self.session_bridge_process.pid, helper_pid)
        self.assertEqual(process_start_time(helper_pid), helper_start)
        self.assertIsNone(self.session_bridge_process.poll())
        for key in (KEY_PAUSE, KEY_RIGHT, KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_LEFTALT):
            self.assertFalse(self.key_is_down(key))
            self.assertEqual(self.output_count(key), 0)
        self.entry.grab_focus()
        self.pump_until(self.entry.has_focus, "range probe fresh recovery focus")
        self.entry.select_region(0, 4)
        fresh = self.ready_editor(os.getpid())
        self.assertEqual(fresh[0], 0)
        self.assertEqual(fresh[10:15], source[10:15])
        self.assertEqual((self.entry.get_text(), other.get_text()), ("same", "other"))
        self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD), clipboard)
        self.assertEqual(self.selection_owner("CLIPBOARD"), clipboard_owner)
        self.assertEqual(process_start_time(helper_pid), helper_start)
        self.assertEqual(process_start_time(daemon_pid), daemon_start)

    def test_accessible_three_range_changes_settle_within_original_deadline(self):
        self.assert_accessible_range_probe("settle")

    def test_accessible_continuous_range_changes_expire_and_recover(self):
        self.assert_accessible_range_probe("churn")

    def test_accessible_range_change_focus_drift_never_mints_authority(self):
        self.assert_accessible_range_probe("focus")

    def test_accessible_two_children_same_toplevel_and_focus_aba(self):
        self.window.remove(self.entry)
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        other = Gtk.Entry(text="destination")
        box.pack_start(self.entry, True, True, 0); box.pack_start(other, True, True, 0)
        self.window.add(box); self.window.show_all()
        self.entry.set_text("source"); self.entry.grab_focus()
        self.pump_until(self.entry.has_focus, "source child focus")
        focus = self.xdo("getwindowfocus")
        source = self.ready_editor(os.getpid()); self.assertEqual(source[0], 0)
        self.assertEqual(source[8], os.getpid())
        other.grab_focus(); self.pump_until(other.has_focus, "destination child focus")
        self.assertEqual(self.observe_editor(source[15])[0], 2)
        destination = self.observe_editor(); self.assertEqual(destination[0], 0)
        self.assertNotEqual(destination[5], source[5])
        self.entry.grab_focus(); self.pump_until(self.entry.has_focus, "A/B/A returns to source")
        self.assertEqual(self.observe_editor(source[15])[0], 2)
        fresh = self.observe_editor(); self.assertEqual(fresh[0], 0)
        self.assertEqual(fresh[5], source[5]); self.assertNotEqual(fresh[15], source[15])
        self.assertGreater(fresh[6], source[6]); self.assertEqual(self.xdo("getwindowfocus"), focus)
        other.grab_focus(); self.pump_until(other.has_focus, "unqueried ABA B")
        self.entry.grab_focus(); self.pump_until(self.entry.has_focus, "unqueried ABA A")
        self.assertEqual(self.observe_editor(fresh[15])[0], 2)
        newest = self.observe_editor(); self.assertEqual(newest[0], 0)
        self.assertEqual(newest[5], fresh[5]); self.assertNotEqual(newest[15], fresh[15])
        self.assertEqual((self.entry.get_text(), other.get_text()), ("source", "destination"))

    def test_accessible_exact_native_range_stale_and_expired_are_readonly(self):
        text = "before ghbdtn after"; exact = (7, 13, hashlib.sha256(b"ghbdtn").hexdigest())
        self.entry.set_text(text); self.entry.select_region(7, 13)
        observed = self.ready_editor(os.getpid()); self.assertEqual(observed[0], 0)
        self.assertEqual(observed[9], self.entry.get_position())
        self.assertEqual(observed[10:15], (7, 13, *exact))
        verified = self.observe_editor(observed[15], exact); self.assertEqual(verified[0], 0)
        self.assertEqual(verified[15], observed[15])
        stale = self.observe_editor(observed[15], (7, 13, "00" * 32))
        self.assertEqual((stale[0], stale[15]), (2, ""))
        expired = self.observe_editor(observed[15], exact, expired=True)
        self.assertEqual((expired[0], expired[15]), (3, ""))
        self.assertEqual((self.entry.get_text(), self.entry.get_selection_bounds()), (text, (7, 13)))
        self.assertEqual(self.output_count(KEY_RIGHT), 0)
        self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD), "startup clipboard baseline")
        self.assertEqual(self.keyboard_locks(), 0)

    def assert_accessible_prebinding_focus(self, bound):
        self.window.remove(self.entry)
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL); other = Gtk.Entry(text="same")
        for entry in (self.entry, other):
            box.pack_start(entry, True, True, 0)
            entry.set_text("same"); entry.select_region(0, 4)
        self.window.add(box); self.window.show_all(); self.entry.grab_focus()
        source = self.ready_editor(os.getpid()); process = self.harness.process
        selection_before = (self.entry.get_selection_bounds(), other.get_selection_bounds())
        pid, started = process.pid, process_start_time(process.pid)
        self.assertIsNone(process.poll()); self.assertIsNotNone(started); self.assertNotEqual(pid, os.getpid())
        namespace = os.readlink("/proc/self/ns/pid")
        self.assertEqual(os.readlink(f"/proc/{pid}/ns/pid"), namespace)
        self.assertNotEqual(namespace, os.environ["PUNTO_E2E_OUTER_PID_NAMESPACE"])
        def resume():
            if process.poll() is None:
                self.assertEqual(process_start_time(pid), started)
                os.kill(pid, signal.SIGCONT)
        self.addCleanup(resume)
        os.kill(pid, signal.SIGSTOP)
        resume_registry = None
        try:
            def idle_read_retired():
                try:
                    observed = self.observe_editor(source[15])
                except GLib.Error as error:
                    if Gio.DBusError.get_remote_error(error) != "org.punto.Switcher.Error.Busy":
                        raise
                    return False
                if observed[0] == 1:
                    return False
                self.assertEqual(observed[0], 0)
                return True
            self.pump_until(idle_read_retired, "prior idle observation retired", timeout=0.4)
            session = Gio.bus_get_sync(Gio.BusType.SESSION, None)
            address = session.call_sync("org.a11y.Bus", "/org/a11y/bus", "org.a11y.Bus",
                "GetAddress", None, GLib.VariantType("(s)"),
                Gio.DBusCallFlags.NO_AUTO_START, 300, None).unpack()[0]
            bus = Gio.DBusConnection.new_for_address_sync(address,
                Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT |
                Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, None, None)
            self.addCleanup(bus.close_sync, None)
            provider, sender = source[4], bus.get_unique_name()
            helper_pid = self.session_bridge_process.pid
            self.assertIsNone(self.session_bridge_process.poll())
            self.assertTrue(hasattr(os, "pidfd_open") and hasattr(signal, "pidfd_send_signal"))
            def registry_bus_call(method, parameters, reply_type):
                return bus.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                    method, parameters, GLib.VariantType(reply_type),
                    Gio.DBusCallFlags.NO_AUTO_START, 300, None).unpack()[0]
            registry_owner = registry_bus_call("GetNameOwner",
                GLib.Variant("(s)", ("org.a11y.atspi.Registry",)), "(s)")
            self.assertEqual(registry_owner, source[3])
            registry_pid = registry_bus_call("GetConnectionUnixProcessID",
                GLib.Variant("(s)", (registry_owner,)), "(u)")
            self.assertGreater(registry_pid, 1); self.assertNotIn(registry_pid, (os.getpid(), helper_pid, pid))
            registry_proc = pathlib.Path("/proc", str(registry_pid)); registry_start = process_start_time(registry_pid)
            self.assertIsNotNone(registry_start)
            registry_pidfd = os.pidfd_open(registry_pid); self.addCleanup(os.close, registry_pidfd)
            self.assertEqual(os.readlink(registry_proc / "ns/pid"), namespace)
            self.assertEqual(os.readlink(registry_proc / "exe"), str(pathlib.Path("/usr/libexec/at-spi2-registryd").resolve(strict=True)))
            self.assertEqual(registry_bus_call("GetConnectionUnixUser",
                GLib.Variant("(s)", (registry_owner,)), "(u)"), os.geteuid())
            uid_line = next(line for line in (registry_proc / "status").read_text().splitlines() if line.startswith("Uid:"))
            self.assertEqual(uid_line.split()[1:], [str(os.geteuid())] * 4)
            self.assertEqual(process_start_time(registry_pid), registry_start)
            def resume_registry(): signal.pidfd_send_signal(registry_pidfd, signal.SIGCONT, None, 0)
            self.addCleanup(resume_registry)
            monitor = subprocess.Popen(["dbus-monitor", "--address", address, "--profile",
                f"type='method_call',destination='{registry_owner}',path='/org/a11y/atspi/accessible/root',interface='org.a11y.atspi.Accessible',member='GetChildren'",
                f"type='method_return',sender='{registry_owner}'",
                f"type='signal',sender='{provider}',path='{source[5]}',interface='org.a11y.atspi.Event.Object',member='StateChanged'",
                f"type='method_call',sender='{sender}',destination='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='GetId'"],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
            def stop_monitor():
                try:
                    if monitor.poll() is None:
                        monitor.terminate()
                    monitor.wait(timeout=2)
                finally:
                    os.set_blocking(monitor.stderr.fileno(), False)
                    try:
                        diagnostic = os.read(monitor.stderr.fileno(), 2048)
                    except BlockingIOError:
                        diagnostic = b""
                    if diagnostic:
                        print("[punto-e2e] monitor_stderr=" +
                              diagnostic.decode("ascii", errors="backslashreplace"), flush=True)
                    monitor.stdout.close(); monitor.stderr.close()
            self.addCleanup(stop_monitor)
            pending = bytearray()
            def profile_until(deadline, destination, interface, member, expected_sender=None, message_type="mc", path=None, reply_serial=None):
                while GLib.get_monotonic_time() < deadline:
                    while b"\n" in pending:
                        line, _, tail = pending.partition(b"\n"); pending[:] = tail; fields = line.decode("ascii").split("\t")
                        if (reply_serial is not None and len(fields) == 6 and
                                fields[0] == "mr" and fields[3] == expected_sender and
                                fields[5] == str(reply_serial)):
                            return GLib.get_monotonic_time(), fields[3], int(fields[2])
                        # --profile header: type timestamp serial sender destination path interface member.
                        if (len(fields) == 8 and fields[0] == message_type and
                                (fields[6], fields[7]) == (interface, member) and
                                (destination is None or fields[4] == destination) and
                                (path is None or fields[5] == path) and
                                (expected_sender is None or fields[3] == expected_sender)):
                            return GLib.get_monotonic_time(), fields[3], int(fields[2])
                    self.assertIsNone(monitor.poll(), "private AT-SPI monitor exited")
                    remaining = max(0, (deadline - GLib.get_monotonic_time()) / 1e6)
                    if select.select([monitor.stdout], [], [], remaining)[0]:
                        chunk = os.read(monitor.stdout.fileno(), 4096)
                        self.assertTrue(chunk, "private AT-SPI monitor closed")
                        pending.extend(chunk)
                return None
            handshake_deadline = GLib.get_monotonic_time() + 1_000_000; ready = None
            while ready is None and GLib.get_monotonic_time() < handshake_deadline:
                bus.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus",
                    "org.freedesktop.DBus", "GetId", None, GLib.VariantType("(s)"),
                    Gio.DBusCallFlags.NO_AUTO_START, 300, None)
                ready = profile_until(min(handshake_deadline, GLib.get_monotonic_time() + 20_000),
                                      "org.freedesktop.DBus", "org.freedesktop.DBus", "GetId", sender)
            self.assertIsNotNone(ready, "actual GetId proves installed monitor match")
            helper = self.real_layout_bridge or self.helper_layout_bridge
            signal.pidfd_send_signal(registry_pidfd, signal.SIGSTOP, None, 0)
            self.pump_until(lambda: next(line for line in (registry_proc / "status").read_text().splitlines()
                            if line.startswith("State:")).split()[1] in ("T", "t"),
                            "owned private Registry stopped before measurement", timeout=0.4)
            if bound in ("preparation-priority", "foreground-priority"):
                old = os.environ.get("PUNTO_PREPARATION_OLD_COUNTERFACTUAL") == "1"
                prep_method = "ObserveEditorOrigin" if old else "PrepareEditorOrigin"
                if not old:
                    xml = self.accessible_peer.call_sync(
                        None, "/org/punto/Switcher/SessionBridge1",
                        "org.freedesktop.DBus.Introspectable", "Introspect", None,
                        GLib.VariantType("(s)"), Gio.DBusCallFlags.NONE, 300, None).unpack()[0]
                    self.assertIn('<method name="PrepareEditorOrigin">', xml,
                                  "candidate preparation API must exist; no fallback")
                descriptor = json.loads(pathlib.Path(
                    "/run/user/0/punto/session-bridge.json").read_text(encoding="ascii"))
                peers = [self.accessible_peer]
                for _ in range(2):
                    peer = Gio.DBusConnection.new_for_address_sync(
                        "unix:path=" + descriptor["socket_path"],
                        Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT, None, None)
                    def close_peer(peer=peer):
                        if not peer.is_closed():
                            peer.close_sync(None)
                    self.addCleanup(close_peer)
                    peers.append(peer)
                clipboard_owner = self.selection_owner("CLIPBOARD")
                helper_start = process_start_time(helper_pid)
                self.assertIsNotNone(helper_start)
                replies = {}
                def submit(peer, label, method, budget):
                    deadline = GLib.get_monotonic_time() + budget
                    def completed(connection, response, _data):
                        try:
                            replies[label] = connection.call_finish(response).unpack()
                            print(f"[punto-e2e] observation_reply label={label} "
                                  f"status={replies[label][0]} at_us={GLib.get_monotonic_time()} "
                                  f"deadline_us={deadline}", flush=True)
                        except GLib.Error as error:
                            replies[label] = error
                    peer.call(None, "/org/punto/Switcher/SessionBridge1",
                        "org.punto.Switcher.SessionBridge1", method,
                        GLib.Variant("(suts)", (helper.bound_helper, os.getpid(), deadline, "")),
                        GLib.VariantType("(ustssottuiiiiiss)"), Gio.DBusCallFlags.NONE,
                        budget // 1000, None, completed, None)
                    return deadline
                def response(label, deadline, status):
                    self.pump_until(lambda: label in replies, label + " actual reply",
                                    timeout=max(0, (deadline - GLib.get_monotonic_time()) / 1e6))
                    self.assertLess(GLib.get_monotonic_time(), deadline)
                    reply = replies[label]
                    if isinstance(reply, GLib.Error):
                        raise reply
                    self.assertEqual((reply[0], reply[1]), (status, helper.bound_helper))
                    if status != 0:
                        self.assertEqual(reply[15], "")
                    return reply
                def arrived(deadline, label):
                    events = []
                    def query_arrived():
                        event = profile_until(min(deadline, GLib.get_monotonic_time() + 2_000),
                            registry_owner, "org.a11y.atspi.Accessible", "GetChildren",
                            path="/org/a11y/atspi/accessible/root")
                        if event is not None:
                            events.append(event)
                        self.assertNotIn(label, replies,
                                         f"{label} replied before Registry admission: {replies.get(label)}")
                        return bool(events)
                    self.pump_until(query_arrived, label + " actual GetChildren at stopped owned Registry",
                        timeout=max(0, (deadline - GLib.get_monotonic_time()) / 1e6))
                    event = events[0]
                    self.assertEqual(registry_bus_call("GetConnectionUnixProcessID",
                        GLib.Variant("(s)", (event[1],)), "(u)"), helper_pid)
                    self.assertLess(GLib.get_monotonic_time(), deadline)
                    print(f"[punto-e2e] observation_admitted label={label} "
                          f"helper_pid={helper_pid} serial={event[2]} at_us={event[0]} "
                          f"deadline_us={deadline}", flush=True)
                    return event
                prep_first = bound == "preparation-priority"
                initial_deadline = submit(peers[1], "initial",
                    prep_method if prep_first else "ObserveEditorOrigin",
                    100_000 if prep_first else 300_000)
                initial = arrived(initial_deadline, "initial")
                self.assertNotIn("initial", replies)
                if prep_first:
                    final_deadline = submit(peers[2], "fresh", "ObserveEditorOrigin", 300_000)
                    successor = arrived(final_deadline, "fresh")
                    self.assertNotEqual(successor[2], initial[2])
                    response("initial", initial_deadline, 2)
                    self.assertNotIn("fresh", replies)
                    peers[1].close_sync(None)
                    label = "fresh"
                else:
                    prep_deadline = submit(peers[0], "preparation", prep_method, 100_000)
                    response("preparation", prep_deadline, 1)
                    refused_deadline = submit(peers[2], "foreground", "ObserveEditorOrigin", 300_000)
                    response("foreground", min(refused_deadline, initial_deadline), 1)
                    self.assertNotIn("initial", replies)
                    final_deadline, label = initial_deadline, "initial"
                self.assertEqual(process_start_time(registry_pid), registry_start)
                resume_registry()
                late = []
                def initial_provider_replied():
                    event = profile_until(min(final_deadline, GLib.get_monotonic_time() + 2_000),
                        None, None, None, registry_owner, reply_serial=initial[2])
                    if event is not None:
                        late.append(event)
                    return bool(late)
                self.pump_until(initial_provider_replied, "actual original Registry reply after resume",
                    timeout=max(0, (final_deadline - GLib.get_monotonic_time()) / 1e6))
                fresh = response(label, final_deadline, 0)
                self.assertEqual((fresh[3], fresh[4], fresh[5], fresh[8]),
                                 (registry_owner, provider, source[5], os.getpid()))
                self.assertEqual(fresh[9:15], (-1, -1, -1, -1, -1, ""))
                self.assertGreaterEqual(fresh[6], source[6])
                self.assertTrue(fresh[15])
                self.assertEqual((self.entry.get_text(), other.get_text()), ("same", "same"))
                self.assertEqual((self.entry.get_selection_bounds(), other.get_selection_bounds()),
                                 selection_before)
                self.assertEqual(self.selection_owner("CLIPBOARD"), clipboard_owner)
                self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD), "startup clipboard baseline")
                self.assertEqual(process_start_time(pid), started)
                self.assertEqual(process_start_time(helper_pid), helper_start)
                self.assertIsNone(process.poll()); self.assertIsNone(self.session_bridge_process.poll())
                self.assertEqual(self.output_count(KEY_V), 0)
                for key in (KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_LEFTALT):
                    self.assertFalse(self.key_is_down(key))
                print(f"[punto-e2e] observation_priority mode={bound} old={int(old)} "
                      f"helper_pid={helper_pid} initial_serial={initial[2]} "
                      f"late_serial={late[0][2]} status={fresh[0]}", flush=True)
                return
            called = GLib.get_monotonic_time(); deadline = called + 300_000
            token = source[15] if bound == "token" else ""
            method, signature, values = "ObserveEditorOrigin", "(suts)", (
                helper.bound_helper, os.getpid(), deadline, token)
            if bound == "range":
                method, signature = "ObserveEditor", "(sutsiis)"
                values += (0, 4, hashlib.sha256(b"same").hexdigest())
            result = []
            def completed(connection, response, _data):
                try:
                    result.append(connection.call_finish(response).unpack())
                except GLib.Error as error:
                    result.append(error)
            self.accessible_peer.call(None, "/org/punto/Switcher/SessionBridge1",
                "org.punto.Switcher.SessionBridge1", method, GLib.Variant(signature, values),
                GLib.VariantType("(ustssottuiiiiiss)"), Gio.DBusCallFlags.NONE,
                300, None, completed, None)
            arrived = profile_until(deadline, registry_owner, "org.a11y.atspi.Accessible", "GetChildren",
                                    path="/org/a11y/atspi/accessible/root")
            self.assertIsNotNone(arrived, "bootstrap query arrived at paused private Registry")
            arrived, phase_owner, serial = arrived
            actual_pid = bus.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus",
                "org.freedesktop.DBus", "GetConnectionUnixProcessID", GLib.Variant("(s)", (phase_owner,)),
                GLib.VariantType("(u)"), Gio.DBusCallFlags.NO_AUTO_START, 300, None).unpack()[0]
            self.assertEqual(actual_pid, helper_pid, "GetChildren sender is the owned helper")
            other.grab_focus(); other.select_region(0, 4); focused = GLib.get_monotonic_time()
            self.assertTrue(other.has_focus(), "real B focus established before GTK dispatch")
            focus_events = []
            def focus_emitted():
                event = profile_until(min(deadline, GLib.get_monotonic_time() + 2_000), None,
                    "org.a11y.atspi.Event.Object", "StateChanged", provider, "sig", source[5])
                if event is not None: focus_events.append(event)
                return bool(focus_events)
            self.pump_until(focus_emitted, "real old A state signal before Registry resume",
                            timeout=max(0, (deadline - GLib.get_monotonic_time()) / 1e6))
            self.assertLess(GLib.get_monotonic_time(), deadline); resume_registry()
            self.pump_until(lambda: result, "single prebinding observation reply", timeout=0.4)
            if isinstance(result[0], GLib.Error):
                raise result[0]
            print(f"[punto-e2e] prebinding_phase bound={bound} pid={os.getpid()} "
                  f"helper_pid={helper_pid} serial={serial} called_us={called} bootstrap_us={arrived} focus_us={focused} "
                  f"event_us={focus_events[0][0]} event_serial={focus_events[0][2]} "
                  f"reply_us={GLib.get_monotonic_time()} status={result[0][0]}", flush=True)
            self.assertEqual(result[0][0], 0 if bound == "fresh" else 2)
            if bound == "fresh":
                self.assertEqual((result[0][1], result[0][4], result[0][8]),
                                 (helper.bound_helper, provider, os.getpid()))
                self.assertNotEqual(result[0][5], source[5])
                self.assertGreater(result[0][6], source[6])
            else: self.assertEqual(result[0][15], "")
            self.assertEqual((self.entry.get_text(), other.get_text()), ("same", "same"))
            self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD), "startup clipboard baseline")
        finally:
            try:
                if resume_registry is not None: resume_registry()
            finally: resume()

    def test_accessible_prebinding_fresh_focus_observation(self):
        self.assert_accessible_prebinding_focus("fresh")

    def test_accessible_prebinding_expected_token_rejects_focus_change(self):
        self.assert_accessible_prebinding_focus("token")

    def test_accessible_prebinding_expected_range_without_token_rejects_focus_change(self):
        self.assert_accessible_prebinding_focus("range")

    def test_accessible_preparation_yields_to_fresh_origin_and_old_peer_close_is_scoped(self):
        self.assert_accessible_prebinding_focus("preparation-priority")

    def test_accessible_foreground_refuses_preparation_and_second_foreground(self):
        self.assert_accessible_prebinding_focus("foreground-priority")

    def test_automatic_history_cannot_cross_identical_child_fields(self):
        self.prepare_word_editor()
        self.assertEqual(ipc_request(b"SET_STATUS 1\n"), b"OK ENABLED\n")
        self.window.remove(self.entry)
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        other = Gtk.Entry(text="ghbdtn")
        box.pack_start(self.entry, True, True, 0)
        box.pack_start(other, True, True, 0)
        self.window.add(box); self.window.show_all()
        self.entry.grab_focus()
        self.pump_until(self.entry.has_focus, "origin child before physical typing")
        source = self.observe_editor(); self.assertEqual(source[0], 0)
        x11_focus = self.xdo("getwindowfocus")
        daemon_pid = self.harness.process.pid
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn", "physical origin word")
        other.grab_focus(); other.set_position(-1)
        self.pump_until(other.has_focus, "programmatic identical destination")
        self.assertEqual(self.xdo("getwindowfocus"), x11_focus)
        self.assertEqual(self.observe_editor(source[15])[0], 2)
        self.assertEqual(self.observe_editor()[0], 0)
        Gtk.Clipboard.get(Gdk.SELECTION_PRIMARY).clear()
        self.pump_until(lambda: self.selection_text(Gdk.SELECTION_PRIMARY) is None,
                        "no unrelated retained PRIMARY at the origin test seam")
        self.harness.send_key(KEY_SPACE)
        self.pump_until(lambda: other.get_text().endswith(" "), "physical Space in destination")
        self.pump_for(0.35)
        self.assertEqual((self.entry.get_text(), other.get_text()), ("ghbdtn", "ghbdtn "))
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "0")
        self.assertEqual((self.output_count(KEY_SPACE, 1), self.output_count(KEY_SPACE, 0)), (1, 1))
        self.harness.type_word("ghbdtn")
        self.harness.send_key(KEY_SPACE)
        self.pump_until(lambda: other.get_text() == "ghbdtn привет ", "fresh destination correction")
        self.assertEqual(self.entry.get_text(), "ghbdtn")
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "1")
        self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD), "startup clipboard baseline")
        for key in (KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_LEFTALT):
            self.assertFalse(self.key_is_down(key))

    def test_accessible_origin_has_no_range_authority(self):
        self.entry.set_text("before ghbdtn after")
        self.entry.select_region(7, 13)
        source = self.ready_editor(os.getpid())
        helper = self.real_layout_bridge or self.helper_layout_bridge
        origin = self.accessible_call("ObserveEditorOrigin", GLib.Variant("(suts)", (
            helper.bound_helper, os.getpid(), GLib.get_monotonic_time() + 300_000, "")),
            "(ustssottuiiiiiss)")
        self.assertEqual(origin[0], 0)
        self.assertEqual(origin[4:7], source[4:7])
        self.assertEqual(origin[9:15], (-1, -1, -1, -1, -1, ""))
        self.assertEqual(origin[15], source[15])
        verified = self.observe_editor(origin[15], (7, 13, hashlib.sha256(b"ghbdtn").hexdigest()))
        self.assertEqual(verified[0], 0)
        self.assertEqual(self.entry.get_selection_bounds(), (7, 13))
        self.assertEqual(self.entry.get_text(), "before ghbdtn after")
        self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD), "startup clipboard baseline")

    def test_automatic_partial_backspace_preserves_word_origin(self):
        self.prepare_word_editor()
        self.assertEqual(ipc_request(b"SET_STATUS 1\n"), b"OK ENABLED\n")
        self.ready_editor(os.getpid())
        daemon_pid = self.harness.process.pid
        self.harness.type_word("ghbdtnx")
        self.pump_until(lambda: self.entry.get_text() == "ghbdtnx", "physical partial word")
        self.harness.send_key(KEY_BACKSPACE)
        self.pump_until(lambda: self.entry.get_text() == "ghbdtn", "physical partial Backspace")
        self.harness.send_key(KEY_SPACE)
        self.pump_until(lambda: self.entry.get_text() == "привет ", "correction after partial Backspace")
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "1")
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD), "startup clipboard baseline")

    def test_automatic_burst_preserves_distinct_same_child_words(self):
        self.prepare_word_editor()
        self.assertEqual(ipc_request(b"SET_STATUS 1\n"), b"OK ENABLED\n")
        self.ready_editor(os.getpid())
        self.send_text_batch("ghbdtn world ")
        self.pump_until(lambda: self.entry.get_text() == "привет world ", "older correction preserves distinct younger word")
        self.assertEqual(self.stats_fields()[1]["word_dispatches"], "1")
        self.assertEqual(self.entry.get_selection_bounds(), ())
        self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD), "startup clipboard baseline")
        for key in (KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_LEFTALT):
            self.assertFalse(self.key_is_down(key))

    def test_accessible_provider_loss_recovers_without_helper_restart(self):
        helper_pid = self.session_bridge_process.pid
        old = self.start_accessible_provider()
        original = self.ready_editor(old.pid); self.assertEqual(original[0], 0)
        old.terminate(); old.wait(timeout=2)
        refused = self.observe_editor(original[15], pid=old.pid)
        self.assertIn(refused[0], (1, 2)); self.assertEqual(refused[15], "")
        replacement = self.start_accessible_provider()
        fresh = self.ready_editor(replacement.pid); self.assertEqual(fresh[0], 0)
        self.assertEqual(fresh[2], original[2], "provider loss preserves the healthy bus epoch")
        self.assertGreater(fresh[6], original[6], "replacement gets fresh focus authority")
        self.assertEqual(self.observe_editor(original[15], pid=replacement.pid)[0], 2)
        self.assertNotEqual(fresh[15], original[15]); self.assertNotEqual(fresh[4], original[4])
        self.xdo("type", "--clearmodifiers", "f")
        self.pump_until(lambda: select.select([replacement.stdout], [], [], 0)[0], "fresh field input")
        self.assertEqual(replacement.stdout.readline().strip(), "f")
        typed = self.observe_editor(fresh[15], pid=replacement.pid)
        self.assertEqual(typed[0], 0)
        self.assertEqual(typed[9], 1)
        state = self.accessible_call("GetState", GLib.Variant("(st)",
            ("33" * 16, GLib.get_monotonic_time() + 300_000)), "(usssttty)")
        self.assertEqual(state[0], 0); self.assertEqual(self.session_bridge_process.pid, helper_pid)
        self.assertIsNone(self.session_bridge_process.poll())

        self.assertTrue(hasattr(os, "pidfd_open") and hasattr(signal, "pidfd_send_signal"),
                        "registry loss requires PID-pinned signaling")
        inner_namespace = os.readlink("/proc/self/ns/pid")
        self.assertNotEqual(inner_namespace, os.environ["PUNTO_E2E_OUTER_PID_NAMESPACE"])
        session_bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
        address = session_bus.call_sync(
            "org.a11y.Bus", "/org/a11y/bus", "org.a11y.Bus", "GetAddress", None,
            GLib.VariantType("(s)"), Gio.DBusCallFlags.NO_AUTO_START, 300, None).unpack()[0]
        self.assertTrue(address.startswith("unix:"))
        bus = Gio.DBusConnection.new_for_address_sync(
            address, Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT
            | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, None, None)
        self.addCleanup(bus.close_sync, None)

        def registry_bus_call(method, parameters, reply_type):
            return bus.call_sync(
                "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                method, parameters, GLib.VariantType(reply_type),
                Gio.DBusCallFlags.NO_AUTO_START, 300, None).unpack()[0]

        registry_name = "org.a11y.atspi.Registry"
        registry_owner = registry_bus_call("GetNameOwner", GLib.Variant("(s)", (registry_name,)), "(s)")
        self.assertEqual(registry_owner, fresh[3])
        registry_pid = registry_bus_call("GetConnectionUnixProcessID",
                                        GLib.Variant("(s)", (registry_owner,)), "(u)")
        self.assertGreater(registry_pid, 1)
        self.assertNotIn(registry_pid, (os.getpid(), helper_pid))
        registry_proc = pathlib.Path("/proc", str(registry_pid))
        registry_exe = pathlib.Path("/usr/libexec/at-spi2-registryd").resolve(strict=True)
        registry_start = process_start_time(registry_pid)
        self.assertIsNotNone(registry_start)
        clipboard_before = self.selection_text(Gdk.SELECTION_CLIPBOARD)
        pidfd = os.pidfd_open(registry_pid)
        try:
            self.assertEqual(os.readlink(registry_proc / "ns/pid"), inner_namespace)
            self.assertEqual(os.readlink(registry_proc / "exe"), str(registry_exe))
            self.assertEqual(process_start_time(registry_pid), registry_start)
            self.assertEqual(registry_bus_call("GetConnectionUnixUser",
                             GLib.Variant("(s)", (registry_owner,)), "(u)"), os.geteuid())
            self.assertEqual(registry_bus_call("GetConnectionUnixProcessID",
                             GLib.Variant("(s)", (registry_owner,)), "(u)"), registry_pid)
            self.assertEqual(registry_bus_call("GetNameOwner",
                             GLib.Variant("(s)", (registry_name,)), "(s)"), registry_owner)
            self.assertEqual(os.readlink(registry_proc / "ns/pid"), inner_namespace)
            self.assertEqual(os.readlink(registry_proc / "exe"), str(registry_exe))
            self.assertEqual(process_start_time(registry_pid), registry_start)
            uid_line = next(line for line in (registry_proc / "status").read_text().splitlines()
                            if line.startswith("Uid:"))
            self.assertEqual(uid_line.split()[1:], [str(os.geteuid())] * 4)
            signal.pidfd_send_signal(pidfd, signal.SIGTERM, None, 0)
        finally:
            os.close(pidfd)
        self.pump_until(lambda: not registry_bus_call("NameHasOwner",
                        GLib.Variant("(s)", (registry_owner,)), "(b)"),
                        "old private registry UNIQUE owner disappeared")
        self.assertIn(registry_bus_call("StartServiceByName",
                      GLib.Variant("(su)", (registry_name, 0)), "(u)"), (1, 2))
        new_registry = registry_bus_call("GetNameOwner", GLib.Variant("(s)", (registry_name,)), "(s)")
        self.assertNotEqual(new_registry, registry_owner)
        third = self.start_accessible_provider()
        restarted = self.ready_editor(third.pid)
        self.assertGreater(restarted[2], fresh[2], "registry loss requires a new bus epoch")
        self.assertEqual(restarted[3], new_registry)
        self.assertNotEqual(restarted[15], fresh[15])
        self.assertEqual(self.observe_editor(fresh[15], pid=third.pid)[0], 2)
        self.xdo("type", "--clearmodifiers", "f")
        self.pump_until(lambda: select.select([third.stdout], [], [], 0)[0],
                        "physical input after actual registry replacement")
        self.assertEqual(third.stdout.readline().strip(), "f")
        typed_after_restart = self.observe_editor(restarted[15], pid=third.pid)
        self.assertEqual(typed_after_restart[0], 0)
        self.assertEqual(typed_after_restart[9], 1)
        self.assertEqual(self.selection_text(Gdk.SELECTION_CLIPBOARD), clipboard_before)
        for key in (KEY_PAUSE, KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_LEFTALT):
            self.assertFalse(self.key_is_down(key))
        self.assertEqual(self.session_bridge_process.pid, helper_pid)
        self.assertIsNone(self.session_bridge_process.poll())


if __name__ == "__main__":
    if len(sys.argv) not in (2, 3):
        print("usage: test_event_loop_gtk_e2e.py DRIVER [SESSION_BRIDGE]",
              file=sys.stderr)
        raise SystemExit(2)
    DRIVER = pathlib.Path(sys.argv[1])
    if not DRIVER.is_file():
        print(f"missing EventLoop e2e driver: {DRIVER}", file=sys.stderr)
        raise SystemExit(2)
    SESSION_BRIDGE = pathlib.Path(sys.argv[2]) if len(sys.argv) == 3 else None
    if SESSION_BRIDGE is not None and not SESSION_BRIDGE.is_file():
        print(f"missing session bridge: {SESSION_BRIDGE}", file=sys.stderr)
        raise SystemExit(2)
    if os.environ.get("PUNTO_EVENT_LOOP_E2E_INNER") != "1":
        raise SystemExit(run_in_sandbox(DRIVER, SESSION_BRIDGE))
    require_private_network_namespace()
    test_case = (AccessibleContextGtkE2E if os.environ.get("PUNTO_ACCESSIBLE_CONTEXT_E2E") == "1"
                 else EventLoopGtkE2E)
    names = unittest.defaultTestLoader.getTestCaseNames(test_case)
    if test_case is AccessibleContextGtkE2E:
        names = [name for name in names if name.startswith("test_accessible_")]
    test_name = os.environ.get("PUNTO_EVENT_LOOP_E2E_TEST")
    if test_name:
        names = test_name.split(",")
    else:
        shard = os.environ.get("PUNTO_EVENT_LOOP_E2E_SHARD")
        if shard:
            index, count = (int(part) for part in shard.split("/", 1))
            if count < 1 or index < 0 or index >= count:
                raise SystemExit(f"invalid PUNTO_EVENT_LOOP_E2E_SHARD={shard}")
            names = [name for position, name in enumerate(names)
                     if position % count == index]
    suite = unittest.TestSuite(test_case(name) for name in names)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(not result.wasSuccessful())
