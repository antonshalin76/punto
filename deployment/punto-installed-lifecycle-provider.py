#!/usr/bin/python3
"""Exact disposable-session writer retirement and runtime postflight callback.

The package orchestrator alone owns restoration, masks and UNKNOWN receipts.
This callback never installs packages, restores files or verifies typed input.
"""

import hashlib
import json
import os
from pathlib import Path
import pwd
import re
import select
import signal
import socket
import stat
import struct
import subprocess
import sys
import time
import types


class Refusal(RuntimeError):
    def __init__(self, message, status=66):
        super().__init__(message)
        self.status = (75 if status == "INVALID_OBSERVATION" else 66) if isinstance(status, str) else status


class NotPublished(Refusal):
    """Positive startup absence, usable only while exact successors stay live."""


UNIT_OBSERVER_SOURCE = '''
def observe_unit(unit, user, deadline, *, clock=None):
    import os, time
    from gi.repository import Gio, GLib
    clock = clock or time.monotonic
    expected_uid = 1000 if user else 0
    if type(user) is not bool or os.getuid() != expected_uid:
        raise RuntimeError("unit observer Unix context differs")
    def remaining():
        left = deadline - clock()
        if left <= 0:
            raise TimeoutError("unit observation original deadline expired")
        return left
    remaining()
    address = Gio.dbus_address_get_for_bus_sync(Gio.BusType.SESSION if user else Gio.BusType.SYSTEM, None)
    connection = Gio.DBusConnection.new_for_address_sync(address,
        Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, None, None)
    try:
        def call(destination, path, interface, method, parameters, signature):
            result = connection.call_sync(destination, path, interface, method, parameters,
                GLib.VariantType.new(signature), Gio.DBusCallFlags.NO_AUTO_START,
                max(1, int(remaining() * 1000)), None)
            remaining()
            return result.unpack()
        bus, bus_path = "org.freedesktop.DBus", "/org/freedesktop/DBus"
        service, manager = "org.freedesktop.systemd1", "/org/freedesktop/systemd1"
        owner, = call(bus, bus_path, bus, "GetNameOwner", GLib.Variant("(s)", (service,)), "(s)")
        if not Gio.dbus_is_unique_name(owner):
            raise RuntimeError("unit manager owner is not unique")
        uid, = call(bus, bus_path, bus, "GetConnectionUnixUser", GLib.Variant("(s)", (owner,)), "(u)")
        if uid != expected_uid:
            raise RuntimeError("unit manager UnixUID differs")
        path, = call(owner, manager, service + ".Manager", "LoadUnit", GLib.Variant("(s)", (unit,)), "(o)")
        if not path.startswith(manager + "/unit/"):
            raise RuntimeError("unit manager object path differs")
        call(owner, path, service + ".Unit", "Ref", None, "()")
        def properties(interface):
            value, = call(owner, path, "org.freedesktop.DBus.Properties", "GetAll",
                          GLib.Variant("(s)", (service + interface,)), "(a{sv})")
            return value
        first = properties(".Unit")
        process = properties(".Service")
        last = properties(".Unit")
        names = ("Id", "LoadState", "ActiveState", "FragmentPath", "DropInPaths", "Job")
        if any(first[name] != last[name] for name in names) or last["Id"] != unit:
            raise RuntimeError("unit state changed during pinned observation")
        if (any(type(last[name]) is not str for name in names[:4]) or
                type(last["DropInPaths"]) is not list or any(type(p) is not str for p in last["DropInPaths"]) or
                type(process["MainPID"]) is not int or not 0 <= process["MainPID"] < 2**32 or
                type(last["Job"]) is not tuple or len(last["Job"]) != 2 or
                type(last["Job"][0]) is not int or not 0 <= last["Job"][0] < 2**32 or
                type(last["Job"][1]) is not str or not GLib.Variant.is_object_path(last["Job"][1])):
            raise RuntimeError("unit property types differ")
        after, = call(bus, bus_path, bus, "GetNameOwner", GLib.Variant("(s)", (service,)), "(s)")
        if after != owner:
            raise RuntimeError("unit manager handed off during observation")
        return {name: last[name] for name in names[1:]} | {"MainPID": process["MainPID"]}
    finally:
        connection.close_sync(None)
'''
exec(UNIT_OBSERVER_SOURCE)


def trusted_file(path, digest=None):
    path = Path(path)
    if not path.is_absolute():
        raise Refusal("admitted file is not absolute")
    for parent in (*reversed(path.parents), path):
        meta = parent.lstat()
        if meta.st_uid != 0 or meta.st_gid != 0 or meta.st_mode & 0o022 or stat.S_ISLNK(meta.st_mode):
            raise Refusal("admitted file parent/owner changed")
    meta = path.lstat()
    if not stat.S_ISREG(meta.st_mode) or meta.st_nlink != 1:
        raise Refusal("admitted file is not a single regular inode")
    payload = path.read_bytes()
    after = path.lstat()
    fields = ("st_dev", "st_ino", "st_mode", "st_uid", "st_gid", "st_nlink", "st_size", "st_mtime_ns", "st_ctime_ns")
    if any(getattr(after, field) != getattr(meta, field) for field in fields) or \
            (digest is not None and hashlib.sha256(payload).hexdigest() != digest):
        raise Refusal("admitted file metadata/hash changed")
    return payload


class Provider:
    def __init__(self, spec, facts, deadline, *, clock=time.monotonic):
        self.s, self.f, self.deadline, self.clock = spec, facts, deadline, clock
        self.pending_effect = False

    def remaining(self):
        left = self.deadline - self.clock()
        if left <= 0:
            raise Refusal("callback deadline expired; completion unknown", 75)
        return left

    def user(self, argv):
        return ["/usr/bin/setpriv", "--reuid=" + str(self.s["uid"]), "--regid=" + str(self.s["gid"]),
                "--init-groups", "/usr/bin/env", "-i", "PATH=/usr/bin:/bin",
                "HOME=" + self.s["home"], "USER=" + self.s["username"], "LOGNAME=" + self.s["username"],
                "XDG_RUNTIME_DIR=/run/user/1000", "DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus",
                "XDG_SESSION_TYPE=x11", "XDG_SESSION_ID=" + self.s["session"],
                "DISPLAY=" + self.s["display"], "XAUTHORITY=" + self.s["xauthority"], *argv]

    def run(self, argv):
        self.remaining()
        child = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE, env={"PATH": "/usr/bin:/bin", "LC_ALL": "C"})
        try:
            out, err = child.communicate(timeout=self.remaining())
        except BaseException as error:
            child.kill()
            try:
                child.communicate(timeout=.1)
            except subprocess.TimeoutExpired:
                raise Refusal("control child termination is unknown", 75) from error
            if isinstance(error, (subprocess.TimeoutExpired, Refusal)):
                raise Refusal("control command completion is unknown", 75) from error
            raise
        self.remaining()
        if child.returncode:
            raise Refusal("control command refused: " + err.decode(errors="replace")[:512],
                          75 if self.pending_effect else 66)
        return out.decode()

    def bus(self, destination, object_path, method, arguments=(), *, user=True):
        from gi.repository import GLib
        argv = ["/usr/bin/gdbus", "call", "--session" if user else "--system", "--dest", destination,
                "--object-path", object_path, "--method", method, *arguments]
        text = self.run(self.user(argv) if user else argv).strip()
        return GLib.Variant.parse(None, text, None, None).unpack()

    def bus_peer(self, name):
        destination, path = "org.freedesktop.DBus", "/org/freedesktop/DBus"
        owner, = self.bus(destination, path, destination + ".GetNameOwner", (name,))
        if not re.fullmatch(r":[0-9]+\.[0-9]+", owner):
            raise Refusal("session owner is not unique")
        pid, = self.bus(destination, path, destination + ".GetConnectionUnixProcessID", (owner,))
        uid, = self.bus(destination, path, destination + ".GetConnectionUnixUser", (owner,))
        after, = self.bus(destination, path, destination + ".GetNameOwner", (name,))
        if after != owner:
            raise Refusal("well-known peer handed off during observation")
        return owner, pid, uid

    def unit(self, unit, user=False):
        code = UNIT_OBSERVER_SOURCE + '\nimport json,sys\nprint(json.dumps(observe_unit(sys.argv[1],sys.argv[2]=="user",float(sys.argv[3]))))'
        argv = ["/usr/bin/python3", "-c", code, unit, "user" if user else "system", str(self.deadline)]
        result = json.loads(self.run(self.user(argv) if user else argv))
        if result.pop("Job") != [0, "/"]:
            raise Refusal("unit has an uncompleted manager job", 75)
        return result

    def unit_authority(self, unit, state, profile):
        fragment = state["FragmentPath"]
        if state["LoadState"] == "not-found":
            return
        for dropin in state["DropInPaths"]:
            if dropin not in profile["installed"]:
                raise Refusal("effective unit has an unadmitted drop-in")
            trusted_file(dropin, profile["installed"][dropin])
        if unit == "udevmon.service" and state["LoadState"] == "masked" and fragment in ("/dev/null", "/run/systemd/system/udevmon.service"):
            return  # Exact runtime-mask ownership belongs to the calling orchestrator.
        if unit == self.s["legacy_tray_unit"]:
            if fragment != "/run/user/1000/systemd/transient/" + unit:
                raise Refusal("legacy transient unit fragment differs")
        elif fragment not in profile["installed"]:
            raise Refusal("unit fragment is not admitted by the exact profile")
        else:
            trusted_file(fragment, profile["installed"][fragment])

    def authority(self):
        self.remaining()
        if Path("/proc/sys/kernel/random/boot_id").read_text().strip() != self.s["boot_id"]:
            raise Refusal("guest boot identity changed")
        if Path("/sys/class/dmi/id/product_uuid").read_text().strip().lower() != self.s["domain_uuid"]:
            raise Refusal("disposable guest identity differs")
        for core in self.s["core"].values():
            if self.f.process_identity(core["identity"][0], core["executable"]) != core["identity"] or \
                    Path(f"/proc/{core['identity'][0]}").stat().st_uid != 1000 or \
                    hashlib.sha256(Path(f"/proc/{core['identity'][0]}/exe").read_bytes()).hexdigest() != core["sha256"]:
                raise Refusal("Shell/IBus authority changed")
        user = pwd.getpwnam(self.s["username"])
        if (user.pw_uid, user.pw_gid, user.pw_dir) != (self.s["uid"], self.s["gid"], self.s["home"]):
            raise Refusal("active user mapping changed")
        result = self.run(["/usr/bin/loginctl", "show-session", self.s["session"],
                           "--property=Active", "--property=User", "--property=Type", "--property=State"])
        if set(result.splitlines()) != {"Active=yes", "User=1000", "Type=x11", "State=active"}:
            raise Refusal("active session differs")
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as bus:
            bus.settimeout(self.remaining())
            bus.connect("/run/user/1000/bus")
            pid, uid, _ = struct.unpack("3i", bus.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))
        expected = self.s["session_bus"]
        if uid != 1000 or self.f.process_identity(pid, expected["executable"]) != expected["identity"] or \
                hashlib.sha256(Path(f"/proc/{pid}/exe").read_bytes()).hexdigest() != expected["sha256"]:
            raise Refusal("active session bus peer changed")
        self.remaining()

    def payload(self, name):
        profile = self.s["profiles"][name]
        for path, digest in profile["installed"].items():
            trusted_file(path, digest)
        version = self.run(["/usr/bin/dpkg-query", "--show", "--showformat=${db:Status-Status}:${Version}",
                            "punto-switcher"]).strip()
        if version != "installed:" + profile["version"]:
            raise Refusal("configured package version differs")
        for path, digest in self.s["preserved"].items():
            if hashlib.sha256(Path(path).read_bytes()).hexdigest() != digest:
                raise Refusal("config/store/enrollment preservation differs")
        return profile

    def current_profile(self):
        matches = []
        for name in ("original", "candidate", "exact285"):
            try:
                self.payload(name)
            except Refusal as error:
                if error.status == 75:
                    raise
                continue
            except FileNotFoundError:
                continue
            matches.append(name)
        if not matches:
            raise Refusal("no complete admitted payload profile")
        # Equal original/candidate maps denote identical admitted bytes, not version-only identity.
        if len(matches) > 1 and any(self.s["profiles"][name] != self.s["profiles"][matches[0]] for name in matches[1:]):
            raise Refusal("payload profile is ambiguous")
        return matches[0]

    def admit_writers(self, profile):
        records = self.f.writer_processes()
        by_path = {path: role for role, path in profile["processes"].items()}
        admitted = {}
        for pid, item in records.items():
            role = by_path.get(item["executable"])
            uid = 1000 if role in ("helper", "tray") else 0
            if role is None or role in admitted or item["deleted"] or item["uid"] != uid or \
                    item["sha256"] != profile["installed"].get(item["executable"]):
                raise Refusal("unadmitted/competing actual writer")
            admitted[role] = item
        return admitted

    def tray_owner(self, pid):
        from gi.repository import Gio, GLib
        watcher_name = "org.kde.StatusNotifierWatcher"
        watcher, watcher_pid, watcher_uid = self.bus_peer(watcher_name)
        if watcher != self.s["watcher_owner"] or (watcher_pid, watcher_uid) != (self.s["core"]["gnome"]["identity"][0], 1000):
            raise Refusal("watcher is not the unchanged pinned core owner")
        items, = self.bus(watcher, "/StatusNotifierWatcher",
                          "org.freedesktop.DBus.Properties.Get",
                          (watcher_name, "RegisteredStatusNotifierItems"))
        if type(items) is not list or any(type(item) is not str or not item for item in items):
            raise Refusal("watcher catalogue shape differs")
        hits, seen = [], set()
        for item in items:
            name, separator, path = item.partition("/")
            name = name.removesuffix("@")
            if not separator or not Gio.dbus_is_name(name) or not GLib.Variant.is_object_path("/" + path):
                raise Refusal("watcher catalogue entry is malformed")
            entry = (name, "/" + path)
            if entry in seen:
                raise Refusal("watcher catalogue entry is duplicated")
            seen.add(entry)
            owner, actual, uid = self.bus_peer(name)
            if actual == pid:
                if uid != 1000:
                    raise Refusal("matching tray PID has a foreign UnixUID")
                hits.append(owner)
        after, = self.bus("org.freedesktop.DBus", "/org/freedesktop/DBus",
                          "org.freedesktop.DBus.GetNameOwner", (watcher_name,))
        if after != watcher:
            raise Refusal("watcher handed off during catalogue observation")
        if not hits:
            raise NotPublished("exact tray PID is not registered yet")
        if len(hits) != 1:
            raise Refusal("tray successor lacks one actual StatusNotifier peer")
        return hits[0]

    def effect(self, argv):
        self.authority()
        self.pending_effect = True
        try:
            self.run(argv)
            self.authority()
        except Exception as error:
            raise Refusal("admitted effect completion is not verified", 75) from error
        self.pending_effect = False

    def control(self, verb, unit, *, user=False):
        argv = ["/usr/bin/systemctl", *( ["--user"] if user else [] ), verb, unit]
        self.effect(self.user(argv) if user else argv)

    def start(self, unit, profile, *, user=False):
        state = self.unit(unit, user)
        self.unit_authority(unit, state, profile)
        if state["LoadState"] != "loaded" or state["MainPID"] or state["ActiveState"] not in ("inactive", "failed"):
            raise Refusal("effective successor unit is not admitted and quiescent")
        self.control("start", unit, user=user)

    def quiesce(self, *, rollback=False):
        self.authority()
        name = self.current_profile()
        profile = self.payload(name)
        writers = self.admit_writers(profile)
        if not rollback and writers and \
                (profile != self.s["profiles"][self.s["initial_profile"]] or
                 {role: item["identity"] for role, item in writers.items()} != self.s["initial_writers"]):
            raise Refusal("initial writer PID/start admission changed")
        if not rollback and not writers:
            for role, expected in self.s["initial_writers"].items():
                try:
                    _, fd = self.f.pin_process(expected[0], profile["processes"][role], expected)
                except (FileNotFoundError, ProcessLookupError):
                    continue
                os.close(fd)
                raise Refusal("initial identity is still alive despite writer absence", 75)
        pins = {}
        try:
            for role, item in writers.items():
                identity, fd = self.f.pin_process(item["identity"][0], item["executable"], item["identity"])
                pins[role] = (identity, fd)
            tray_managed = False
            for unit, role, user in (("udevmon.service", "udevmon", False),
                    ("punto-session-bridge.service", "helper", True), ("punto-tray.service", "tray", True),
                    (self.s["legacy_tray_unit"], "tray", True)):
                state = self.unit(unit, user)
                self.unit_authority(unit, state, profile)
                if state["MainPID"]:
                    if role not in writers or state["MainPID"] != writers[role]["identity"][0]:
                        raise Refusal("unit controls an unadmitted writer")
                    tray_managed |= role == "tray"
            if rollback and "tray" in writers and not tray_managed:
                raise Refusal("rollback tray is not an admitted managed successor")
            if "tray" in writers:
                self.tray_owner(writers["tray"]["identity"][0])
            for unit, role, user in (("udevmon.service", "udevmon", False),
                    ("punto-session-bridge.service", "helper", True), ("punto-tray.service", "tray", True),
                    (self.s["legacy_tray_unit"], "tray", True)):
                state = self.unit(unit, user)
                self.unit_authority(unit, state, profile)
                if state["LoadState"] != "not-found":
                    if state["MainPID"] and (role not in writers or state["MainPID"] != writers[role]["identity"][0]):
                        raise Refusal("unit authority changed before stop")
                    self.control("stop", unit, user=user)
            if "tray" in pins and not select.select([pins["tray"][1]], [], [], 0)[0]:
                self.authority()
                self.pending_effect = True
                signal.pidfd_send_signal(pins["tray"][1], signal.SIGTERM)
                self.pending_effect = False
            for _, fd in pins.values():
                if not select.select([fd], [], [], self.remaining())[0]:
                    raise Refusal("admitted old writer death is unknown", 75)
            if self.f.writer_processes():
                raise Refusal("writer respawn/competitor remains")
            self.authority()
            return {"writers_dead": len(pins), "runtime_verified": "quiescent", "input_verified": False}
        finally:
            for _, fd in pins.values():
                os.close(fd)

    def helper(self, identity):
        directory = Path("/run/user/1000/punto")
        descriptor = directory / "session-bridge.json"
        for path, mode in ((directory, 0o700), (descriptor, 0o600)):
            try:
                meta = path.lstat()
            except FileNotFoundError as error:
                raise NotPublished("helper directory/descriptor is not published yet") from error
            if stat.S_ISLNK(meta.st_mode) or meta.st_uid != 1000 or stat.S_IMODE(meta.st_mode) != mode:
                raise Refusal("helper descriptor metadata differs")
            if (path == directory and not stat.S_ISDIR(meta.st_mode)) or (path == descriptor and
                    (not stat.S_ISREG(meta.st_mode) or meta.st_nlink != 1 or meta.st_size > 4096)):
                raise Refusal("helper descriptor type/link/size differs")
        before = descriptor.read_bytes()
        value = json.loads(before)
        instance = value.get("helper_instance", "")
        if value.get("protocol_version") != 1 or not re.fullmatch("[0-9a-f]{32}", instance) or \
                [value.get("pid"), str(value.get("start_time"))] != identity:
            raise Refusal("fresh helper descriptor identity differs")
        endpoint = directory / ("session-bridge-" + instance + ".sock")
        meta = endpoint.lstat()
        if value.get("socket_path") != str(endpoint) or not stat.S_ISSOCK(meta.st_mode) or \
                meta.st_uid != 1000 or stat.S_IMODE(meta.st_mode) != 0o600:
            raise Refusal("fresh helper endpoint differs")
        owner, pid, uid = self.bus_peer("org.punto.Switcher.SessionBridge1")
        if (pid, uid) != (identity[0], 1000) or descriptor.read_bytes() != before:
            raise Refusal("fresh helper bus/descriptor changed")
        after, = self.bus("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus.GetNameOwner",
                          ("org.punto.Switcher.SessionBridge1",))
        if after != owner:
            raise Refusal("helper mapping changed after descriptor validation")
        return owner

    def runtime_ready(self, profile, identity, legacy):
        self.remaining()
        self.authority()
        if {role: item["identity"] for role, item in self.admit_writers(profile).items()} != identity:
            raise Refusal("successor identity changed during publication settlement")
        self.f.pipeline_facts(self.f.process_snapshot(), identity, self.s["evdev"], self.run, privilege=())
        units = [("udevmon.service", "udevmon", False),
                 (self.s["legacy_tray_unit"] if legacy else "punto-tray.service", "tray", True)]
        if not legacy:
            units.append(("punto-session-bridge.service", "helper", True))
        for unit, role, user in units:
            state = self.unit(unit, user)
            self.unit_authority(unit, state, profile)
            if state["LoadState"] != "loaded" or state["ActiveState"] != "active" or state["MainPID"] != identity[role][0]:
                raise Refusal("successor unit runtime projection differs")
        self.authority()

    def await_publication(self, profile, identity, legacy):
        helper_seen = None
        while True:
            self.runtime_ready(profile, identity, legacy)
            try:
                helper = None if legacy else self.helper(identity["helper"])
            except NotPublished as error:
                if helper_seen is not None:
                    raise Refusal("validated helper publication was withdrawn") from error
                self.runtime_ready(profile, identity, legacy)
                time.sleep(min(.025, self.remaining()))
                continue
            if helper_seen is not None and helper != helper_seen:
                raise Refusal("validated helper publication handed off")
            helper_seen = helper
            try:
                tray = self.tray_owner(identity["tray"][0])
            except NotPublished:
                self.runtime_ready(profile, identity, legacy)
                time.sleep(min(.025, self.remaining()))
                continue
            self.runtime_ready(profile, identity, legacy)
            return helper, tray

    def activate(self, name):
        self.authority()
        profile = self.payload(name)
        if self.f.writer_processes():
            raise Refusal("activation requires already-quiescent writer set")
        self.effect(self.user(["/usr/bin/systemctl", "--user", "daemon-reload"]))
        legacy = name == "exact285"
        starts = [("udevmon.service", False)]
        if not legacy:
            starts += [("punto-session-bridge.service", True), ("punto-tray.service", True)]
        for unit, user in starts:
            state = self.unit(unit, user)
            self.unit_authority(unit, state, profile)
            if state["LoadState"] != "loaded" or state["MainPID"] or state["ActiveState"] not in ("inactive", "failed"):
                raise Refusal("effective successor unit is not admitted and quiescent")
        if legacy and self.unit(self.s["legacy_tray_unit"], True)["LoadState"] != "not-found":
            raise Refusal("legacy tray transient unit already exists")
        if not legacy:
            self.start("punto-session-bridge.service", profile, user=True)
        self.start("udevmon.service", profile)
        if legacy:
            if self.unit(self.s["legacy_tray_unit"], True)["LoadState"] != "not-found":
                raise Refusal("legacy tray transient unit already exists")
            self.effect(self.user(["/usr/bin/systemd-run", "--user", "--collect", "--property=Type=exec",
                                  "--unit=" + self.s["legacy_tray_unit"], "/usr/local/bin/punto-tray"]))
        else:
            self.start("punto-tray.service", profile, user=True)
        # These are observations of accepted jobs, never repeated effect commands.
        while True:
            writers = self.admit_writers(profile)
            if set(writers) == set(profile["processes"]):
                break
            self.remaining()
            time.sleep(min(.025, self.remaining()))
        identity = {role: item["identity"] for role, item in writers.items()}
        helper_owner, tray_owner = self.await_publication(profile, identity, legacy)
        self.authority()
        try:
            self.f.runtime_status(self.s["original_enabled"], identity["daemon"][0],
                                  profile="exact285-gtk16" if legacy else "modern", _deadline=self.deadline,
                                  failure=Refusal)
        except (OSError, Refusal) as error:
            raise Refusal("enabled projection completion is not confirmed", 75) from error
        self.authority()
        self.payload(name)
        if self.admit_writers(profile) != writers:
            raise Refusal("successor writers changed during postflight")
        self.authority()
        return {"profile": name, "identity": identity, "tray_owner": tray_owner,
                "helper_owner": helper_owner, "runtime_verified": True, "input_verified": False}

    def rollback_profile(self, version, snapshot):
        helper = self.s["files_helper"]
        trusted_file(helper["path"], helper["sha256"])
        self.run([helper["path"], "verify", "/", self.s["exact285_archive"],
                  self.s["candidate_archive"], snapshot])
        kind = trusted_file(Path(snapshot) / "kind").decode().strip()
        name = "original" if kind == "snapshot-original" else "exact285" if kind in ("snapshot", "snapshot-285") else None
        if name is None or self.s["profiles"][name]["version"] != version:
            raise Refusal("rollback snapshot kind/version differs")
        profile = self.payload(name)
        if name == "original":
            for path in ("usr/bin/punto-daemon", "usr/libexec/punto-session-bridge",
                    "usr/share/punto-switcher/protocol/punto-dbus-v1.xml",
                    "etc/xdg/autostart/punto-session-bridge.desktop"):
                trusted_file(Path(snapshot) / "overlay" / path, profile["installed"]["/" + path])
        return name


def dispatch(provider, argv, lifecycle_mode):
    if lifecycle_mode not in ("baseline285", "fresh", "upgrade"):
        raise Refusal("unsupported lifecycle admission", 64)
    if argv == []:
        return provider.activate("exact285" if lifecycle_mode == "baseline285" else "candidate")
    if argv == ["quiesce-v1", lifecycle_mode]:
        return provider.quiesce()
    if argv == ["quiesce-v1", lifecycle_mode, "rollback"]:
        return provider.quiesce(rollback=True)
    if len(argv) == 3 and argv[0] == "rollback-v1":
        return provider.activate(provider.rollback_profile(argv[1], argv[2]))
    raise Refusal("unsupported callback ABI", 64)


def main(argv):
    start = time.monotonic()
    if os.geteuid() != 0:
        raise Refusal("callback requires the admitted root orchestrator")
    spec_path = os.environ["PUNTO_PROVIDER_SPEC"]
    spec = json.loads(trusted_file(spec_path))
    if spec.get("contract") != "punto-installed-runtime-v1" or spec.get("domain_uuid") != \
            "e5926030-f735-4637-936c-4af8ee226f70" or spec.get("uid") != 1000 or \
            type(spec.get("original_enabled")) is not bool or \
            not re.fullmatch(r":[0-9]+\.[0-9]+", spec.get("watcher_owner", "")) or \
            not re.fullmatch(r"punto-legacy-tray-[0-9a-f]{32}\.service", spec.get("legacy_tray_unit", "")):
        raise Refusal("explicit disposable runtime contract differs")
    budget = spec["callback_budget_seconds"]
    if type(budget) is not int or not 1 <= budget <= int(os.environ.get("PUNTO_ACTIVATION_TIMEOUT_SECONDS", "60")) <= 120:
        raise Refusal("callback budget exceeds outer admitted deadline")
    for path, digest in spec["dependencies"].items():
        trusted_file(path, digest)
    if str(Path(__file__)) not in spec["dependencies"] or spec["facts_module"] not in spec["dependencies"]:
        raise Refusal("callback/shared import are not admitted dependencies")
    legacy = spec["profiles"]["exact285"]
    legacy_payload = {"/usr/local/bin/punto-daemon": "edc37fc25bd16d79e8a1252d1e8a9ef012099b271f8c8b6e7f17176d173cd6a6",
                      "/usr/local/bin/punto": "07b56664729be21303560f7912028e0c64e056b375573dcfd21d92875e3b4579",
                      "/usr/local/bin/punto-tray": "45881738bda0433702bcbe2bc211e07f343f109360a2a75840a43676cddf5db1"}
    if set(spec["profiles"]) != {"original", "candidate", "exact285"} or \
            spec["initial_profile"] not in spec["profiles"] or legacy["version"] != "2.8.5" or \
            any(legacy["installed"].get(path) != digest for path, digest in legacy_payload.items()) or \
            "helper" in legacy["processes"] or legacy["processes"].get("daemon") != "/usr/local/bin/punto-daemon":
        raise Refusal("exact archived baseline profile differs")
    for name, profile in spec["profiles"].items():
        roles = {"udevmon", "interception", "daemon", "uinput", "tray"}
        if name != "exact285":
            roles.add("helper")
        if set(profile["processes"]) != roles or any(path not in profile["installed"] for path in profile["processes"].values()):
            raise Refusal("complete writer role/hash projection differs")
    if set(spec["initial_writers"]) != set(spec["profiles"][spec["initial_profile"]]["processes"]):
        raise Refusal("initial writer identities are incomplete")
    trusted_file(spec["exact285_archive"], "80c977e4b195f73bdbf89c9e2ba8b1c4162a6c318ea74142fedd6010d222ebd4")
    for field in ("candidate_archive", "facts_module"):
        if spec[field] not in spec["dependencies"]:
            raise Refusal("staged archive/import dependency is not frozen")
    if spec["dependencies"].get(spec["files_helper"]["path"]) != spec["files_helper"]["sha256"]:
        raise Refusal("snapshot verifier is not an admitted dependency")
    module_bytes = trusted_file(spec["facts_module"], spec["dependencies"][spec["facts_module"]])
    facts = types.ModuleType("punto_runtime_facts")
    exec(compile(module_bytes, spec["facts_module"], "exec"), facts.__dict__)
    provider = Provider(spec, facts, start + budget)
    result = dispatch(provider, argv, spec["lifecycle_mode"])
    print(json.dumps(result, sort_keys=True))
    return 0


def interrupted(signum, frame):
    raise Refusal("callback interrupted; accepted work may be unknown", 75)


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGINT, interrupted)
    try:
        sys.exit(main(sys.argv[1:]))
    except Refusal as error:
        print("punto-runtime: " + str(error), file=sys.stderr)
        sys.exit(error.status)
    except Exception as error:
        print("punto-runtime: observation refused: " + str(error), file=sys.stderr)
        sys.exit(66)
