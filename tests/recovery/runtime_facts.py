"""Shared installed-process facts and pinned public status exchange."""

import hashlib
import os
import socket
import struct
import time
from pathlib import Path


class FactsFailure(RuntimeError):
    def __init__(self, message, kind="ABORTED_SAFETY"):
        super().__init__(message)
        self.kind = kind


class ProcessesNotPublished(FactsFailure):
    def __init__(self, identity):
        super().__init__("successor process graph is not yet published", "NOT_YET_PUBLISHED")
        self.identity = identity.copy()


def runtime_status(enabled=None, expected_pid=None, *, profile="modern", _deadline=None,
                   failure=FactsFailure, socket_module=socket, clock=time.monotonic):
    """Only the documented, unprivileged public status commands are admitted."""
    if enabled is not None and type(enabled) is not bool:
        raise failure("invalid public status argument")
    if profile not in ("modern", "exact285-gtk16"):
        raise failure("unsupported status wire profile")
    legacy = profile == "exact285-gtk16"
    if legacy and (type(expected_pid) is not int or expected_pid <= 0):
        raise failure("legacy status requires the pinned daemon peer")
    bounded = legacy or _deadline is not None
    deadline = (_deadline if _deadline is not None else clock() + 1) if bounded else None
    command = "GET_STATUS" if enabled is None else f"SET_STATUS {int(enabled)}"
    with socket_module.socket(socket_module.AF_UNIX, socket_module.SOCK_STREAM) as client:
        if bounded and clock() >= deadline:
            raise failure("public status deadline expired", "INVALID_OBSERVATION")
        client.settimeout(deadline - clock() if bounded else 1)
        client.connect("/run/punto.sock")
        if expected_pid is not None:
            pid, uid, _ = struct.unpack("3i", client.getsockopt(socket_module.SOL_SOCKET, socket_module.SO_PEERCRED, struct.calcsize("3i")))
            if pid != expected_pid or uid != 0:
                raise failure("public status peer is not the pinned daemon", "ABORTED_SAFETY")
        if bounded:
            remaining = deadline - clock()
            if remaining <= 0:
                raise failure("public status deadline expired", "INVALID_OBSERVATION")
            client.settimeout(remaining)
        client.sendall((command + "\n").encode())
        result = bytearray()
        deadline = deadline if bounded else clock() + 1
        while b"\n" not in result and len(result) < 128:
            client.settimeout(max(0.001, deadline - clock()))
            part = client.recv(128 - len(result))
            if not part or clock() >= deadline:
                break
            result.extend(part)
    replies = (b"OK ENABLED\n", b"OK DISABLED\n")
    if bytes(result) not in replies or (bounded and clock() >= deadline):
        raise failure("public status command not confirmed", "FAIL_PRODUCT")
    actual = bytes(result) == replies[0]
    readback = {"profile": profile, "_deadline": deadline} if bounded else {}
    readback.update(failure=failure, socket_module=socket_module, clock=clock)
    if enabled is not None and (actual != enabled or runtime_status(expected_pid=expected_pid, **readback) != enabled):
        raise failure("public status readback differs", "FAIL_PRODUCT")
    return actual



def process_snapshot(path=Path):
    processes = {}
    for item in path("/proc").glob("[0-9]*/stat"):
        try:
            args = (item.parent / "cmdline").read_bytes().split(b"\0")
            raw = item.read_text().rsplit(")", 1)[1].split()
            processes[int(item.parent.name)] = (args, int(raw[1]), raw[19])
        except (FileNotFoundError, PermissionError, ProcessLookupError):
            continue
    return processes


def process_identities(processes, executables, hashes, run, *,
                       privilege=("sudo", "-n"), failure=FactsFailure, path=Path,
                       not_yet_published_roles=(), expected=None):
    identity = {}
    missing = False
    expected = expected or {}
    for role, executable in executables.items():
        argv_names = {executable.encode(), path(executable).name.encode()}
        hits = [pid for pid, (args, _, _) in processes.items() if args[0] in argv_names]
        if not hits and role in not_yet_published_roles and role not in expected:
            missing = True
            continue
        if len(hits) != 1:
            raise failure(f"expected exactly one pinned {role} process")
        actual = run([*privilege, "readlink", f"/proc/{hits[0]}/exe"]).strip()
        if actual != str(path(executable).resolve()):
            raise failure(f"actual pinned {role} executable differs or was replaced")
        if executable in hashes:
            live_hash = run([*privilege, "sha256sum", f"/proc/{hits[0]}/exe"]).split()[0]
            if live_hash != hashes[executable]:
                raise failure(f"running {role} ELF differs from candidate payload")
        identity[role] = [hits[0], processes[hits[0]][2]]
        if role in expected and identity[role] != expected[role]:
            raise failure(f"pinned {role} process identity changed")
    if missing:
        raise ProcessesNotPublished(identity)
    return identity


def pipeline_facts(processes, identity, evdev, run, *,
                   privilege=("sudo", "-n"), failure=FactsFailure):
    ids = {role: identity[role][0] for role in ("udevmon", "interception", "daemon", "uinput")}
    def link(pid, fd):
        return run([*privilege, "readlink", f"/proc/{pid}/fd/{fd}"]).strip()
    fds = run([*privilege, "ls", f"/proc/{ids['interception']}/fd"]).split()
    args = processes[ids["interception"]][0]
    if evdev.encode() not in args or evdev not in [link(ids["interception"], fd) for fd in fds]:
        raise failure("selected evdev is not owned by the pinned interception process")
    for left, right in (("interception", "daemon"), ("daemon", "uinput")):
        pipe = link(ids[left], "1")
        if not pipe.startswith("pipe:[") or pipe != link(ids[right], "0"):
            raise failure("input pipeline pipe identity differs")
    for role in ("interception", "daemon", "uinput"):
        parent = ids[role]
        for _ in range(30):
            parent = processes.get(parent, (None, 0, None))[1]
            if parent in (0, ids["udevmon"]):
                break
        if parent != ids["udevmon"]:
            raise failure("input pipeline does not have the pinned udevmon ancestor")
    fds = run([*privilege, "ls", f"/proc/{ids['uinput']}/fd"]).split()
    if "/dev/uinput" not in [link(ids["uinput"], fd) for fd in fds]:
        raise failure("pinned output process lacks the actual uinput handle")


def process_identity(pid, executable, *, failure=FactsFailure, path=Path):
    item = path("/proc") / str(pid)
    raw = (item / "stat").read_text().rsplit(")", 1)[1].split()
    if raw[0] in ("T", "t", "Z") or (item / "exe").resolve() != path(executable).resolve():
        raise failure("owned terminal process stopped or executable differs", "ABORTED_SAFETY")
    return [pid, raw[19]]


def pin_process(pid, executable, expected=None, *, failure=FactsFailure):
    identity = process_identity(pid, executable, failure=failure)
    if expected is not None and identity != expected:
        raise failure("writer PID/start no longer exists")
    fd = os.pidfd_open(pid)
    try:
        if process_identity(pid, executable, failure=failure) != identity:
            raise failure("writer ownership changed while pinning")
    except BaseException:
        os.close(fd)
        raise
    return identity, fd


def writer_processes():
    """Actual executable links, including renamed argv and deleted inodes."""
    records = {}
    names = {"punto-daemon", "punto-session-bridge", "punto-tray", "interception", "intercept", "uinput", "udevmon"}
    for item in Path("/proc").glob("[0-9]*/exe"):
        try:
            executable = os.readlink(item)
            normalized = executable.removesuffix(" (deleted)")
            if Path(normalized).name not in names:
                continue
            raw = (item.parent / "stat").read_text().rsplit(")", 1)[1].split()
            records[int(item.parent.name)] = {
                "executable": normalized, "deleted": executable != normalized,
                "identity": [int(item.parent.name), raw[19]], "uid": item.parent.stat().st_uid,
                "sha256": hashlib.sha256(item.read_bytes()).hexdigest()}
            if (item.parent / "stat").read_text().rsplit(")", 1)[1].split()[19] != raw[19]:
                raise FactsFailure("writer changed during actual executable observation")
        except (FileNotFoundError, ProcessLookupError):
            continue
    return records
