#!/usr/bin/python3

import json
import os
from pathlib import Path
import select
import signal
import stat
import subprocess
import sys
import tempfile
import threading
import time

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib


ROOT = Path(__file__).resolve().parents[1]
PROTOCOL = ROOT / "protocol" / "punto-dbus-v1.xml"
EXTENSION_PATH = "/org/punto/Switcher/InputSource1"
EXTENSION_IFACE = "org.punto.Switcher.InputSource1"
PEER_PATH = "/org/punto/Switcher/SessionBridge1"
PEER_IFACE = "org.punto.Switcher.SessionBridge1"
OBSERVER_PATH = "/org/punto/Switcher/DesktopObserver1"
OBSERVER_IFACE = "org.punto.Switcher.DesktopObserver1"
EXTENSION_ID = "11" * 16
LEASE_ID = "44" * 16
CLIENT_ID = "33" * 16
DIGEST = "55" * 32


def monotonic_us() -> int:
    return GLib.get_monotonic_time()


def acquire_name(connection: Gio.DBusConnection, name: str) -> None:
    reply = connection.call_sync(
        "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "RequestName",
        GLib.Variant("(su)", (name, 4)), GLib.VariantType("(u)"),
        Gio.DBusCallFlags.NO_AUTO_START, 1000, None)
    if reply.unpack()[0] != 1:
        raise RuntimeError(f"could not acquire {name}")


def release_name(connection: Gio.DBusConnection, name: str) -> None:
    reply = connection.call_sync(
        "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "ReleaseName",
        GLib.Variant("(s)", (name,)), GLib.VariantType("(u)"),
        Gio.DBusCallFlags.NO_AUTO_START, 1000, None)
    if reply.unpack()[0] != 1:
        raise RuntimeError(f"could not release {name}")


def new_message_bus_connection() -> Gio.DBusConnection:
    return Gio.DBusConnection.new_for_address_sync(
        os.environ["DBUS_SESSION_BUS_ADDRESS"],
        Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT |
        Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION,
        None, None)


class FakeAccessibilityStatus:
    def __init__(self) -> None:
        self.connection = Gio.bus_get_sync(Gio.BusType.SESSION, None)
        acquire_name(self.connection, "org.a11y.Bus")
        node = Gio.DBusNodeInfo.new_for_xml(
            '<node><interface name="org.a11y.Status">'
            '<property name="IsEnabled" type="b" access="readwrite"/>'
            '<property name="ScreenReaderEnabled" type="b" access="readwrite"/>'
            '</interface></node>')
        self.registration = self.connection.register_object(
            "/org/a11y/bus", node.interfaces[0], None, self._get, self._set)
        self.reset()

    def reset(self, enabled: bool = True) -> None:
        self.enabled = enabled
        self.calls = []
        self.fault = ""
        self.marker = None
        self.invalidate_session = None
        self.marker_at_set = False
        self.errors = []

    def _get(self, _connection, _sender, _path, _interface, name):
        self.calls.append(("Get", name))
        if name != "IsEnabled":
            self.errors.append("screen reader property read")
            return GLib.Variant("b", False)
        if self.invalidate_session is not None:
            self.invalidate_session.write_text("zero\n", encoding="ascii")
            self.invalidate_session = None
        if self.fault == "get-timeout":
            time.sleep(1.2)
        if self.fault == "shared-budget" and not any(call[0] == "Set" for call in self.calls):
            time.sleep(0.6)
        if self.fault == "get-error":
            raise GLib.Error.new_literal(Gio.dbus_error_quark(),
                                        "status read denied", Gio.DBusError.ACCESS_DENIED)
        if self.fault == "readback-timeout" and any(call[0] == "Set" for call in self.calls):
            time.sleep(1.2)
        if self.fault == "readback-false" and any(call[0] == "Set" for call in self.calls):
            return GLib.Variant("b", False)
        return GLib.Variant("b", self.enabled)

    def _set(self, _connection, _sender, _path, _interface, name, value):
        enabled = value.unpack()
        self.calls.append(("Set", name, enabled))
        if name != "IsEnabled" or not enabled:
            self.errors.append("forbidden status mutation")
            return False
        self.marker_at_set = secure_enrollment_marker(self.marker)
        if not self.marker_at_set:
            self.errors.append("Set preceded secure enrollment marker")
            return False
        if self.fault == "set-denied":
            return False
        self.enabled = True
        if self.fault == "set-timeout":
            time.sleep(1.2)
        if self.fault == "shared-budget":
            time.sleep(0.6)
        return True

    def close(self) -> None:
        self.connection.unregister_object(self.registration)
        release_name(self.connection, "org.a11y.Bus")


def secure_enrollment_marker(path: Path | None) -> bool:
    if path is None:
        return False
    try:
        info = path.lstat()
        parent = path.parent.lstat()
    except FileNotFoundError:
        return False
    return (stat.S_ISREG(info.st_mode) and stat.S_IMODE(info.st_mode) == 0o600
            and info.st_uid == os.getuid() and info.st_size == 0 and info.st_nlink == 1
            and stat.S_ISDIR(parent.st_mode) and stat.S_IMODE(parent.st_mode) == 0o700
            and parent.st_uid == os.getuid())


class FakeExtension:
    def __init__(self) -> None:
        self.connection = Gio.bus_get_sync(Gio.BusType.SESSION, None)
        acquire_name(self.connection, "org.gnome.Shell")
        node = Gio.DBusNodeInfo.new_for_xml(PROTOCOL.read_text(encoding="ascii"))
        interface = node.lookup_interface(EXTENSION_IFACE)
        if interface is None:
            raise RuntimeError("extension interface missing")
        self.calls: list[tuple[str, tuple[object, ...]]] = []
        self.role = 0
        self.state_generation = 8
        self.malformed_switch = False
        self.switch_outcome: int | None = None
        self.cancel_marker: Path | None = None
        self.flood_marker: Path | None = None
        self.replace_event: threading.Event | None = None
        self.operation_result: tuple[object, ...] | None = None
        self.operation_key: tuple[object, ...] | None = None
        self.drop_switch_reply = False
        self.bind_state_file: Path | None = None
        self.bind_state_value = "zero"
        self.switch_state_file: Path | None = None
        self.delay_state_once_ms = 0
        self.delay_state_attempts = 0
        self.bound_helper: str | None = None
        self.pending_invocations: list[object] = []
        self.state_override = None
        self.state_hold_client = None
        self.state_held = threading.Event()
        self.state_revoke_file = None
        self.state_private_delay_ms = 0
        self.registration = self.connection.register_object(
            EXTENSION_PATH, interface, self._call, None, None)

    def _call(self, _connection, _sender, _path, _interface, method,
              parameters, invocation) -> None:
        values = parameters.unpack()
        self.calls.append((method, values))
        if method == "BindHelper":
            self.bound_helper = values[0]
            if self.bind_state_file is not None:
                self.bind_state_file.write_text(
                    self.bind_state_value + "\n", encoding="ascii")
            invocation.return_value(GLib.Variant("(us)", (0, EXTENSION_ID)))
        elif method == "GetState":
            if self.state_revoke_file is not None:
                self.state_revoke_file.write_text("inactive\n", encoding="ascii")
            if values[1] == self.state_hold_client:
                self.pending_invocations.append(invocation)
                self.state_held.set()
                return
            def reply_state() -> bool:
                invocation.return_value(GLib.Variant(
                    "(ussttty)",
                    self.state_override or (0, EXTENSION_ID, LEASE_ID,
                     monotonic_us() + 30_000_000, 7,
                     self.state_generation, self.role)))
                return GLib.SOURCE_REMOVE

            if values[1] == CLIENT_ID and self.state_private_delay_ms > 0:
                GLib.timeout_add(self.state_private_delay_ms, reply_state)
            elif self.delay_state_once_ms > 0 and self.delay_state_attempts > 0:
                delay_ms = self.delay_state_once_ms
                self.delay_state_attempts -= 1
                GLib.timeout_add(delay_ms, reply_state)
            else:
                reply_state()
        elif method == "Switch":
            if self.switch_state_file is not None:
                self.switch_state_file.write_text(
                    "ambiguous\n", encoding="ascii")
            if self.flood_marker is not None:
                self.flood_marker.touch(mode=0o600, exist_ok=False)
            if self.drop_switch_reply:
                self.role = values[10]
                self.state_generation += 1
                self.operation_key = values[1:6]
                self.operation_result = (
                    1, EXTENSION_ID, values[5], 7,
                    self.state_generation, self.role)
                self.pending_invocations.append(invocation)
                return
            if self.cancel_marker is not None:
                self.cancel_marker.touch(mode=0o600, exist_ok=False)
                self.pending_invocations.append(invocation)
                return
            if self.replace_event is not None:
                self.role = values[10]
                self.state_generation += 1
                self.operation_key = values[1:6]
                self.operation_result = (
                    1, EXTENSION_ID, values[5], 7,
                    self.state_generation, self.role)
                self.pending_invocations.append(invocation)
                self.replace_event.set()
                return
            result_digest = ("66" * 32 if values[4] == 2 or
                             self.malformed_switch else values[5])
            if (values[4] != 2 and
                    self.switch_outcome in (None, 18)):
                self.role = values[10]
                self.state_generation += 1
            self.operation_key = values[1:6]
            self.operation_result = (
                1, EXTENSION_ID, values[5], 7,
                self.state_generation, self.role)
            invocation.return_value(GLib.Variant(
                "(usstty)", (self.switch_outcome or 1, EXTENSION_ID,
                              result_digest, 7,
                              self.state_generation, self.role)))
        elif method == "GetOperation":
            if (values[0] != self.bound_helper or
                    values[1:6] != self.operation_key):
                invocation.return_value(GLib.Variant(
                    "(usstty)", (22, EXTENSION_ID, values[5], 7,
                                  self.state_generation, self.role)))
                return
            invocation.return_value(GLib.Variant(
                "(usstty)", self.operation_result or
                (1, EXTENSION_ID, values[5], 7, 9, 1)))
        else:
            invocation.return_dbus_error(
                "org.punto.Test.UnknownMethod", "unknown method")

    def release_name(self) -> None:
        release_name(self.connection, "org.gnome.Shell")

    def acquire_name(self) -> None:
        acquire_name(self.connection, "org.gnome.Shell")


def peer_connection(socket_path: str) -> Gio.DBusConnection:
    return Gio.DBusConnection.new_for_address_sync(
        f"unix:path={socket_path}",
        Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT, None, None)


def peer_client(socket_path: str, helper_id: str, observe: bool = False) -> int:
    connection = peer_connection(socket_path)
    state = connection.call_sync(
        None, PEER_PATH, PEER_IFACE, "GetState",
        GLib.Variant("(st)", (CLIENT_ID, monotonic_us() + 500_000)),
        GLib.VariantType("(usssttty)"), Gio.DBusCallFlags.NONE, 1000, None)
    values = state.unpack()
    if values[0] != 0 or values[1] != helper_id or values[2] != EXTENSION_ID:
        raise RuntimeError(f"unexpected state reply: {values!r}")
    if observe:
        print("ObserveEditor: authenticated GetState control passed", flush=True)
        for deadline, expected in ((monotonic_us() + 300_000, 1),
                                   (monotonic_us() - 1, 3)):
            result = connection.call_sync(
                None, PEER_PATH, PEER_IFACE, "ObserveEditor",
                GLib.Variant("(sutsiis)",
                             (helper_id, os.getpid(), deadline, "", -1, -1, "")),
                GLib.VariantType("(ustssottuiiiiiss)"),
                Gio.DBusCallFlags.NONE, 300, None).unpack()
            if result[0] != expected or result[1] != helper_id or result[15]:
                raise RuntimeError(f"unavailable/expired observation gained authority: {result!r}")
    switch = connection.call_sync(
        None, PEER_PATH, PEER_IFACE, "Switch",
        GLib.Variant("(sssststttyy)",
                     (helper_id, EXTENSION_ID, CLIENT_ID, values[3], 1,
                      DIGEST, monotonic_us() + 500_000, values[5], values[6],
                      values[7], 1)),
        GLib.VariantType("(usstty)"), Gio.DBusCallFlags.NONE, 1000, None)
    switched = switch.unpack()
    if switched != (1, EXTENSION_ID, DIGEST, 7, 9, 1):
        raise RuntimeError(f"unexpected Switch reply: {switched!r}")
    operation = connection.call_sync(
        None, PEER_PATH, PEER_IFACE, "GetOperation",
        GLib.Variant("(sssstst)",
                     (helper_id, EXTENSION_ID, CLIENT_ID, values[3], 1,
                      DIGEST, monotonic_us() + 500_000)),
        GLib.VariantType("(usstty)"), Gio.DBusCallFlags.NONE, 1000, None)
    if operation.unpack() != switched:
        raise RuntimeError("GetOperation did not preserve the terminal reply")
    malformed = connection.call_sync(
        None, PEER_PATH, PEER_IFACE, "Switch",
        GLib.Variant("(sssststttyy)",
                     (helper_id, EXTENSION_ID, CLIENT_ID, values[3], 2,
                      DIGEST, monotonic_us() + 500_000, values[5], values[6],
                      values[7], 1)),
        GLib.VariantType("(usstty)"), Gio.DBusCallFlags.NONE, 1000, None)
    if malformed.unpack()[0] != 22:
        raise RuntimeError("malformed post-dispatch reply lost uncertainty")
    recovered = connection.call_sync(
        None, PEER_PATH, PEER_IFACE, "GetOperation",
        GLib.Variant("(sssstst)",
                     (helper_id, EXTENSION_ID, CLIENT_ID, values[3], 2,
                      DIGEST, monotonic_us() + 500_000)),
        GLib.VariantType("(usstty)"), Gio.DBusCallFlags.NONE, 1000, None)
    if recovered.unpack() != (1, EXTENSION_ID, DIGEST, 7, 9, 1):
        raise RuntimeError("malformed Switch reply was not recoverable by query")
    expired = connection.call_sync(
        None, PEER_PATH, PEER_IFACE, "GetState",
        GLib.Variant("(st)", (CLIENT_ID, monotonic_us() - 1)),
        GLib.VariantType("(usssttty)"), Gio.DBusCallFlags.NONE, 1000, None)
    if expired.unpack()[0] != 21:
        raise RuntimeError("expired request was not rejected before forwarding")
    oversized = connection.call_sync(
        None, PEER_PATH, PEER_IFACE, "GetState",
        GLib.Variant("(st)", ("a" * 2048, monotonic_us() + 500_000)),
        GLib.VariantType("(usssttty)"), Gio.DBusCallFlags.NONE, 1000, None)
    if oversized.unpack()[0] != 17:
        raise RuntimeError("oversized request was not rejected before forwarding")
    print(json.dumps({"state": values, "switch": switched}))
    return 0


def hold_peer(socket_path: str) -> int:
    connection = peer_connection(socket_path)
    print("READY", flush=True)
    sys.stdin.readline()
    connection.close_sync(None)
    return 0


def observer_read(connection, deadline=None):
    return connection.call_sync(
        "org.punto.Switcher.SessionBridge1", OBSERVER_PATH, OBSERVER_IFACE,
        "ReadState", GLib.Variant("(t)", (deadline if deadline is not None
                                          else monotonic_us() + 100_000,)),
        GLib.VariantType("(usstty)"), Gio.DBusCallFlags.NO_AUTO_START,
        300, None).unpack()


def observer_private_four(socket_path: str) -> int:
    peers = [peer_connection(socket_path) for _ in range(4)]
    print("READY", flush=True)
    sys.stdin.readline()
    loop = GLib.MainLoop()
    results = []

    def received(peer, result):
        try:
            results.append(peer.call_finish(result).unpack())
        except GLib.Error as error:
            results.append(str(error))
        if len(results) == 4:
            loop.quit()

    for peer in peers:
        peer.call(
            None, PEER_PATH, PEER_IFACE, "GetState",
            GLib.Variant("(st)", (CLIENT_ID, monotonic_us() + 100_000)),
            GLib.VariantType("(usssttty)"), Gio.DBusCallFlags.NONE,
            250, None, received)
    bound = GLib.timeout_add(300, lambda: (loop.quit(), GLib.SOURCE_REMOVE)[1])
    loop.run()
    if len(results) != 4 or any(not isinstance(value, tuple) or value[0] != 0
                                for value in results):
        raise RuntimeError(f"private concurrent capacity blocked: {results!r}")
    GLib.source_remove(bound)
    print(f"FOUR_PRIVATE_STATES {monotonic_us()}", flush=True)
    for peer in peers:
        peer.close_sync(None)
    return 0


def run_desktop_observer_contract(helper: Path) -> int:
    extension = FakeExtension()
    status = FakeAccessibilityStatus()
    loop = GLib.MainLoop()
    thread = threading.Thread(target=loop.run, daemon=True)
    thread.start()
    process = None
    descriptor = None
    try:
        with tempfile.TemporaryDirectory(prefix="punto-desktop-observer-") as root_value:
            root = Path(root_value)
            root.chmod(0o700)
            state = root / "state"
            state.mkdir(mode=0o700)
            session_state = root / "session"
            session_state.write_text("user-manager\n", encoding="ascii")
            env = os.environ.copy()
            env.update(XDG_RUNTIME_DIR=str(root), STATE_DIRECTORY=str(state),
                       PUNTO_TEST_SESSION_STATE_FILE=str(session_state))
            status.marker = state / "accessibility-enrolled"
            process = subprocess.Popen([str(helper)], env=env,
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            descriptor = wait_for_descriptor(root / "punto/session-bridge.json", process)
            connection = new_message_bus_connection()
            for role in (0, 1):
                extension.role = role
                before = len(extension.calls)
                probe = subprocess.run([str(helper), "--check-ready"], env=env,
                                       capture_output=True, text=True, timeout=1)
                if probe.returncode != 0:
                    raise RuntimeError(f"desktop readiness probe refused ready role{role}: {probe.stderr!r}")
                if [method for method, _ in extension.calls[before:]] != ["GetState"]:
                    raise RuntimeError("desktop readiness probe changed helper/control state")
            extension.role = 0
            for outcome, extension_id, role in ((15, EXTENSION_ID, 0),
                                                (0, "invalid", 0),
                                                (0, EXTENSION_ID, 255)):
                extension.state_override = (outcome, extension_id, LEASE_ID,
                                            monotonic_us() + 30_000_000, 7, 8, role)
                probe = subprocess.run([str(helper), "--check-ready"], env=env,
                                       capture_output=True, text=True, timeout=1)
                if probe.returncode != 75:
                    raise RuntimeError("desktop readiness probe accepted unavailable state")
            extension.state_override = None
            extension.delay_state_once_ms = 150
            extension.delay_state_attempts = 1
            probe = subprocess.run([str(helper), "--check-ready"], env=env,
                                   capture_output=True, text=True, timeout=1)
            if probe.returncode != 75:
                raise RuntimeError("desktop readiness probe accepted a late response")
            time.sleep(0.06)
            from test_cli_contract import CliHarness
            harness = CliHarness()
            try:
                started = harness.run("start", PUNTO_SESSION_BRIDGE=str(helper),
                                      PUNTO_START_TIMEOUT_MS="500",
                                      PUNTO_IPC_TIMEOUT_MS="200")
                if started.returncode != 0 or harness.service_state.read_text() != "active\n":
                    raise RuntimeError(f"CLI did not admit the real desktop probe: {started.stdout!r} {started.stderr!r}")
            finally:
                harness.cleanup()
            before = len(extension.calls)
            for _ in range(3):
                value = observer_read(connection)
                if value != (0, descriptor["helper_instance"], EXTENSION_ID, 7, 8, 0):
                    raise RuntimeError(f"public projection gained/lost fields: {value!r}")
            calls = extension.calls[before:]
            clients = {values[1] for method, values in calls if method == "GetState"}
            if len(clients) != 1 or [method for method, _ in calls] != ["GetState"] * 3:
                raise RuntimeError("public reads changed control state or allocated caller identities")
            internal_client = clients.pop()
            if internal_client == CLIENT_ID:
                raise RuntimeError("public observation reused private client identity")
            for deadline in (monotonic_us() - 1, monotonic_us() + 200_000):
                before = len(extension.calls)
                if observer_read(connection, deadline)[0] != 21 or len(extension.calls) != before:
                    raise RuntimeError("invalid observer deadline forwarded")
            for budget in (100, 500, 1_000, 2_000):
                short = observer_read(connection, monotonic_us() + budget)
                if short[0] not in (0, 21) or process.poll() is not None:
                    raise RuntimeError(f"expiry was mistaken for helper authority drift: {short!r}")
                if observer_read(connection)[0] != 0 or process.poll() is not None:
                    raise RuntimeError("same helper did not recover after short deadline")
            before = len(extension.calls)
            try:
                connection.call_sync(
                    "org.punto.Switcher.SessionBridge1", OBSERVER_PATH, OBSERVER_IFACE,
                    "ReadState", GLib.Variant("(s)", ("invalid",)), None,
                    Gio.DBusCallFlags.NONE, 300, None)
                raise RuntimeError("wrong wire signature admitted")
            except GLib.Error:
                pass
            if len(extension.calls) != before:
                raise RuntimeError("malformed observation reached upstream")
            before = len(extension.calls)
            foreign = subprocess.run(
                ["sudo", "-n", "env", "DBUS_SESSION_BUS_ADDRESS=" +
                 os.environ["DBUS_SESSION_BUS_ADDRESS"], "/usr/bin/python3",
                 str(Path(__file__).resolve()), "--observer-foreign"],
                capture_output=True, text=True, timeout=2)
            if foreign.returncode != 0 or len(extension.calls) != before:
                raise RuntimeError(f"foreign observer reached upstream: {foreign.stderr!r}")
            for reply, expected in (
                    ((14, EXTENSION_ID, "", 0, 7, 8, 255), 14),
                    ((0, EXTENSION_ID, "bad", monotonic_us() + 30_000_000, 7, 8, 0), 17),
                    ((0, EXTENSION_ID, LEASE_ID, monotonic_us() - 1, 7, 8, 0), 17),
                    ((0, EXTENSION_ID, LEASE_ID, monotonic_us() + 30_000_000, 7, 8, 2), 17)):
                extension.state_override = reply
                result = observer_read(connection)
                if result[0] != expected or result[5] != 255:
                    raise RuntimeError(f"bad upstream state exposed authority: {result!r}")
            extension.state_override = None
            if observer_read(connection)[0] != 0:
                raise RuntimeError("safe refusal poisoned next observer read")
            private = subprocess.Popen(
                ["sudo", "-n", "/usr/bin/python3", str(Path(__file__).resolve()),
                 "--observer-private-four", str(descriptor["socket_path"])],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            try:
                ready, _, _ = select.select([private.stdout], [], [], 2)
                if not ready or private.stdout.readline().strip() != "READY":
                    raise RuntimeError("private four-slot control did not preconnect")
                extension.state_hold_client = internal_client
                extension.state_private_delay_ms = 20
                held_result = []
                held_deadline = monotonic_us() + 100_000
                held = threading.Thread(target=lambda: held_result.append(observer_read(connection, held_deadline)))
                held.start()
                if not extension.state_held.wait(0.05):
                    raise RuntimeError("public upstream barrier not reached")
                busy_connection = new_message_bus_connection()
                try:
                    observer_read(busy_connection)
                    raise RuntimeError("second public read bypassed single-flight")
                except GLib.Error as error:
                    if "Busy" not in str(error):
                        raise
                finally:
                    busy_connection.close_sync(None)
                output, error = private.communicate("GO\n", timeout=1)
                held.join(timeout=1)
                if (private.returncode != 0 or "FOUR_PRIVATE_STATES" not in output
                        or int(output.split("FOUR_PRIVATE_STATES ")[1].split()[0]) >= held_deadline
                        or held.is_alive() or not held_result or held_result[0][0] != 21):
                    raise RuntimeError(f"public/private deadline-capacity control failed: {output!r} {error!r}")
                extension.state_hold_client = None
                extension.state_private_delay_ms = 0
                if observer_read(connection)[0] != 0:
                    raise RuntimeError("timeout prevented recovery")
            finally:
                if private.poll() is None:
                    private.terminate()
                    private.communicate(timeout=2)
            extension.state_held.clear()
            extension.state_hold_client = internal_client
            disconnected = new_message_bus_connection()
            disconnected.call(
                "org.punto.Switcher.SessionBridge1", OBSERVER_PATH, OBSERVER_IFACE,
                "ReadState", GLib.Variant("(t)", (monotonic_us() + 100_000,)),
                GLib.VariantType("(usstty)"), Gio.DBusCallFlags.NONE, 300, None, None)
            if not extension.state_held.wait(0.05):
                raise RuntimeError("disconnect barrier missing")
            disconnected.close_sync(None)
            time.sleep(0.02)
            extension.state_hold_client = None
            if observer_read(connection)[0] != 0:
                raise RuntimeError("disconnected caller retained public slot")
            extension.state_revoke_file = session_state
            before = len(extension.calls)
            try:
                revoked = observer_read(connection)
            except GLib.Error:
                revoked = None
            if ((revoked is not None and revoked[0] == 0) or
                    session_state.read_text(encoding="ascii") != "inactive\n" or
                    [method for method, _ in extension.calls[before:]] != ["GetState"]):
                raise RuntimeError("post-forward session revocation returned fresh observation")
            process.wait(timeout=2)
            if process.returncode != 1:
                raise RuntimeError("session drift retained stale helper")
            extension.state_revoke_file = None
            session_state.write_text("user-manager\n", encoding="ascii")
            process = subprocess.Popen([str(helper)], env=env,
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            descriptor = wait_for_descriptor(root / "punto/session-bridge.json", process)
            ready = observer_read(connection)
            if ready[0] != 0 or ready[1] != descriptor["helper_instance"]:
                raise RuntimeError(f"published helper observer is not ready: {ready!r}")
            before = len(extension.calls)
            session_state.write_text("inactive\n", encoding="ascii")
            try:
                revoked = observer_read(connection)
            except GLib.Error:
                revoked = None
            if (revoked is not None and revoked[0] == 0) or len(extension.calls) != before:
                raise RuntimeError("pre-forward session revocation crossed upstream admission")
            process.wait(timeout=2)
            if process.returncode != 1:
                raise RuntimeError("pre-forward session drift retained helper")
            for termination in ("owner", "shutdown"):
                session_state.write_text("user-manager\n", encoding="ascii")
                process = subprocess.Popen([str(helper)], env=env,
                                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                descriptor = wait_for_descriptor(root / "punto/session-bridge.json", process)
                ready = observer_read(connection)
                if ready[0] != 0 or ready[1] != descriptor["helper_instance"]:
                    raise RuntimeError(f"published helper observer is not ready: {ready!r}")
                extension.state_hold_client = extension.calls[-1][1][1]
                extension.state_held.clear()
                results = []

                def receive_terminated_read():
                    try:
                        results.append(observer_read(connection))
                    except GLib.Error:
                        results.append("DBusError")

                receiver = threading.Thread(target=receive_terminated_read)
                receiver.start()
                if not extension.state_held.wait(0.05):
                    raise RuntimeError(f"{termination} callback barrier missing")
                if termination == "owner":
                    replacement = new_message_bus_connection()
                    extension.release_name()
                    acquire_name(replacement, "org.gnome.Shell")
                    try:
                        process.wait(timeout=2)
                        if process.returncode != 1:
                            raise RuntimeError("Shell replacement did not retire pinned helper")
                    finally:
                        release_name(replacement, "org.gnome.Shell")
                        replacement.close_sync(None)
                        extension.acquire_name()
                else:
                    process.send_signal(signal.SIGTERM)
                    process.wait(timeout=2)
                    if process.returncode != 0:
                        raise RuntimeError("shutdown failed to retire public callback")
                receiver.join(timeout=1)
                if receiver.is_alive() or not results or (
                        results[0] != "DBusError" and results[0][0] == 0):
                    raise RuntimeError(f"{termination} returned late public success")
                extension.state_hold_client = None
            connection.close_sync(None)
            probe = subprocess.run([str(helper), "--check-ready"], env=env,
                                   capture_output=True, text=True, timeout=1)
            if probe.returncode != 75:
                raise RuntimeError("desktop readiness probe accepted absent helper")
    except BaseException:
        diagnostic = {"helper_pid": process.pid if process is not None else None,
                      "process_poll": process.poll() if process is not None else None,
                      "descriptor": descriptor}
        try:
            diagnostic["bus_owner"] = extension.connection.call_sync(
                "org.freedesktop.DBus", "/org/freedesktop/DBus",
                "org.freedesktop.DBus", "GetNameOwner",
                GLib.Variant("(s)", ("org.punto.Switcher.SessionBridge1",)),
                GLib.VariantType("(s)"), Gio.DBusCallFlags.NO_AUTO_START,
                100, None).unpack()[0]
        except GLib.Error as error:
            diagnostic["bus_owner_error"] = str(error)
        print("observer_failure_context=" + json.dumps(diagnostic), file=sys.stderr)
        raise
    finally:
        if process is not None:
            before_cleanup = process.poll()
            if before_cleanup is None:
                process.terminate()
            output, error = process.communicate(timeout=2)
            print("observer_helper_terminal=" + json.dumps({
                "pid": process.pid, "before_cleanup": before_cleanup,
                "exit_code": process.returncode, "stdout": output, "stderr": error}),
                file=sys.stderr)
        status.close()
        extension.connection.unregister_object(extension.registration)
        extension.release_name()
        loop.quit()
        thread.join(timeout=2)
    print("test_desktop_observer_isolated_gdbus: OK")
    return 0


def probe_peer(socket_path: str, helper_id: str) -> int:
    deadline = monotonic_us() + 100_000
    try:
        connection = peer_connection(socket_path)
        state = connection.call_sync(
            None, PEER_PATH, PEER_IFACE, "GetState",
            GLib.Variant("(st)", (CLIENT_ID, deadline)),
            GLib.VariantType("(usssttty)"), Gio.DBusCallFlags.NONE, 250, None
        ).unpack()
        observed = monotonic_us()
        connection.close_sync(None)
        if (observed >= deadline or state[0] != 0 or
                state[1] != helper_id or state[2] != EXTENSION_ID or
                state[3] != LEASE_ID or state[4] <= observed or
                state[7] not in (0, 1)):
            return 2
    except GLib.Error:
        return 2
    return 0


def peer_state_outcome(socket_path: str, expected: int) -> int:
    connection = peer_connection(socket_path)
    state = connection.call_sync(
        None, PEER_PATH, PEER_IFACE, "GetState",
        GLib.Variant("(st)", (CLIENT_ID, monotonic_us() + 500_000)),
        GLib.VariantType("(usssttty)"), Gio.DBusCallFlags.NONE, 1000, None)
    if state.unpack()[0] != expected:
        raise RuntimeError(f"unexpected state outcome: {state.unpack()!r}")
    try:
        connection.close_sync(None)
    except GLib.Error:
        pass
    return 0


def peer_switch_then_revocation(socket_path: str, helper_id: str) -> int:
    connection = peer_connection(socket_path)
    state = connection.call_sync(
        None, PEER_PATH, PEER_IFACE, "GetState",
        GLib.Variant("(st)", (CLIENT_ID, monotonic_us() + 500_000)),
        GLib.VariantType("(usssttty)"), Gio.DBusCallFlags.NONE, 1000, None
    ).unpack()
    switched = connection.call_sync(
        None, PEER_PATH, PEER_IFACE, "Switch",
        GLib.Variant("(sssststttyy)",
                     (helper_id, EXTENSION_ID, CLIENT_ID, state[3], 1,
                      DIGEST, monotonic_us() + 500_000, state[5], state[6],
                      state[7], 1)),
        GLib.VariantType("(usstty)"), Gio.DBusCallFlags.NONE, 1000, None
    ).unpack()
    if switched[0] != 1:
        raise RuntimeError(f"attempted Switch lost terminal result: {switched!r}")
    operation = connection.call_sync(
        None, PEER_PATH, PEER_IFACE, "GetOperation",
        GLib.Variant("(sssstst)",
                     (helper_id, EXTENSION_ID, CLIENT_ID, state[3], 1,
                      DIGEST, monotonic_us() + 500_000)),
        GLib.VariantType("(usstty)"), Gio.DBusCallFlags.NONE, 1000, None
    ).unpack()
    if operation[0] != 15:
        raise RuntimeError(f"revoked follow-up was forwarded: {operation!r}")
    return 0


def _write_proc_file(path: Path, data: bytes | str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() or path.is_symlink():
        path.unlink()
    if isinstance(data, str):
        path.write_text(data, encoding="ascii")
    else:
        path.write_bytes(data)
    path.chmod(0o444)


def _uid_map_line(ns_uid: int, host_uid: int, count: int) -> str:
    return f"{ns_uid:10d}{host_uid:11d}{count:11d}\n"


def _authorization_environment(proc_root: Path, daemon_exe: Path | None) -> dict[str, str]:
    environment = os.environ.copy()
    environment["PUNTO_TEST_PROC_ROOT"] = str(proc_root)
    if daemon_exe is not None:
        environment["PUNTO_TEST_DAEMON_EXE"] = str(daemon_exe)
    else:
        environment.pop("PUNTO_TEST_DAEMON_EXE", None)
    return environment


def _protected_daemon_exe(root: Path) -> Path:
    daemon = root / "punto-daemon"
    daemon.write_bytes(b"\x7fELF")
    daemon.chmod(0o755)
    return daemon


def _daemon_peer_tree(proc_root: Path, pid: int, *, cgroup: str,
                      cmdline: bytes, comm: str, daemon_exe: Path) -> None:
    peer = proc_root / str(pid)
    peer.mkdir(parents=True, exist_ok=True)
    _write_proc_file(peer / "cgroup", cgroup)
    _write_proc_file(peer / "cmdline", cmdline)
    _write_proc_file(peer / "comm", comm)
    exe = peer / "exe"
    if exe.exists() or exe.is_symlink():
        exe.unlink()
    exe.symlink_to(daemon_exe)


def probe_authorize_peer(helper: Path, peer_uid: int, peer_pid: int,
                         environment: dict[str, str]) -> tuple[int, str]:
    result = subprocess.run(
        [str(helper), "--authorize-peer", str(peer_uid), str(peer_pid)],
        env=environment, check=False, capture_output=True, text=True, timeout=2)
    return result.returncode, result.stdout.strip()


def expect_authorize_peer(helper: Path, peer_uid: int, peer_pid: int,
                          environment: dict[str, str], accepted: bool,
                          label: str) -> None:
    code, output = probe_authorize_peer(helper, peer_uid, peer_pid, environment)
    expected = "accepted=1" if accepted else "accepted=0"
    if output != expected or (code == 0) != accepted:
        raise RuntimeError(
            f"{label}: output={output!r} code={code}, expected {expected}")


def check_authorization_seam(helper: Path) -> None:
    overflow_uid = 65534
    self_uid = os.getuid()
    peer_pid = 4242
    with tempfile.TemporaryDirectory(prefix="punto-bridge-auth-") as root_name:
        root = Path(root_name)
        proc_root = root / "proc"
        daemon_exe = _protected_daemon_exe(root)
        _write_proc_file(proc_root / "sys" / "kernel" / "overflowuid",
                         f"{overflow_uid}\n")
        host_root_env = _authorization_environment(proc_root, daemon_exe)
        _write_proc_file(proc_root / "self" / "uid_map",
                         _uid_map_line(0, 0, 4294967295))
        expect_authorize_peer(helper, 0, 1, host_root_env, True,
                              "literal UID 0 in the host-root UID namespace")

        _write_proc_file(proc_root / "self" / "uid_map",
                         _uid_map_line(self_uid, self_uid, 1))
        expect_authorize_peer(
            helper, 0, 1, host_root_env, False,
            "literal UID 0 without a host-root namespace mapping")
        _daemon_peer_tree(
            proc_root, peer_pid,
            cgroup="0::/system.slice/udevmon.service\n",
            cmdline=os.fsencode(daemon_exe) + b"\x00",
            comm="punto-daemon\n", daemon_exe=daemon_exe)
        mapped_env = _authorization_environment(proc_root, daemon_exe)
        expect_authorize_peer(
            helper, overflow_uid, peer_pid, mapped_env, True,
            "overflow UID with namespace-mapped genuine daemon provenance")
        expect_authorize_peer(
            helper, self_uid, peer_pid, mapped_env, False,
            "same-user peer with daemon provenance")
        expect_authorize_peer(
            helper, overflow_uid, 0, mapped_env, False,
            "overflow UID without peer PID provenance")

        _write_proc_file(proc_root / "self" / "uid_map",
                         _uid_map_line(self_uid, self_uid, 1) +
                         _uid_map_line(0, 0, 1))
        expect_authorize_peer(
            helper, overflow_uid, peer_pid, mapped_env, False,
            "overflow UID with a non-singleton helper mapping")
        _write_proc_file(proc_root / "self" / "uid_map",
                         _uid_map_line(self_uid, self_uid, 1))

        _write_proc_file(proc_root / str(peer_pid) / "comm", "not-punto\n")
        expect_authorize_peer(
            helper, overflow_uid, peer_pid, mapped_env, False,
            "overflow UID with wrong process name")
        _write_proc_file(proc_root / str(peer_pid) / "comm", "punto-daemon\n")

        _write_proc_file(proc_root / str(peer_pid) / "cmdline",
                         os.fsencode(daemon_exe) + b"\x00--other\x00")
        expect_authorize_peer(
            helper, overflow_uid, peer_pid, mapped_env, False,
            "overflow UID with extra daemon arguments")
        _write_proc_file(proc_root / str(peer_pid) / "cmdline",
                         os.fsencode(daemon_exe) + b"\x00")

        daemon_exe.chmod(0o777)
        expect_authorize_peer(
            helper, overflow_uid, peer_pid, mapped_env, False,
            "overflow UID with writable daemon executable")
        daemon_exe.chmod(0o755)

        _write_proc_file(proc_root / str(peer_pid) / "cgroup",
                         "0::/system.slice/ssh.service\n")
        unproven_env = _authorization_environment(proc_root, daemon_exe)
        expect_authorize_peer(
            helper, overflow_uid, peer_pid, unproven_env, False,
            "overflow UID without genuine daemon cgroup provenance")


def wait_for_descriptor(path: Path, process: subprocess.Popen,
                        timeout: float = 3.0) -> dict[str, object]:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.is_file() and not path.is_symlink():
            value = json.loads(path.read_text(encoding="ascii"))
            if value.get("pid") == process.pid:
                return value
        if process.poll() is not None:
            stdout, stderr = process.communicate()
            raise RuntimeError(
                f"helper exited {process.returncode}: {stdout!r} {stderr!r}")
        time.sleep(0.02)
    raise RuntimeError("helper descriptor was not published")


def check_accessibility_contract(helper: Path, status: FakeAccessibilityStatus) -> None:
    with tempfile.TemporaryDirectory(prefix="punto-accessibility-") as root_value:
        root = Path(root_value)
        root.chmod(0o700)
        environment = os.environ.copy()
        environment["XDG_RUNTIME_DIR"] = str(root)
        environment["PUNTO_TEST_SESSION_MODE"] = "user-manager"
        environment.pop("PUNTO_TEST_SESSION_STATE_FILE", None)
        descriptor = root / "punto" / "session-bridge.json"

        def state_case(name: str, enabled: bool = False):
            state = root / name
            state.mkdir(mode=0o700)
            status.reset(enabled)
            status.marker = state / "accessibility-enrolled"
            env = environment.copy()
            env["STATE_DIRECTORY"] = str(state)
            return state, env

        def launch(env, ready: bool):
            started = time.monotonic()
            process = subprocess.Popen([str(helper)], env=env,
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            try:
                if ready:
                    wait_for_descriptor(descriptor, process)
                    process.send_signal(signal.SIGTERM)
                stdout, stderr = process.communicate(timeout=3)
                expected = 0 if ready else 75
                if process.returncode != expected or descriptor.exists():
                    raise RuntimeError(f"accessibility startup expected {expected}: "
                                       f"exit={process.returncode} stderr={stderr!r}")
                if status.errors:
                    raise RuntimeError(f"accessibility fixture contract: {status.errors!r}")
                return stderr, time.monotonic() - started
            finally:
                if process.poll() is None:
                    process.terminate()
                    process.communicate(timeout=3)

        state, env = state_case("first-false")
        launch(env, True)
        if (not secure_enrollment_marker(status.marker) or not status.marker_at_set
                or status.calls != [("Get", "IsEnabled"), ("Set", "IsEnabled", True),
                                    ("Get", "IsEnabled")] or not status.enabled):
            raise RuntimeError("first false startup did not durably enroll before one true Set/readback")
        enrollment = status.marker.stat()
        status.calls.clear()
        launch(env, True)
        if (status.calls != [("Get", "IsEnabled")] or
                status.marker.stat().st_ino != enrollment.st_ino):
            raise RuntimeError("restart/update rewrote enrollment or repeated Set")
        status.enabled = False
        status.calls.clear()
        warning, _elapsed = launch(env, True)
        if (status.calls != [("Get", "IsEnabled")] or status.enabled
                or "accessibility-disabled" not in warning):
            raise RuntimeError("explicit disable after enrollment was overridden or hidden")
        print("A11Y first-false/restart/update/explicit-disable PASS", flush=True)

        _state, env = state_case("already-true", True)
        launch(env, True)
        if not secure_enrollment_marker(status.marker) or status.calls != [("Get", "IsEnabled")]:
            raise RuntimeError("already-true startup failed enrollment or issued Set")
        print("A11Y already-true/no-exit-reset PASS", flush=True)

        for fault in ("get-error", "get-timeout", "set-denied", "set-timeout",
                      "readback-false", "readback-timeout", "shared-budget"):
            _state, env = state_case(fault)
            status.fault = fault
            _stderr, elapsed = launch(env, False)
            if elapsed > 1.6:
                raise RuntimeError(f"{fault} exceeded shared startup bound: {elapsed:.3f}s")
            premarker = fault.startswith("get-")
            if status.marker.exists() == premarker:
                raise RuntimeError(f"{fault} violated enrollment effect boundary")
            if premarker and any(call[0] == "Set" for call in status.calls):
                raise RuntimeError(f"{fault} wrote status after failed initial read")
            # A late public reply cannot make the failed helper publish later.
            time.sleep(0.25)
            if descriptor.exists():
                raise RuntimeError(f"{fault} published after startup failure")
            if not premarker:
                status.enabled = False
                status.fault = ""
                status.calls.clear()
                warning, _elapsed = launch(env, True)
                if any(call[0] == "Set" for call in status.calls) or "accessibility-disabled" not in warning:
                    raise RuntimeError(f"{fault} retried mutation after durable enrollment")
            print(f"A11Y {fault} PASS", flush=True)

        state, env = state_case("session-revoked-at-get")
        authority = root / "a11y-session-state"
        authority.write_text("user-manager\n", encoding="ascii")
        env["PUNTO_TEST_SESSION_STATE_FILE"] = str(authority)
        status.invalidate_session = authority
        launch(env, False)
        if status.marker.exists() or any(call[0] == "Set" for call in status.calls):
            raise RuntimeError("authority lost during initial Get crossed enrollment/Set boundary")
        print("A11Y initial-Get-session-revocation PASS", flush=True)

        for kind in ("missing-state", "absent-state", "unsafe-state", "state-symlink", "marker-symlink",
                     "marker-mode", "marker-nonempty", "marker-fifo", "marker-hardlink"):
            state, env = state_case(kind)
            if kind == "missing-state":
                env.pop("STATE_DIRECTORY")
            elif kind == "absent-state":
                env["STATE_DIRECTORY"] = str(root / "does-not-exist")
            elif kind == "unsafe-state":
                state.chmod(0o755)
            elif kind == "state-symlink":
                link = root / "linked-state"
                link.symlink_to(state, target_is_directory=True)
                env["STATE_DIRECTORY"] = str(link)
            elif kind == "marker-symlink":
                target = root / "marker-target"
                target.touch(mode=0o600)
                status.marker.symlink_to(target)
            elif kind == "marker-fifo":
                os.mkfifo(status.marker, mode=0o600)
            elif kind == "marker-hardlink":
                target = root / "hardlinked-marker-target"
                target.touch(mode=0o600)
                os.link(target, status.marker)
            else:
                status.marker.touch(mode=0o600)
                if kind == "marker-mode":
                    status.marker.chmod(0o644)
                else:
                    status.marker.write_text("not empty", encoding="ascii")
            _stderr, elapsed = launch(env, False)
            if elapsed > 1.6:
                raise RuntimeError(f"{kind} blocked startup: {elapsed:.3f}s")
            if any(call[0] == "Set" for call in status.calls):
                raise RuntimeError(f"{kind} reached status mutation")
            print(f"A11Y {kind} PASS", flush=True)

        for mode in ("wrong-uid", "inactive", "remote", "wrong-seat"):
            _state, env = state_case("foreign-" + mode)
            env["PUNTO_TEST_SESSION_MODE"] = mode
            launch(env, False)
            if status.marker.exists() or any(call[0] == "Set" for call in status.calls):
                raise RuntimeError(f"foreign session {mode} changed accessibility/enrollment")
        print("A11Y foreign-session/no-screen-reader-access PASS", flush=True)


def run_accessibility_contract(helper: Path) -> int:
    extension = FakeExtension()
    status = FakeAccessibilityStatus()
    loop = GLib.MainLoop()
    thread = threading.Thread(target=loop.run, daemon=True)
    thread.start()
    try:
        check_accessibility_contract(helper, status)
    finally:
        status.close()
        extension.connection.unregister_object(extension.registration)
        extension.release_name()
        loop.quit()
        thread.join(timeout=2)
    print("test_session_bridge_accessibility_contract: OK")
    return 0


def run_contract(helper: Path, client: Path) -> int:
    check_authorization_seam(helper)
    extension = FakeExtension()
    status = FakeAccessibilityStatus()
    loop = GLib.MainLoop()
    thread = threading.Thread(target=loop.run, daemon=True)
    thread.start()
    check_accessibility_contract(helper, status)
    status.reset(True)
    with tempfile.TemporaryDirectory(prefix="punto-session-bridge-") as root:
        runtime = Path(root)
        runtime.chmod(0o700)
        environment = os.environ.copy()
        environment["XDG_RUNTIME_DIR"] = str(runtime)
        state_directory = runtime / "accessibility-state"
        state_directory.mkdir(mode=0o700)
        environment["STATE_DIRECTORY"] = str(state_directory)
        status.marker = state_directory / "accessibility-enrolled"
        environment.pop("PUNTO_TEST_SESSION_MODE", None)
        expected_exit = {
            "inactive": 75,
            "zero": 75,
            "ambiguous": 75,
            "inventory-error": 75,
            "metadata-error": 75,
            "wrong-uid": 75,
            "wayland": 75,
            "remote": 75,
            "greeter": 75,
            "wrong-seat": 75,
            "lookup-error": 75,
            "direct-contradictory": 64,
        }
        for mode, exit_status in expected_exit.items():
            rejected_environment = environment.copy()
            rejected_environment["PUNTO_TEST_SESSION_MODE"] = mode
            rejected = subprocess.run(
                [str(helper)], env=rejected_environment, check=False,
                capture_output=True, text=True, timeout=3)
            if rejected.returncode != exit_status:
                raise RuntimeError(
                    f"session mode {mode} returned {rejected.returncode}, "
                    f"expected {exit_status}")
        extension.release_name()
        shell_absent = subprocess.run(
            [str(helper)], env=environment, check=False,
            capture_output=True, text=True, timeout=3)
        extension.acquire_name()
        if shell_absent.returncode != 75:
            raise RuntimeError("temporary Shell absence was not retryable")

        punto_runtime = runtime / "punto"
        punto_runtime.mkdir(mode=0o755)
        invalid_runtime = subprocess.run(
            [str(helper)], env=environment, check=False,
            capture_output=True, text=True, timeout=3)
        punto_runtime.chmod(0o700)
        if invalid_runtime.returncode != 64:
            raise RuntimeError("invalid runtime permissions were not permanent")

        state_file = runtime / "session-state"
        state_file.write_text("user-manager\n", encoding="ascii")
        state_environment = environment.copy()
        state_environment["PUNTO_TEST_SESSION_STATE_FILE"] = str(state_file)

        extension.bind_state_file = state_file
        startup_race = subprocess.run(
            [str(helper)], env=state_environment, check=False,
            capture_output=True, text=True, timeout=3)
        extension.bind_state_file = None
        if startup_race.returncode != 75:
            raise RuntimeError(
                f"startup session race returned {startup_race.returncode}, "
                "expected retryable rejection")
        descriptor_path = runtime / "punto" / "session-bridge.json"
        if descriptor_path.exists():
            raise RuntimeError("startup session race published a descriptor")

        state_file.write_text("user-manager\n", encoding="ascii")
        barrier_ready = runtime / "pre-publish-ready"
        barrier_release = runtime / "pre-publish-release"
        barrier_environment = state_environment.copy()
        barrier_environment["PUNTO_TEST_PRE_PUBLISH_READY"] = str(barrier_ready)
        barrier_environment["PUNTO_TEST_PRE_PUBLISH_RELEASE"] = str(
            barrier_release)
        publication_race = subprocess.Popen(
            [str(helper)], env=barrier_environment, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True)
        barrier_deadline = time.monotonic() + 2
        while time.monotonic() < barrier_deadline and not barrier_ready.exists():
            if publication_race.poll() is not None:
                stdout, stderr = publication_race.communicate()
                raise RuntimeError(
                    f"pre-publication helper exited early: {stdout!r} {stderr!r}")
            time.sleep(0.005)
        if not barrier_ready.exists():
            publication_race.kill()
            raise RuntimeError("pre-publication barrier was not reached")
        state_file.write_text("ambiguous\n", encoding="ascii")
        barrier_release.write_text("release\n", encoding="ascii")
        publication_race.wait(timeout=3)
        if publication_race.returncode != 75 or descriptor_path.exists():
            raise RuntimeError("stale authority crossed descriptor publication")
        if list((runtime / "punto").glob("session-bridge-*.sock")):
            raise RuntimeError("pre-publication rejection retained its socket")

        state_file.write_text("user-manager\n", encoding="ascii")
        owner_barrier_ready = runtime / "owner-pre-publish-ready"
        owner_barrier_release = runtime / "owner-pre-publish-release"
        owner_barrier_environment = state_environment.copy()
        owner_barrier_environment["PUNTO_TEST_PRE_PUBLISH_READY"] = str(
            owner_barrier_ready)
        owner_barrier_environment["PUNTO_TEST_PRE_PUBLISH_RELEASE"] = str(
            owner_barrier_release)
        owner_race = subprocess.Popen(
            [str(helper)], env=owner_barrier_environment,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        owner_deadline = time.monotonic() + 2
        while time.monotonic() < owner_deadline and not owner_barrier_ready.exists():
            if owner_race.poll() is not None:
                stdout, stderr = owner_race.communicate()
                raise RuntimeError(
                    f"owner-race helper exited early: {stdout!r} {stderr!r}")
            time.sleep(0.005)
        if not owner_barrier_ready.exists():
            owner_race.kill()
            raise RuntimeError("owner-race barrier was not reached")
        replacement_owner = new_message_bus_connection()
        extension.release_name()
        acquire_name(replacement_owner, "org.gnome.Shell")
        owner_barrier_release.write_text("release\n", encoding="ascii")
        owner_race.wait(timeout=3)
        release_name(replacement_owner, "org.gnome.Shell")
        replacement_owner.close_sync(None)
        extension.acquire_name()
        if owner_race.returncode != 75 or descriptor_path.exists():
            raise RuntimeError("replaced Shell owner crossed descriptor publication")
        if list((runtime / "punto").glob("session-bridge-*.sock")):
            raise RuntimeError("owner-race rejection retained its socket")

        revoked_descriptor = None
        for revocation_mode in ("ambiguous", "inactive", "disappeared"):
            state_file.write_text("user-manager\n", encoding="ascii")
            revoked_process = subprocess.Popen(
                [str(helper)], env=state_environment, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, text=True)
            revoked_descriptor = wait_for_descriptor(
                descriptor_path, revoked_process)
            revoked_socket = str(revoked_descriptor["socket_path"])
            before_revocation = len(extension.calls)
            state_file.write_text(revocation_mode + "\n", encoding="ascii")
            revoked_call = subprocess.run(
                ["sudo", "-n", "/usr/bin/python3",
                 str(Path(__file__).resolve()), "--peer-state-outcome",
                 revoked_socket, "15"], check=False, capture_output=True,
                text=True, timeout=3)
            if revoked_call.returncode != 0:
                raise RuntimeError(
                    f"runtime {revocation_mode} did not fail closed: "
                    f"{revoked_call.stdout!r} {revoked_call.stderr!r}")
            revoked_process.wait(timeout=3)
            if (revoked_process.returncode != 1 or
                    len(extension.calls) != before_revocation):
                raise RuntimeError(
                    f"runtime {revocation_mode} forwarded work or retained helper")

        state_file.write_text("user-manager\n", encoding="ascii")
        uncertain_process = subprocess.Popen(
            [str(helper)], env=state_environment, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True)
        uncertain_descriptor = wait_for_descriptor(
            descriptor_path, uncertain_process)
        extension.switch_state_file = state_file
        before_uncertain = len(extension.calls)
        uncertain_call = subprocess.run(
            ["sudo", "-n", "/usr/bin/python3", str(Path(__file__).resolve()),
             "--peer-switch-revocation",
             str(uncertain_descriptor["socket_path"]),
             str(uncertain_descriptor["helper_instance"])],
            check=False, capture_output=True, text=True, timeout=3)
        extension.switch_state_file = None
        if uncertain_call.returncode != 0:
            raise RuntimeError(
                f"post-dispatch revocation lost uncertainty semantics: "
                f"{uncertain_call.stdout!r} {uncertain_call.stderr!r}")
        uncertain_process.wait(timeout=3)
        uncertain_methods = [method for method, _ in
                             extension.calls[before_uncertain:]]
        if uncertain_process.returncode != 1 or uncertain_methods != [
                "GetState", "Switch"]:
            raise RuntimeError(
                f"revoked attempted Switch was replayed: {uncertain_methods!r}")

        state_file.write_text("user-manager\n", encoding="ascii")
        recovered_process = subprocess.Popen(
            [str(helper)], env=state_environment, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True)
        recovered_descriptor = wait_for_descriptor(
            descriptor_path, recovered_process)
        if (revoked_descriptor is None or
                recovered_descriptor["helper_instance"] ==
                revoked_descriptor["helper_instance"]):
            raise RuntimeError("fresh helper reused a revoked identity")
        recovered_process.send_signal(signal.SIGTERM)
        recovered_process.wait(timeout=3)
        if recovered_process.returncode != 0:
            raise RuntimeError("fresh helper did not recover after transient ambiguity")

        extension.role = 0
        extension.state_generation = 8
        extension.operation_key = None
        extension.operation_result = None
        environment["PUNTO_TEST_SESSION_MODE"] = "direct"
        functional_calls_start = len(extension.calls)
        process = subprocess.Popen(
            [str(helper)], env=environment, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True)
        root_helper = subprocess.run(
            ["sudo", "-n", str(helper)], env=environment, check=False,
            capture_output=True, text=True, timeout=3)
        if root_helper.returncode != 64:
            raise RuntimeError("helper did not reject UID 0 permanently")
        descriptor_path = runtime / "punto" / "session-bridge.json"
        descriptor = wait_for_descriptor(descriptor_path, process)
        expected_keys = {
            "protocol_version", "socket_path", "helper_instance", "pid",
            "start_time",
        }
        if set(descriptor) != expected_keys or descriptor["protocol_version"] != 1:
            raise RuntimeError(f"invalid descriptor: {descriptor!r}")
        if descriptor["pid"] != process.pid:
            raise RuntimeError("descriptor PID does not identify helper")
        socket_path = Path(str(descriptor["socket_path"]))
        if socket_path.parent != descriptor_path.parent or not socket_path.exists():
            raise RuntimeError("descriptor socket escaped its runtime directory")
        if socket_path.stat().st_mode & 0o777 != 0o600:
            raise RuntimeError("helper socket mode is not 0600")

        descriptor_json = descriptor_path.read_text(encoding="ascii")
        for field, invalid in (
                ("pid", int(descriptor["pid"]) + 1),
                ("start_time", int(descriptor["start_time"]) + 1)):
            invalid_descriptor = dict(descriptor)
            invalid_descriptor[field] = invalid
            descriptor_path.write_text(
                json.dumps(invalid_descriptor, separators=(",", ":")) + "\n",
                encoding="ascii")
            before = len(extension.calls)
            rejected_identity = subprocess.run(
                ["sudo", "-n", str(client), str(runtime), str(os.getuid()),
                 "0", "0", "reject"], check=False, capture_output=True,
                text=True, timeout=2)
            if (rejected_identity.returncode != 0 or
                    len(extension.calls) != before):
                raise RuntimeError(
                    f"descriptor {field} mismatch crossed the client boundary")
        descriptor_path.write_text(descriptor_json, encoding="ascii")
        descriptor_path.write_text(
            "protocol_version: 1\n"
            f"socket_path: {descriptor['socket_path']}\n"
            f"helper_instance: {descriptor['helper_instance']}\n"
            f"pid: {descriptor['pid']}\n"
            f"start_time: {descriptor['start_time']}\n",
            encoding="ascii")
        before = len(extension.calls)
        yaml_descriptor = subprocess.run(
            ["sudo", "-n", str(client), str(runtime), str(os.getuid()),
             "0", "0", "reject"], check=False, capture_output=True,
            text=True, timeout=2)
        descriptor_path.write_text(descriptor_json, encoding="ascii")
        if yaml_descriptor.returncode != 0 or len(extension.calls) != before:
            raise RuntimeError("non-JSON descriptor crossed the client boundary")

        before = len(extension.calls)
        fifo_replacement = descriptor_path.with_suffix(".fifo")
        fifo_replacement.unlink(missing_ok=True)
        os.mkfifo(fifo_replacement, mode=0o600)
        os.replace(fifo_replacement, descriptor_path)
        fifo_client = subprocess.Popen(
            ["sudo", "-n", "timeout", "--signal=KILL", "3s",
             str(client), str(runtime), str(os.getuid()), "0", "0",
             "fifo-recovery"], stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True)
        try:
            ready, _unused, _errors = select.select(
                [fifo_client.stdout], [], [], 2)
            line = fifo_client.stdout.readline().strip() if ready else ""
            if line != "FIFO_REJECTED":
                status = fifo_client.poll()
                stderr = (fifo_client.stderr.read()
                          if status is not None else "still running")
                raise RuntimeError(
                    "FIFO descriptor read did not reject boundedly: "
                    f"status={status} line={line!r} stderr={stderr!r}")
            if len(extension.calls) != before:
                raise RuntimeError("FIFO descriptor reached the extension")
            restored = descriptor_path.with_suffix(".restored")
            restored.write_text(descriptor_json, encoding="ascii")
            restored.chmod(0o600)
            os.replace(restored, descriptor_path)
            stdout, stderr = fifo_client.communicate(timeout=3)
            if fifo_client.returncode != 0 or "layout_bridge_verified" not in stdout:
                raise RuntimeError(
                    f"same client did not recover after descriptor restore: "
                    f"{stdout!r} {stderr!r}")
        finally:
            if fifo_client.poll() is None:
                fifo_client.communicate(timeout=4)
            if not descriptor_path.is_file():
                restored = descriptor_path.with_suffix(".cleanup")
                restored.write_text(descriptor_json, encoding="ascii")
                restored.chmod(0o600)
                os.replace(restored, descriptor_path)

        before = len(extension.calls)
        stalled_connect = subprocess.run(
            ["sudo", "-n", "env", "PUNTO_TEST_STALLED_CONNECT=1",
             str(client), str(runtime), str(os.getuid()), "0", "0",
             "connect-cancel"], check=False, capture_output=True, text=True,
            timeout=2)
        if stalled_connect.returncode != 0 or len(extension.calls) != before:
            raise RuntimeError(
                f"stalled connect was not cooperatively cancelled: "
                f"{stalled_connect.stdout!r} {stalled_connect.stderr!r}")

        unauthorized = False
        try:
            connection = peer_connection(str(socket_path))
            connection.call_sync(
                None, PEER_PATH, PEER_IFACE, "GetState",
                GLib.Variant("(st)", (CLIENT_ID, monotonic_us() + 100_000)),
                GLib.VariantType("(usssttty)"), Gio.DBusCallFlags.NONE,
                250, None)
        except GLib.Error:
            unauthorized = True
        if not unauthorized:
            raise RuntimeError("same-UID peer bypassed root-only admission")

        root_result = subprocess.run(
            ["sudo", "-n", "/usr/bin/python3", str(Path(__file__).resolve()),
             ("--peer-observe-client" if os.environ.get("PUNTO_OBSERVE_EDITOR_E2E") == "1"
              else "--peer-client"), str(socket_path),
             str(descriptor["helper_instance"])],
            check=False, capture_output=True, text=True, timeout=5)
        if root_result.returncode != 0:
            raise RuntimeError(
                f"root peer failed: {root_result.stdout!r} {root_result.stderr!r}")
        time.sleep(0.2)

        extension.role = 0
        extension.state_generation = 8
        extension.malformed_switch = True
        layout_result = subprocess.run(
            ["sudo", "-n", str(client), str(runtime), str(os.getuid()),
             "0", "1"], check=False, capture_output=True, text=True,
            timeout=5)
        if layout_result.returncode != 0:
            raise RuntimeError(
                f"desktop layout client failed: {layout_result.stdout!r} "
                f"{layout_result.stderr!r}")
        extension.malformed_switch = False
        verify_result = subprocess.run(
            ["sudo", "-n", str(client), str(runtime), str(os.getuid()),
             "1", "1"], check=False, capture_output=True, text=True,
            timeout=5)
        if verify_result.returncode != 0:
            raise RuntimeError(
                f"desktop layout verification failed: "
                f"{verify_result.stdout!r} {verify_result.stderr!r}")
        time.sleep(0.1)

        before = len(extension.calls)
        extension.role = 0
        extension.state_generation = 8
        extension.delay_state_once_ms = 950
        extension.delay_state_attempts = 2
        recovered_state = subprocess.run(
            ["sudo", "-n", str(client), str(runtime), str(os.getuid()),
             "0", "1"], check=False, capture_output=True, text=True,
            timeout=5)
        if recovered_state.returncode != 0:
            raise RuntimeError(
                "desktop layout client did not recover a lost preflight "
                f"GetState: {recovered_state.stdout!r} "
                f"{recovered_state.stderr!r}")
        recovered_state_methods = [
            method for method, _values in extension.calls[before:]
        ]
        if recovered_state_methods != [
                "GetState", "GetState", "GetState", "Switch", "GetState"]:
            raise RuntimeError(
                "preflight recovery duplicated or skipped layout work: "
                f"{recovered_state_methods!r}")
        time.sleep(0.1)

        before = len(extension.calls)
        extension.role = 0
        extension.switch_outcome = 2
        rejected = subprocess.run(
            ["sudo", "-n", str(client), str(runtime), str(os.getuid()),
             "0", "1", "reject"], check=False, capture_output=True,
            text=True, timeout=5)
        if rejected.returncode != 0:
            raise RuntimeError(
                f"AlreadyCurrent was accepted: {rejected.stdout!r} "
                f"{rejected.stderr!r}")
        rejected_methods = [method for method, _values in extension.calls[before:]]
        if rejected_methods != ["GetState", "Switch"]:
            raise RuntimeError(
                f"terminal rejection triggered more work: {rejected_methods!r}")
        extension.switch_outcome = None

        before = len(extension.calls)
        extension.role = 0
        extension.switch_outcome = 18
        recovered_not_found = subprocess.run(
            ["sudo", "-n", str(client), str(runtime), str(os.getuid()),
             "0", "1"], check=False, capture_output=True, text=True,
            timeout=5)
        if recovered_not_found.returncode != 0:
            raise RuntimeError(
                f"Switch NotFound was not recovered through the journal: "
                f"{recovered_not_found.stdout!r} "
                f"{recovered_not_found.stderr!r}")
        not_found_methods = [method for method, _values
                             in extension.calls[before:]]
        if not_found_methods != ["GetState", "Switch", "GetOperation",
                                 "GetState"]:
            raise RuntimeError(
                f"Switch NotFound recovery was not exact: "
                f"{not_found_methods!r}")
        extension.switch_outcome = None

        cancel_marker = runtime / "cancel-after-switch"
        extension.role = 0
        extension.cancel_marker = cancel_marker
        cancel_environment = environment.copy()
        cancel_environment["PUNTO_TEST_CANCEL_MARKER"] = str(cancel_marker)
        before = len(extension.calls)
        cancelled = subprocess.run(
            ["sudo", "-n", "env",
             f"PUNTO_TEST_CANCEL_MARKER={cancel_marker}",
             str(client), str(runtime), str(os.getuid()),
             "0", "1", "cancel"], env=cancel_environment, check=False,
            capture_output=True, text=True, timeout=5)
        if cancelled.returncode != 0:
            raise RuntimeError(
                f"post-dispatch cancellation failed: {cancelled.stdout!r} "
                f"{cancelled.stderr!r}")
        cancelled_methods = [method for method, _values in extension.calls[before:]]
        if cancelled_methods != ["GetState", "Switch"]:
            raise RuntimeError(
                f"cancelled call queried operation: {cancelled_methods!r}")
        switch_deadline = extension.calls[before + 1][1][6]
        held_receivers = len(extension.pending_invocations)
        if not cancel_marker.exists() or held_receivers == 0:
            raise RuntimeError("cancelled Switch was not held at the extension")
        concurrent_state = subprocess.run(
            ["sudo", "-n", "/usr/bin/python3", str(Path(__file__).resolve()),
             "--probe-peer", str(socket_path),
             str(descriptor["helper_instance"])], check=False,
            capture_output=True, text=True, timeout=1)
        if (concurrent_state.returncode != 0 or
                monotonic_us() >= switch_deadline or
                len(extension.pending_invocations) != held_receivers or
                [method for method, _ in extension.calls[before:]] !=
                ["GetState", "Switch", "GetState"]):
            raise RuntimeError(
                "held Switch blocked another peer's fresh State: "
                f"status={concurrent_state.returncode} "
                f"stdout={concurrent_state.stdout!r} "
                f"stderr={concurrent_state.stderr!r}")
        extension.cancel_marker = None
        time.sleep(1.0)

        poison = runtime / "pump-after-completion"
        callback = runtime / "switch-callback-completed"
        flood = runtime / "flood-switch-pump"
        r3_failures: list[str] = []
        for marker in (poison, callback, flood):
            marker.unlink(missing_ok=True)
        flood_environment = environment.copy()
        flood_environment["PUNTO_TEST_FLOOD_MARKER"] = str(flood)
        flood_environment["PUNTO_TEST_POISON_MARKER"] = str(poison)
        flood_environment["PUNTO_TEST_CALLBACK_MARKER"] = str(callback)
        before = len(extension.calls)
        extension.role = 0
        extension.flood_marker = flood
        flooded_reply = subprocess.run(
            ["sudo", "-n", "timeout", "--signal=KILL", "3s", "env",
             f"PUNTO_TEST_FLOOD_MARKER={flood}",
             f"PUNTO_TEST_POISON_MARKER={poison}",
             f"PUNTO_TEST_CALLBACK_MARKER={callback}",
             str(client), str(runtime), str(os.getuid()), "0", "1"],
            env=flood_environment, check=False, capture_output=True,
            text=True, timeout=4)
        flooded_methods = [method for method, _values in extension.calls[before:]]
        if (flooded_reply.returncode != 0 or poison.exists() or
                not callback.exists() or flooded_methods != [
                    "GetState", "Switch", "GetState"]):
            r3_failures.append(
                "terminal Switch callback did not stop its flooded pump: "
                f"returncode={flooded_reply.returncode} methods={flooded_methods!r} "
                f"callback={callback.exists()} poison={poison.exists()}")

        for marker in (poison, callback, flood):
            marker.unlink(missing_ok=True)
        before = len(extension.calls)
        extension.role = 0
        extension.malformed_switch = True
        malformed_flood = subprocess.run(
            ["sudo", "-n", "timeout", "--signal=KILL", "3s", "env",
             f"PUNTO_TEST_FLOOD_MARKER={flood}",
             f"PUNTO_TEST_POISON_MARKER={poison}",
             f"PUNTO_TEST_CALLBACK_MARKER={callback}",
             str(client), str(runtime), str(os.getuid()), "0", "1"],
            env=flood_environment, check=False, capture_output=True,
            text=True, timeout=4)
        extension.malformed_switch = False
        malformed_methods = [
            method for method, _values in extension.calls[before:]]
        if (malformed_flood.returncode != 0 or poison.exists() or
                not callback.exists() or malformed_methods != [
                    "GetState", "Switch", "GetOperation", "GetState"] or
                malformed_methods.count("Switch") != 1):
            r3_failures.append(
                "foreign terminal reply was accepted or replayed under flood: "
                f"returncode={malformed_flood.returncode} "
                f"methods={malformed_methods!r} callback={callback.exists()} "
                f"poison={poison.exists()}")

        cancel_marker = runtime / "cancel-flood-after-switch"
        for marker in (poison, callback, flood, cancel_marker):
            marker.unlink(missing_ok=True)
        extension.role = 0
        extension.cancel_marker = cancel_marker
        before = len(extension.calls)
        cancelled_flood = subprocess.run(
            ["sudo", "-n", "timeout", "--signal=KILL", "3s", "env",
             f"PUNTO_TEST_FLOOD_MARKER={flood}",
             f"PUNTO_TEST_POISON_MARKER={poison}",
             f"PUNTO_TEST_CANCEL_MARKER={cancel_marker}",
             str(client), str(runtime), str(os.getuid()),
             "0", "1", "cancel"], env=flood_environment, check=False,
            capture_output=True, text=True, timeout=4)
        cancelled_methods = [
            method for method, _values in extension.calls[before:]]
        if (cancelled_flood.returncode != 0 or not flood.exists() or
                cancelled_methods != ["GetState", "Switch"] or
                cancelled_methods.count("Switch") != 1):
            r3_failures.append(
                "flooded sent Switch ignored cooperative cancellation: "
                f"returncode={cancelled_flood.returncode} "
                f"methods={cancelled_methods!r} flood={flood.exists()}")
        extension.cancel_marker = None
        time.sleep(1.0)

        for marker in (poison, callback, flood):
            marker.unlink(missing_ok=True)
        extension.role = 0
        extension.drop_switch_reply = True
        before = len(extension.calls)
        deadline_recovery = subprocess.run(
            ["sudo", "-n", "timeout", "--signal=KILL", "3s", "env",
             f"PUNTO_TEST_FLOOD_MARKER={flood}",
             f"PUNTO_TEST_POISON_MARKER={poison}",
             str(client), str(runtime), str(os.getuid()), "0", "1"],
            env=flood_environment, check=False, capture_output=True,
            text=True, timeout=4)
        extension.drop_switch_reply = False
        recovered_calls = extension.calls[before:]
        recovered_methods = [method for method, _values in recovered_calls]
        if (deadline_recovery.returncode != 0 or not flood.exists() or
                recovered_methods != [
                    "GetState", "Switch", "GetOperation", "GetState"] or
                recovered_methods.count("Switch") != 1):
            r3_failures.append(
                "flooded sent Switch did not enter identity recovery: "
                f"returncode={deadline_recovery.returncode} "
                f"methods={recovered_methods!r} flood={flood.exists()}")
        elif recovered_calls[2][1][1:6] != recovered_calls[1][1][1:6]:
            r3_failures.append("GetOperation changed original operation identity")
        extension.flood_marker = None
        if r3_failures:
            raise RuntimeError("; ".join(r3_failures))

        replacement_started = threading.Event()
        replacement_data: dict[str, object] = {}
        replacement_errors: list[BaseException] = []
        extension.replace_event = threading.Event()

        def replace_helper_after_journal() -> None:
            try:
                if not extension.replace_event.wait(timeout=2):
                    raise RuntimeError("replacement Switch was not journaled")
                process.kill()
                process.wait(timeout=2)
                # Leave the now-stale descriptor visible briefly. The root
                # client must keep pumping and bind only to the new helper.
                time.sleep(0.15)
                new_process = subprocess.Popen(
                    [str(helper)], env=environment, stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE, text=True)
                new_descriptor = wait_for_descriptor(
                    descriptor_path, new_process)
                replacement_data["process"] = new_process
                replacement_data["descriptor"] = new_descriptor
                replacement_started.set()
            except BaseException as error:
                replacement_errors.append(error)
                replacement_started.set()

        replacement_thread = threading.Thread(
            target=replace_helper_after_journal, daemon=True)
        replacement_thread.start()
        before = len(extension.calls)
        extension.role = 0
        recovered = subprocess.run(
            ["sudo", "-n", str(client), str(runtime), str(os.getuid()),
             "0", "1"], check=False, capture_output=True, text=True,
            timeout=5)
        replacement_thread.join(timeout=3)
        if replacement_thread.is_alive() or replacement_errors:
            raise RuntimeError(
                f"helper replacement failed: {replacement_errors!r}")
        if recovered.returncode != 0:
            raise RuntimeError(
                f"client did not recover through replacement helper: "
                f"{recovered.stdout!r} {recovered.stderr!r}")
        recovered_methods = [method for method, _values in extension.calls[before:]]
        if recovered_methods != ["GetState", "Switch", "BindHelper",
                                 "GetOperation", "GetState"]:
            raise RuntimeError(
                f"replacement recovery replayed or reordered calls: "
                f"{recovered_methods!r}")
        switch_values = extension.calls[before + 1][1]
        operation_values = extension.calls[before + 3][1]
        new_descriptor = replacement_data["descriptor"]
        if operation_values[:6] != (
                new_descriptor["helper_instance"], *switch_values[1:6]):
            raise RuntimeError(
                "replacement recovery changed operation identity or used "
                "the stale helper identity")
        process = replacement_data["process"]
        descriptor = replacement_data["descriptor"]
        socket_path = Path(str(descriptor["socket_path"]))
        extension.replace_event = None
        extension.operation_result = None

        held_peers: list[subprocess.Popen] = []
        try:
            for _index in range(4):
                held = subprocess.Popen(
                    ["sudo", "-n", "/usr/bin/python3",
                     str(Path(__file__).resolve()), "--hold-peer",
                     str(socket_path)], stdin=subprocess.PIPE,
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                ready, _unused, _errors = select.select(
                    [held.stdout], [], [], 2)
                if not ready or held.stdout.readline().strip() != "READY":
                    raise RuntimeError("root peer did not reach held state")
                held_peers.append(held)
            fifth = subprocess.run(
                ["sudo", "-n", "/usr/bin/python3",
                 str(Path(__file__).resolve()), "--probe-peer",
                 str(socket_path), str(descriptor["helper_instance"])],
                check=False, capture_output=True,
                text=True, timeout=3)
            if fifth.returncode == 0:
                raise RuntimeError("fifth peer bypassed connection limit")
        finally:
            for held in held_peers:
                if held.stdin is not None:
                    held.stdin.close()
                held.wait(timeout=2)
        time.sleep(0.1)

        duplicate = subprocess.run(
            [str(helper)], env=environment, check=False,
            capture_output=True, text=True, timeout=3)
        if duplicate.returncode == 0:
            raise RuntimeError("duplicate helper acquired the active name")
        if json.loads(descriptor_path.read_text(encoding="ascii")) != descriptor:
            raise RuntimeError("duplicate helper replaced the active descriptor")

        replacement = descriptor_path.with_suffix(".replacement")
        replacement.write_text('{"newer":true}\n', encoding="ascii")
        replacement.chmod(0o600)
        os.replace(replacement, descriptor_path)
        extension.release_name()
        process.wait(timeout=3)
        if process.returncode != 1:
            stdout, stderr = process.communicate()
            raise RuntimeError(
                f"helper owner-loss exit was {process.returncode}: "
                f"{stdout!r} {stderr!r}")
        if not descriptor_path.exists():
            raise RuntimeError("helper teardown removed a rebound descriptor")
        if socket_path.exists():
            raise RuntimeError("helper teardown retained its owned socket")

        extension.acquire_name()
        replacement_process = subprocess.Popen(
            [str(helper)], env=environment, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True)
        replacement_descriptor = wait_for_descriptor(
            descriptor_path, replacement_process)
        replacement_socket = Path(str(replacement_descriptor["socket_path"]))
        replacement_process.send_signal(signal.SIGTERM)
        replacement_process.wait(timeout=3)
        if replacement_process.returncode != 0:
            stdout, stderr = replacement_process.communicate()
            raise RuntimeError(
                f"replacement helper SIGTERM exit was "
                f"{replacement_process.returncode}: {stdout!r} {stderr!r}")
        if descriptor_path.exists() or replacement_socket.exists():
            raise RuntimeError("replacement helper retained its owned endpoint")
        if not secure_enrollment_marker(status.marker) or status.errors or any(
                call[0] == "Set" for call in status.calls):
            raise RuntimeError("session contract failed already-true enrollment or touched accessibility settings")
    status.close()
    loop.quit()
    thread.join(timeout=2)
    methods = [method for method, _values
               in extension.calls[functional_calls_start:]]
    if methods.count("BindHelper") != 3 or methods[2:7] != [
            "GetState", "Switch", "GetOperation", "Switch",
            "GetOperation"] or methods[7:12] != [
                "GetState", "Switch", "GetOperation", "GetState",
                "GetState"] or any(method not in {
                    "BindHelper", "GetState", "Switch", "GetOperation"
                } for method in methods):
        raise RuntimeError(f"unexpected extension call sequence: {methods!r}")
    print("test_session_bridge_e2e: OK")
    return 0


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--desktop-observer-contract":
        raise SystemExit(run_desktop_observer_contract(Path(sys.argv[2]).resolve()))
    if len(sys.argv) == 3 and sys.argv[1] == "--observer-private-four":
        raise SystemExit(observer_private_four(sys.argv[2]))
    if len(sys.argv) == 2 and sys.argv[1] == "--observer-foreign":
        try:
            observer_read(new_message_bus_connection())
        except GLib.Error:
            raise SystemExit(0)
        raise SystemExit("foreign observer was admitted")
    if len(sys.argv) == 3 and sys.argv[1] == "--accessibility-contract":
        raise SystemExit(run_accessibility_contract(Path(sys.argv[2]).resolve()))
    if len(sys.argv) == 4 and sys.argv[1] == "--peer-observe-client":
        raise SystemExit(peer_client(sys.argv[2], sys.argv[3], observe=True))
    if len(sys.argv) == 4 and sys.argv[1] == "--peer-client":
        raise SystemExit(peer_client(sys.argv[2], sys.argv[3]))
    if len(sys.argv) == 3 and sys.argv[1] == "--hold-peer":
        raise SystemExit(hold_peer(sys.argv[2]))
    if len(sys.argv) == 4 and sys.argv[1] == "--probe-peer":
        raise SystemExit(probe_peer(sys.argv[2], sys.argv[3]))
    if len(sys.argv) == 4 and sys.argv[1] == "--peer-state-outcome":
        raise SystemExit(peer_state_outcome(sys.argv[2], int(sys.argv[3])))
    if len(sys.argv) == 4 and sys.argv[1] == "--peer-switch-revocation":
        raise SystemExit(peer_switch_then_revocation(sys.argv[2], sys.argv[3]))
    if len(sys.argv) != 3:
        raise SystemExit("usage: test_session_bridge_e2e.py HELPER CLIENT")
    raise SystemExit(run_contract(Path(sys.argv[1]).resolve(),
                                  Path(sys.argv[2]).resolve()))
