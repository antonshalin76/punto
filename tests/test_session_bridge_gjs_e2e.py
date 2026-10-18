#!/usr/bin/python3

import hashlib
import json
import os
from pathlib import Path
import select
import signal
import stat
import subprocess
import sys
import tempfile
import time

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib


ROOT = Path(__file__).resolve().parents[1]
PROTOCOL = ROOT / "protocol" / "punto-dbus-v1.xml"
FIXTURE = ROOT / "tests" / "session_bridge_gjs_fixture.mjs"
PEER_PATH = "/org/punto/Switcher/SessionBridge1"
PEER_IFACE = "org.punto.Switcher.SessionBridge1"
CLIENT = "33" * 16
OBSERVER_PATH = "/org/punto/Switcher/DesktopObserver1"
OBSERVER_IFACE = "org.punto.Switcher.DesktopObserver1"


def wait_line(process: subprocess.Popen, expected: str) -> None:
    ready, _, _ = select.select([process.stdout], [], [], 3)
    line = process.stdout.readline().strip() if ready else ""
    if line != expected:
        raise RuntimeError(f"fixture readiness failed: {line!r}")


def wait_descriptor(path: Path, process: subprocess.Popen) -> dict:
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        if path.is_file():
            value = json.loads(path.read_text(encoding="ascii"))
            if value.get("pid") == process.pid:
                return value
        if process.poll() is not None:
            stdout, stderr = process.communicate()
            raise RuntimeError(
                f"helper exited {process.returncode}: {stdout!r} {stderr!r}")
        time.sleep(0.01)
    raise RuntimeError("helper descriptor missing")


def request_once(socket_path: str, helper: str) -> None:
    connection = Gio.DBusConnection.new_for_address_sync(
        f"unix:path={socket_path}",
        Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT, None, None)
    state = connection.call_sync(
        None, PEER_PATH, PEER_IFACE, "GetState",
        GLib.Variant("(st)", (CLIENT, GLib.get_monotonic_time() + 500_000)),
        GLib.VariantType("(usssttty)"), Gio.DBusCallFlags.NONE, 1000, None
    ).unpack()
    if state[0] != 0 or state[1] != helper or state[2] != "11" * 16:
        raise RuntimeError(f"invalid state: {state!r}")
    canonical = (
        "version=1\n"
        f"extension_instance={state[2]}\n"
        f"expected_source_generation={state[5]}\n"
        f"expected_state_generation={state[6]}\n"
        f"expected_role={state[7]}\n"
        "target_role=1\n"
    )
    digest = hashlib.sha256(canonical.encode("ascii")).hexdigest()
    values = (helper, state[2], CLIENT, state[3], 1, digest,
              GLib.get_monotonic_time() + 500_000, state[5], state[6],
              state[7], 1)
    switched = connection.call_sync(
        None, PEER_PATH, PEER_IFACE, "Switch",
        GLib.Variant("(sssststttyy)", values),
        GLib.VariantType("(usstty)"), Gio.DBusCallFlags.NONE, 1000, None
    ).unpack()
    replay = connection.call_sync(
        None, PEER_PATH, PEER_IFACE, "Switch",
        GLib.Variant("(sssststttyy)", values),
        GLib.VariantType("(usstty)"), Gio.DBusCallFlags.NONE, 1000, None
    ).unpack()
    if switched[0] != 1 or replay != switched:
        raise RuntimeError(f"switch/replay failed: {switched!r} {replay!r}")


def fill_private_capacity(socket_path: str, helper: str) -> None:
    connection = Gio.DBusConnection.new_for_address_sync(
        f"unix:path={socket_path}",
        Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT, None, None)
    leases = []
    for index in range(32):
        state = connection.call_sync(
            None, PEER_PATH, PEER_IFACE, "GetState",
            GLib.Variant("(st)", (f"{index + 100:032x}",
                                   GLib.get_monotonic_time() + 500_000)),
            GLib.VariantType("(usssttty)"), Gio.DBusCallFlags.NONE, 1000, None
        ).unpack()
        if state[0] != 0 or state[1] != helper:
            raise RuntimeError(f"private capacity setup failed at {index}: {state!r}")
        leases.append(state[3:5])
    connection.close_sync(None)
    print(json.dumps(leases))


def observe_public(helper: str, expected: int = 0, role: int = 0):
    connection = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    state = connection.call_sync(
        "org.punto.Switcher.SessionBridge1", OBSERVER_PATH, OBSERVER_IFACE,
        "ReadState", GLib.Variant("(t)", (GLib.get_monotonic_time() + 100_000,)),
        GLib.VariantType("(usstty)"), Gio.DBusCallFlags.NO_AUTO_START, 300, None
    ).unpack()
    if (len(state) != 6 or state[0] != expected or state[1] != helper or
            state[5] != (role if expected == 0 else 255)):
        raise RuntimeError(f"production GJS public projection failed: {state!r}")
    if expected == 0 and (state[2] != "11" * 16 or state[3] <= 0 or state[4] <= 0):
        raise RuntimeError(f"production GJS observation lacked generations: {state!r}")
    return state


def main(helper: Path, gjs: Path, old_helper: Path | None, capacity: bool = False) -> int:
    with tempfile.TemporaryDirectory(prefix="punto-gjs-bridge-") as root_value:
        root = Path(root_value)
        root.chmod(0o700)
        marker = root / "gjs-counts.json"
        status_marker = root / "gjs-status-counts.json"
        state_directory = root / "accessibility-state"
        state_directory.mkdir(mode=0o700)
        enrollment_marker = state_directory / "accessibility-enrolled"
        process = None
        fixture = subprocess.Popen(
            [str(gjs), "-m", str(FIXTURE), str(PROTOCOL), str(marker),
             str(status_marker), str(state_directory)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            wait_line(fixture, "READY")
            environment = os.environ.copy()
            environment["XDG_RUNTIME_DIR"] = str(root)
            environment["STATE_DIRECTORY"] = str(state_directory)
            environment["PUNTO_TEST_SESSION_MODE"] = "user-manager"
            descriptor_path = root / "punto" / "session-bridge.json"

            wrong_uid_environment = environment.copy()
            wrong_uid_environment["PUNTO_TEST_HELPER_UID_OFFSET"] = "1"
            wrong_uid = subprocess.run(
                [str(helper)], env=wrong_uid_environment,
                check=False, capture_output=True, text=True, timeout=3)
            if wrong_uid.returncode != 64:
                raise RuntimeError(
                    f"wrong Shell owner UID returned {wrong_uid.returncode}, "
                    f"expected permanent rejection: {wrong_uid.stderr!r}")
            if json.loads(marker.read_text(encoding="ascii"))["BindHelper"] != 0:
                raise RuntimeError("wrong Shell owner UID reached BindHelper")
            if enrollment_marker.exists() or json.loads(status_marker.read_text())["Set"] != 0:
                raise RuntimeError("wrong Shell owner UID changed accessibility enrollment")

            if old_helper is not None:
                old_environment = environment.copy()
                old_environment["PUNTO_TEST_SESSION_MODE"] = "lookup-failure"
                old = subprocess.run(
                    [str(old_helper)], env=old_environment, check=False,
                    capture_output=True, text=True, timeout=3)
                counts = json.loads(marker.read_text(encoding="ascii"))
                if old.returncode != 75 or descriptor_path.exists() or counts["BindHelper"] != 0:
                    raise RuntimeError("old helper did not fail before BindHelper at PID ENODATA")
                print("OLD_RED stage=pid-session-enodata-before-bind exit=75")

            process = subprocess.Popen(
                [str(helper)], env=environment, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, text=True)
            descriptor = wait_descriptor(descriptor_path, process)
            if capacity:
                private_args = ["--fill-private-capacity"]
            else:
                first = observe_public(descriptor["helper_instance"])
                for _ in range(2):
                    if observe_public(descriptor["helper_instance"]) != first:
                        raise RuntimeError("public reads changed GNOME state")
                read_counts = json.loads(marker.read_text(encoding="ascii"))
                if read_counts != {"BindHelper": 1, "GetState": 3, "Switch": 0,
                                   "GetOperation": 0, "activations": 0}:
                    raise RuntimeError(f"observation mutated layout: {read_counts!r}")
                private_args = ["--peer"]
            peer = subprocess.run(
                ["sudo", "-n", "/usr/bin/python3", str(Path(__file__).resolve()),
                 *private_args, str(descriptor["socket_path"]),
                 str(descriptor["helper_instance"])],
                check=False, capture_output=True, text=True, timeout=4)
            if peer.returncode != 0:
                raise RuntimeError(f"production GJS request failed: {peer.stderr!r}")
            observe_public(descriptor["helper_instance"], 14 if capacity else 0,
                           0 if capacity else 1)
            if capacity:
                preserved = subprocess.run(
                    ["sudo", "-n", "/usr/bin/python3", str(Path(__file__).resolve()),
                     "--fill-private-capacity", str(descriptor["socket_path"]),
                     str(descriptor["helper_instance"])],
                    check=False, capture_output=True, text=True, timeout=4)
                if preserved.returncode != 0 or preserved.stdout != peer.stdout:
                    raise RuntimeError(f"public Capacity evicted private leases: {preserved.stderr!r}")
            deadline = time.monotonic() + 1
            counts = {}
            while time.monotonic() < deadline:
                counts = json.loads(marker.read_text(encoding="ascii"))
                if counts.get("GetState") == (65 if capacity else 5):
                    break
                time.sleep(0.01)
            expected_counts = {"BindHelper": 1, "GetState": 65 if capacity else 5,
                               "Switch": 0 if capacity else 2,
                               "GetOperation": 0, "activations": 0 if capacity else 1}
            if counts != expected_counts:
                raise RuntimeError(f"unexpected production GJS counts: {counts!r}")
            process.send_signal(signal.SIGTERM)
            process.wait(timeout=3)
            if process.returncode != 0:
                raise RuntimeError("new helper teardown failed")
            status_counts = json.loads(status_marker.read_text(encoding="ascii"))
            enrollment = enrollment_marker.lstat()
            if (status_counts != {"Get": 2, "Set": 1, "forbidden": 0, "markerBeforeSet": True}
                    or not stat.S_ISREG(enrollment.st_mode)
                    or stat.S_IMODE(enrollment.st_mode) != 0o600
                    or enrollment.st_size != 0 or enrollment.st_uid != os.getuid()):
                raise RuntimeError(f"GJS accessibility enrollment violated contract: {status_counts!r}")
        finally:
            if process is not None and process.poll() is None:
                process.terminate()
                process.communicate(timeout=3)
            fixture.terminate()
            fixture.wait(timeout=3)
    print("test_session_bridge_gjs_e2e: OK")
    return 0


if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "--fill-private-capacity":
        fill_private_capacity(sys.argv[2], sys.argv[3])
        raise SystemExit(0)
    if len(sys.argv) == 4 and sys.argv[1] == "--observer-capacity":
        raise SystemExit(main(Path(sys.argv[2]), Path(sys.argv[3]), None, capacity=True))
    if len(sys.argv) == 4 and sys.argv[1] == "--peer":
        request_once(sys.argv[2], sys.argv[3])
        raise SystemExit(0)
    if len(sys.argv) not in (3, 4):
        raise SystemExit(
            "usage: test_session_bridge_gjs_e2e.py HELPER GJS [OLD_HELPER]")
    raise SystemExit(main(Path(sys.argv[1]), Path(sys.argv[2]),
                          Path(sys.argv[3]) if len(sys.argv) == 4 else None))
