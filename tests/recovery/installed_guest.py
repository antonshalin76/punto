#!/usr/bin/env python3
"""Collect a scoped installed GTK/Chromium vertical; not the signed R10 probe."""

from __future__ import annotations

import argparse
import base64
import ctypes as C
from contextlib import ExitStack
import hashlib
import ipaddress
import json
import os
import re
import select
import shlex
import signal
import socket
import stat
import struct
import subprocess
import sys
import threading
import time
import uuid
from pathlib import Path

from runtime_facts import process_snapshot, process_identities, pipeline_facts, process_identity
from runtime_facts import ProcessesNotPublished
from runtime_facts import runtime_status as shared_runtime_status


class Failure(RuntimeError):
    def __init__(self, message, kind="BLOCKED_PRECONDITION"):
        super().__init__(message)
        self.kind = kind


class ClipboardNotRestored(Failure):
    pass


class StatsNotPublished(Failure):
    pass


def checked_run(argv, timeout=5, input=None, binary=False):
    try:
        result = subprocess.run(argv, input=input, capture_output=True, timeout=timeout, check=False)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise Failure(f"bounded command failed: {type(error).__name__}") from error
    if result.returncode:
        raise Failure(f"command exit {result.returncode}: {result.stderr[:300].decode(errors='replace')}")
    return result.stdout if binary else result.stdout.decode()


def check_files(files):
    for name, expected in files.items():
        path = Path(name)
        if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != expected:
            raise Failure(f"payload/source drift: {name}")


def ordinary_environment(environment):
    if environment.get("XDG_SESSION_TYPE") != "x11":
        raise Failure("ordinary X11 session required")
    if any(name in environment for name in ("GTK_MODULES", "NO_AT_BRIDGE", "GTK_IM_MODULE_FILE")):
        raise Failure("non-ordinary target environment")


def owned_exec(command):
    """Kernel parent-death cleanup without threaded preexec_fn callbacks."""
    parent = os.getppid()
    if C.CDLL(None).prctl(1, 15, 0, 0, 0) or os.getppid() != parent:
        raise Failure("parent-death cleanup unavailable")
    os.execvp(command[0], command)


def guest_group():
    """SSH may already have made this collector its private group leader."""
    identity = os.getpid()
    if os.getpgrp() != identity:
        os.setsid()
    if os.getpgrp() != identity:
        raise Failure("guest collector does not own a private group", "ABORTED_SAFETY")
    return identity


def kill_guest_group(identity):
    if identity != os.getpid() or os.getpgrp() != identity:
        raise Failure("refusing to kill a shared guest group", "ABORTED_SAFETY")
    os.killpg(identity, signal.SIGKILL)


def qmp_keys(keys):
    permitted = set("abcdefghijklmnopqrstuvwxyz") | {"pause", "shift", "home", "right", "space", "ctrl", "alt", "backspace", "tab"}
    if not keys or keys == ["ctrl"] or any(key not in permitted for key in keys) or len(set(keys)) != len(keys):
        raise Failure("key outside fixed scratch-input whitelist")
    if len(keys) > 1 and keys not in (["shift", "home"], ["shift", "pause"], ["ctrl", "a"],
            ["ctrl", "pause"], ["alt", "pause"], ["ctrl", "alt", "pause"], ["ctrl", "z"]) and not (
            len(keys) == 2 and keys[0] == "shift" and keys[1] in "abcdefghijklmnopqrstuvwxyz"):
        raise Failure("unsupported compound chord")
    events = [{"type": "key", "data": {"down": down, "key": {"type": "qcode", "data": "spc" if key == "space" else key}}}
              for down, sequence in ((True, keys), (False, list(reversed(keys)))) for key in sequence]
    return json.dumps({"execute": "input-send-event", "arguments": {"events": events}})


def terminal_enter_payload(admission):
    if (not isinstance(admission, dict) or admission.get("surface") != "terminal" or
            not admission.get("run_id") or not admission.get("nonce") or
            not isinstance(admission.get("xid"), int) or admission["xid"] <= 1 or
            not isinstance(admission.get("server"), list) or len(admission["server"]) != 2 or
            not isinstance(admission.get("reader"), list) or len(admission["reader"]) != 2 or
            not isinstance(admission.get("expires_ns"), int) or
            time.monotonic_ns() >= admission["expires_ns"]):
        raise Failure("Enter lacks a fresh owned canonical terminal admission", "ABORTED_SAFETY")
    return json.dumps({"execute": "input-send-event", "arguments": {"events": [
        {"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": "ret"}}},
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": "ret"}}}]}})


def terminal_ready(value, nonce, directory):
    import termios
    required = termios.ICANON | termios.ISIG | termios.ECHO
    if (value.get("nonce") != nonce or value.get("directory") != str(directory) or
            not isinstance(value.get("pid"), int) or value["pid"] <= 1 or
            not isinstance(value.get("monitor"), int) or value["monitor"] <= 1 or
            not isinstance(value.get("sid"), int) or value["sid"] <= 0 or
            value.get("pgrp") != value.get("foreground_pgrp") or
            value.get("pgrp") == value.get("monitor_pgrp") or
            value.get("sid") != value.get("monitor_sid") or
            value.get("lflag", 0) & required != required or
            value.get("vsusp_hex") != "1a" or
            not str(value.get("tty", "")).startswith("/dev/pts/")):
        raise Failure("reader is not the exact owned foreground canonical PTY")
    return value


def terminal_submission(actual, expected):
    if actual is None:
        return False
    if actual != base64.b64encode(expected).decode():
        raise Failure("canonical PTY submitted bytes differ", "FAIL_PRODUCT")
    return True


def terminal_reader_main(directory, nonce):
    """Finite canonical input sink; input is data and is never executed."""
    import termios
    directory = Path(directory)
    if (directory.parent != Path(__file__).parent or not directory.name.startswith("terminal-") or
            directory.is_symlink() or directory.stat().st_uid != os.getuid() or
            directory.stat().st_mode & 0o077 or not nonce.isalnum()):
        raise Failure("terminal reader scratch scope differs")
    fd = sys.stdin.fileno()
    # A live same-session parent makes the foreground reader group non-orphaned:
    # an escaped Ctrl+Z must produce SIGTSTP rather than be silently discarded.
    monitor = os.getpid()
    gate_read, gate_write = os.pipe()
    child = os.fork()
    if child:
        os.close(gate_read)
        os.setpgid(child, child)
        signal.signal(signal.SIGTTOU, signal.SIG_IGN)
        os.tcsetpgrp(fd, child)
        os.write(gate_write, b"1")
        os.close(gate_write)
        child_fd = os.pidfd_open(child)
        def monitor_exit(_signum, _frame):
            try:
                signal.pidfd_send_signal(child_fd, signal.SIGKILL)
            except ProcessLookupError:
                pass
            raise SystemExit(75)
        signal.signal(signal.SIGALRM, monitor_exit)
        signal.signal(signal.SIGTERM, monitor_exit)
        signal.alarm(60)
        _, status = os.waitpid(child, 0)
        os.close(child_fd)
        raise SystemExit(os.waitstatus_to_exitcode(status))
    os.close(gate_write)
    if os.read(gate_read, 1) != b"1":
        raise SystemExit(77)
    os.close(gate_read)
    def process_start(pid):
        return (Path("/proc") / str(pid) / "stat").read_text().rsplit(")", 1)[1].split()[19]
    ready = {"nonce": nonce, "directory": str(directory), "pid": os.getpid(),
             "start": process_start(os.getpid()), "monitor_start": process_start(monitor),
             "pgrp": os.getpgrp(), "foreground_pgrp": os.tcgetpgrp(fd),
             "monitor": monitor, "monitor_pgrp": os.getpgid(monitor),
             "sid": os.getsid(0), "monitor_sid": os.getsid(monitor),
             "tty": os.ttyname(fd), "lflag": termios.tcgetattr(fd)[3],
             "vsusp_hex": termios.tcgetattr(fd)[6][termios.VSUSP].hex()}
    terminal_ready(ready, nonce, directory)
    def prohibited(signum, _frame):
        with (directory / "signals").open("ab") as stream:
            stream.write(str(signum).encode() + b"\n")
        raise SystemExit(74)
    for signum in (signal.SIGTSTP, signal.SIGINT, signal.SIGQUIT):
        signal.signal(signum, prohibited)
    signal.signal(signal.SIGALRM, lambda _s, _f: sys.exit(75))
    signal.alarm(60)
    (directory / "ready.tmp").write_text(json.dumps(ready))
    (directory / "ready.tmp").replace(directory / "ready.json")
    os.write(1, ("PUNTO-" + nonce + "\nPUNTO-" + nonce + ">").encode())
    value = os.read(fd, 4096)
    if not value.endswith(b"\n") or any(byte < 32 and byte != 10 for byte in value):
        raise SystemExit(76)
    with (directory / "submitted").open("xb") as stream:
        stream.write(value)
    os.write(1, ("\nPUNTO-" + nonce + ">").encode())
    # Keep the admitted window alive for exact receipt/authority readback.
    select.select([fd], [], [], 30)
    raise SystemExit(0)

class Guard:
    def __init__(self, vm, identity, run=checked_run):
        self.vm, self.identity, self.run = vm, identity, run

    def check(self):
        if self.vm != "punto-r23-native":
            raise Failure("only the disposable Punto domain is allowed")
        prefix = ["virsh", "-c", "qemu:///system"]
        if self.run(prefix + ["domuuid", self.vm]).strip() != self.identity:
            raise Failure("domain UUID changed")
        if self.run(prefix + ["domstate", self.identity]).strip() != "running":
            raise Failure("domain is not running")

    def send(self, keys, deadline_ns=None, terminal_admission=None):
        payload = terminal_enter_payload(terminal_admission) if keys == ["enter"] else qmp_keys(keys)
        self.check()
        if keys == ["enter"]:
            terminal_enter_payload(terminal_admission)
        if deadline_ns is not None and time.monotonic_ns() >= deadline_ns:
            raise Failure("undo admission exceeded 2500ms before QMP")
        result = json.loads(self.run(["virsh", "-c", "qemu:///system", "qemu-monitor-command",
                                      self.identity, payload]))
        if "error" in result or result.get("return") != {}:
            raise Failure("QMP input failed or is uncertain; no resend")
        if deadline_ns is not None and time.monotonic_ns() >= deadline_ns:
            raise Failure("undo QMP completion outside measured budget", "INVALID_OBSERVATION")


class Lines:
    """Bounded, correlated JSON lines over one owned subprocess."""
    def __init__(self, command, stderr_path=None):
        command = [sys.executable, __file__, "--exec", *command]
        self.child = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                      stderr=subprocess.PIPE if stderr_path else subprocess.DEVNULL,
                                      bufsize=0, start_new_session=True)
        self.buffer = bytearray()
        self.terminal_failure = None
        os.set_blocking(self.child.stdin.fileno(), False)
        self.stderr_thread = None
        self.stderr_error = None
        if stderr_path:
            def drain():
                retained = 0
                try:
                    with Path(stderr_path).open("xb") as stream:
                        while data := self.child.stderr.read(65536):
                            saved = data[:max(0, 65536 - retained)]
                            stream.write(saved)
                            stream.flush()
                            retained += len(saved)
                except Exception as error:
                    self.stderr_error = type(error).__name__
            self.stderr_thread = threading.Thread(target=drain, daemon=True)
            self.stderr_thread.start()

    def send(self, value, timeout=10, *, deadline_ns=None):
        if getattr(self, "terminal_failure", None) is not None:
            raise self.terminal_failure
        data = json.dumps(value).encode() + b"\n"
        if len(data) > 1_048_576:
            raise Failure("oversized transport request")
        deadline = deadline_ns / 1e9 if deadline_ns is not None else time.monotonic() + timeout
        if time.monotonic() >= deadline:
            raise Failure("observer request deadline expired before dispatch", "INVALID_OBSERVATION")
        while data:
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not select.select([], [self.child.stdin], [], remaining)[1]:
                self.terminal_failure = Failure("observer/SSH request write timeout",
                    "INVALID_OBSERVATION" if deadline_ns is not None else "ABORTED_SAFETY")
                raise self.terminal_failure
            try:
                data = data[os.write(self.child.stdin.fileno(), data):]
            except BlockingIOError:
                continue
            except BrokenPipeError as error:
                self.terminal_failure = Failure("observer/SSH pipe closed during dispatch")
                raise self.terminal_failure from error

    def read(self, timeout=10, *, deadline_ns=None):
        if getattr(self, "terminal_failure", None) is not None:
            raise self.terminal_failure
        deadline = deadline_ns / 1e9 if deadline_ns is not None else time.monotonic() + timeout
        while b"\n" not in self.buffer:
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not select.select([self.child.stdout], [], [], remaining)[0]:
                self.terminal_failure = Failure("observer/SSH response timeout",
                    "INVALID_OBSERVATION" if deadline_ns is not None else "ABORTED_SAFETY")
                raise self.terminal_failure
            data = os.read(self.child.stdout.fileno(), 65536)
            if not data or len(self.buffer) + len(data) > 1_048_576:
                self.terminal_failure = Failure("observer/SSH EOF or oversized response")
                raise self.terminal_failure
            self.buffer.extend(data)
        line, _, rest = self.buffer.partition(b"\n")
        self.buffer = bytearray(rest)
        value = json.loads(line)
        if value.get("ok") is False:
            error = Failure(value.get("error", "guest failure"), value.get("kind", "BLOCKED_PRECONDITION"))
            for note in value.get("notes", [])[:8]:
                if isinstance(note, str):
                    error.add_note(note[:1024])
            if value.get("terminal") is True:
                self.terminal_failure = error
            raise error
        return value

    def call(self, value, *, deadline_ns=None):
        started = time.monotonic_ns()
        deadline_ns = deadline_ns if deadline_ns is not None else started + 10_000_000_000
        self.send(value, deadline_ns=deadline_ns)
        result = self.read(deadline_ns=deadline_ns)
        finished = time.monotonic_ns()
        if finished >= deadline_ns:
            raise Failure("observer response exceeded shared deadline", "INVALID_OBSERVATION")
        if "request_id" in value and result.get("request_id") != value["request_id"]:
            raise Failure("stale or uncorrelated SSH response")
        result["host_interval_ns"] = [started, finished]
        return result

    def close(self):
        if self.child.stdin:
            self.child.stdin.close()
        try:
            self.child.wait(timeout=3)
        except subprocess.TimeoutExpired:
            self.child.terminate()
            try:
                self.child.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.child.kill()
                self.child.wait(timeout=2)
        if self.stderr_thread:
            self.stderr_thread.join(timeout=1)
            if self.stderr_thread.is_alive():
                raise Failure("owned stderr drain did not close", "ABORTED_SAFETY")
        self.child.stdout.close()
        if self.child.stderr:
            self.child.stderr.close()
        if self.stderr_error:
            raise Failure(f"owned stderr retention failed: {self.stderr_error}", "ABORTED_SAFETY")
        if self.child.returncode:
            raise Failure("owned transport/observer exited unsuccessfully", "ABORTED_SAFETY")


def verify_state(before, after, text, selection, group, *, expected_focus=None,
                 expected_following=None, clipboard_payload_restore=False, desktop_pending_group=None):
    state = after["state"]
    if "desktop" in before or "desktop" in after:
        verify_desktop(before, after, group if desktop_pending_group is None else desktop_pending_group,
                       transitional=desktop_pending_group is not None)
    if (state["text"], state["selection"], after["group"]) != (text, selection, group):
        raise Failure("actual text/caret/selection/layout mismatch", "FAIL_PRODUCT")
    for field in ("clipboard", "identity"):
        if field == "clipboard" and clipboard_payload_restore:
            original, restored = before[field], after[field]
            metadata = {"TARGETS", "TIMESTAMP", "MULTIPLE", "SAVE_TARGETS"}
            if not original.get("representations") or not restored.get("owner") or any(
                    original.get(part) != restored.get(part) for part in ("bytes_base64", "representations")) or (
                    set(original["targets"].splitlines()) - metadata != set(restored["targets"].splitlines()) - metadata):
                raise ClipboardNotRestored("Tab plain clipboard representations not restored", "ABORTED_SAFETY")
            continue
        if after[field] != before[field]:
            raise Failure(f"{field} changed", "ABORTED_SAFETY")
    if after["pressed"] or not after["foreground"]:
        raise Failure("held key or lost owned foreground", "ABORTED_SAFETY")
    if state["following_text"] != (before["state"]["following_text"] if expected_following is None else expected_following):
        raise Failure("foreign control changed", "ABORTED_SAFETY")
    if "focus_target" in before["state"] and state.get("focus_target") != (expected_focus or before["state"]["focus_target"]):
        raise Failure("focused text control changed", "ABORTED_SAFETY")
    if "sentinel_input" in before["state"] and state.get("sentinel_input") != before["state"]["sentinel_input"]:
        raise Failure("browser sentinel changed", "ABORTED_SAFETY")
    for field in ("observer_instance_id", "run_id", "case_id", "attempt_id"):
        if field in before and after.get(field) != before[field]:
            raise Failure("observer attempt/instance changed", "INVALID_OBSERVATION")
    if "observation_id" in before and after.get("observation_id", 0) <= before["observation_id"]:
            raise Failure("observer snapshot is not fresh", "INVALID_OBSERVATION")


class XkbState(C.Structure):
    _fields_ = [
        ("group", C.c_ubyte), ("locked_group", C.c_ubyte),
        ("base_group", C.c_ushort), ("latched_group", C.c_ushort),
        ("mods", C.c_ubyte), ("base_mods", C.c_ubyte),
        ("latched_mods", C.c_ubyte), ("locked_mods", C.c_ubyte),
        ("compat_state", C.c_ubyte), ("grab_mods", C.c_ubyte),
        ("compat_grab_mods", C.c_ubyte), ("lookup_mods", C.c_ubyte),
        ("compat_lookup_mods", C.c_ubyte), ("ptr_buttons", C.c_ushort),
    ]


def desktop_receipt(values, descriptor, expected_helper, observed_us):
    if len(values) != 6:
        raise Failure("desktop ReadState reply arity differs", "INVALID_OBSERVATION")
    outcome, helper, extension, source, state, role = values
    if (any(not isinstance(value, str) or len(value) != 32 or
            any(char not in "0123456789abcdef" for char in value) for value in (helper, extension)) or
            type(outcome) is not int or outcome != 0 or type(role) is not int or role not in (0, 1) or
            any(type(value) is not int or not 0 <= value < 2**64 for value in (source, state))):
        raise Failure("desktop public state is not valid", "FAIL_PRODUCT")
    if (helper != descriptor["helper_instance"] or
            [descriptor["pid"], str(descriptor["start_time"])] != expected_helper):
        raise Failure("desktop helper identity differs", "ABORTED_SAFETY")
    return {"outcome": outcome, "helper_instance": helper, "extension_instance": extension,
            "source_generation": source, "state_generation": state, "current_role": role,
            "observed_us": observed_us, "observation_signature": "(usstty)"}


def verify_desktop_authority(before, after):
    if any(field not in snapshot for snapshot in (before, after) for field in ("desktop", "xkb")):
        raise Failure("desktop parity observation missing", "INVALID_OBSERVATION")
    previous, current = before["desktop"], after["desktop"]
    if any(previous[field] != current[field] for field in ("helper_instance", "extension_instance", "session_bus_owner")):
        raise Failure("desktop observation instance changed", "ABORTED_SAFETY")
    if (current["source_generation"] != previous["source_generation"] or
            current["state_generation"] < previous["state_generation"] or
            (current["current_role"] != previous["current_role"] and
             current["state_generation"] <= previous["state_generation"]) or
            current["outcome"] != 0 or
            any(item.get("observation_signature") != "(usstty)" or
                item["deadline_us"] <= item["observed_us"] for item in (previous, current)) or
            current["observed_us"] < previous["observed_us"] or
            any(after["xkb"][field] for field in ("base_group", "latched_group", "base_mods", "latched_mods", "ptr_buttons")) or
            after["xkb"]["locked_mods"] != before["xkb"]["locked_mods"]):
        raise Failure("GNOME source/XKB parity or released state differs", "FAIL_PRODUCT")


def verify_desktop(before, after, group, *, transitional=False):
    verify_desktop_authority(before, after)
    allowed = {group, before["desktop"]["current_role"]} if transitional else {group}
    if (after["desktop"]["current_role"] not in allowed or after["xkb"]["group"] not in allowed or
            after["xkb"]["locked_group"] not in allowed):
        raise Failure("GNOME source/XKB parity differs", "FAIL_PRODUCT")


def profile_contract(manifest):
    """Select one pinned installed contract, never infer a downgrade from a reply."""
    profile = manifest.get("profile", "modern")
    if profile == "modern":
        return False
    if profile != "exact285-gtk16":
        raise Failure("unsupported installed acceptance profile")
    expected = {"/usr/local/bin/punto-daemon": "edc37fc25bd16d79e8a1252d1e8a9ef012099b271f8c8b6e7f17176d173cd6a6",
                "/usr/local/bin/punto": "07b56664729be21303560f7912028e0c64e056b375573dcfd21d92875e3b4579",
                "/usr/local/bin/punto-tray": "45881738bda0433702bcbe2bc211e07f343f109360a2a75840a43676cddf5db1"}
    package, processes, installed = (manifest.get(name, {}) for name in ("package", "processes", "installed"))
    if (package.get("version") != "2.8.5" or package.get("sha256") !=
            "80c977e4b195f73bdbf89c9e2ba8b1c4162a6c318ea74142fedd6010d222ebd4" or
            processes.get("daemon") != "/usr/local/bin/punto-daemon" or "helper" in processes or
            not {"udevmon", "interception", "daemon", "uinput", "gnome", "ibus"}.issubset(processes) or
            any(installed.get(path) != digest for path, digest in expected.items()) or
            any(manifest.get(flag) for flag in ("features", "terminal_phase", "lifecycle_phase", "desktop_parity", "soak")) or
            any(Path(path).name == "punto-session-bridge" or path == "/usr/bin/punto-daemon" for path in installed)):
        raise Failure("exact285 profile archive, payload, roles or phase admission differs")
    return True


def reject_candidate_writers():
    """Observe actual executable links, including writers with unrelated argv[0]."""
    executables = checked_run(["sudo", "-n", "ps", "-e", "-o", "exe=", "-ww"]).splitlines()
    executables = [path.removesuffix(" (deleted)") for path in executables]
    if "/usr/local/bin/punto-daemon" not in executables or any(
            Path(path).name in ("punto-daemon", "punto-tray", "punto-session-bridge") and
            path not in ("/usr/local/bin/punto-daemon", "/usr/local/bin/punto-tray")
            for path in executables):
        raise Failure("legacy profile has missing daemon or candidate writer", "ABORTED_SAFETY")


def runtime_status(enabled=None, expected_pid=None, *, profile="modern", _deadline=None):
    return shared_runtime_status(enabled, expected_pid, profile=profile, _deadline=_deadline,
                                 failure=Failure, socket_module=socket, clock=time.monotonic)


PIPELINE_ROLES = ("udevmon", "interception", "daemon", "uinput")
STATS_NAMES = ("x11_health analysis_health input_health x11_last_progress_ms analysis_last_progress_ms "
    "input_last_progress_ms analysis_outstanding input_in_flight log_dropped text_mutation enabled "
    "configured_enabled config_pending config_generation config_result analyzed need_switch corrections "
    "word_dispatches pending_words ready_results worker_threads daemon_peers analysis_mode control_plane "
    "queued_tasks avg_queue_us avg_analysis_us avg_macro_us avg_macro_payload_bytes learning_ready "
    "learning_pending learning_failed exclusions learning_generation learning_completed_generation "
    "learning_failed_generation daemon_epoch").split()


def public_stats(expected_pid, *, _deadline=None):
    deadline = time.monotonic() + 1
    if _deadline is not None:
        deadline = min(deadline, _deadline)
    def remaining():
        value = deadline - time.monotonic()
        if value <= 0:
            raise Failure("STATS shared deadline expired", "INVALID_OBSERVATION")
        return value
    remaining()
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(remaining())
        try:
            client.connect("/run/punto.sock")
        except FileNotFoundError as error:
            remaining()
            if error.errno != 2:
                raise
            raise StatsNotPublished("STATS socket not published") from error
        remaining()
        pid, uid, _ = struct.unpack("3i", client.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))
        if (pid, uid) != (expected_pid, 0):
            raise Failure("STATS peer differs from admitted daemon", "ABORTED_SAFETY")
        client.settimeout(remaining())
        client.sendall(b"STATS\n")
        data = bytearray()
        while b"\n" not in data and len(data) < 4096:
            client.settimeout(remaining())
            block = client.recv(4096 - len(data))
            if not block:
                break
            data.extend(block)
    line = data.decode("ascii")
    fields = line.rstrip("\n").split(" ")
    if not line.endswith("\n") or line.count("\n") != 1 or fields[0] != "OK" or len(fields) != len(STATS_NAMES) + 1:
        raise Failure("malformed current public STATS")
    stats = {}
    enums = {"x11_health": ("ready", "degraded", "failed"),
             "analysis_health": ("ready", "degraded", "failed"),
             "input_health": ("ready", "degraded", "failed"), "text_mutation": ("x11",),
             "config_result": ("none", "ok", "error"), "analysis_mode": ("auto", "fixed"),
             "control_plane": ("primary",)}
    booleans = {"enabled", "configured_enabled", "config_pending", "input_in_flight",
                "learning_ready", "learning_pending", "learning_failed"}
    for expected, field in zip(STATS_NAMES, fields[1:]):
        name, separator, value = field.partition("=")
        if name != expected or not separator:
            raise Failure("STATS schema/order differs")
        if name in enums:
            if value not in enums[name]:
                raise Failure("STATS capability/enum differs")
            stats[name] = value
        else:
            if not value.isascii() or not value.isdecimal() or str(int(value)) != value or int(value) > 2**64 - 1:
                raise Failure("STATS integer differs")
            stats[name] = int(value)
            if name in booleans and stats[name] not in (0, 1):
                raise Failure("STATS boolean differs")
    if time.monotonic() >= deadline:
        raise Failure("late STATS cannot authorize input")
    return stats


def durable_learning(stats, epoch, generation, count):
    if (stats["daemon_epoch"] != epoch or not epoch or not generation or
            stats["learning_ready"] != 1 or stats["learning_pending"] or stats["learning_failed"] or
            stats["learning_completed_generation"] < generation or stats["exclusions"] != count):
        raise Failure("learning publication not durably confirmed", "FAIL_PRODUCT")


def successor_identity(before, after):
    if before["pressed"] or after["pressed"] or before["clipboard"] != after["clipboard"]:
        raise Failure("pipeline transition changed clipboard/released keys", "ABORTED_SAFETY")
    if set(before["identity"]) != set(after["identity"]):
        raise Failure("pipeline transition changed role set", "ABORTED_SAFETY")
    for role in before["identity"]:
        changed = before["identity"][role] != after["identity"][role]
        if changed != (role in PIPELINE_ROLES):
            raise Failure("pipeline successor or unrelated identity differs: " + role, "ABORTED_SAFETY")


def lifecycle_fixture_profile(value):
    text = "punto-g13-clipboard-preservation-sentinel"
    encoded = base64.b64encode(text.encode("ascii")).decode()
    expected = {name: [kind, 8, encoded] for name, kind in (
        ("COMPOUND_TEXT", "COMPOUND_TEXT"), ("STRING", "STRING"), ("TEXT", "COMPOUND_TEXT"),
        ("UTF8_STRING", "UTF8_STRING"), ("text/plain", "text/plain"),
        ("text/plain;charset=utf-8", "text/plain;charset=utf-8"))}
    names = value.get("targets", "").splitlines()
    if (value.get("publication_pending") or type(value.get("owner")) is not int or value["owner"] <= 0 or
            type(value.get("publication_generation")) is not int or value["publication_generation"] < 0 or
            value.get("bytes_base64") != encoded or value.get("representations") != expected or
            len(names) != len(set(names)) or not {"TARGETS", "TIMESTAMP"} <= set(names) or
            set(names) - {"TARGETS", "TIMESTAMP", "MULTIPLE", "SAVE_TARGETS"} != set(expected)):
        raise Failure("unsupported lifecycle clipboard fixture", "INVALID_OBSERVATION")
    return text


def verify_lifecycle_fixture(before, after, publication):
    lifecycle_fixture_profile(before["clipboard"])
    lifecycle_fixture_profile(after["clipboard"])
    if (before["pressed"] or any(before.get(field) != after.get(field) for field in
            ("identity", "pressed", "group", "foreground", "terminal_identity", "xkb"))):
        raise Failure("non-clipboard authority changed during fixture publication", "ABORTED_SAFETY")
    if "desktop" in before or "desktop" in after:
        verify_desktop(before, after, before["group"])
    if (any(type(publication.get(field)) is not int or publication[field] <= 0 for field in
            ("owner", "timestamp", "publication_generation", "pid")) or publication["timestamp"] > 0xFFFFFFFF or
            publication["owner"] != after["clipboard"]["owner"] or
            publication["pid"] != before["identity"]["gtk_observer"][0] or
            not isinstance(publication.get("observer_instance_id"), str) or not publication["observer_instance_id"] or
            after["clipboard"]["publication_generation"] <= before["clipboard"]["publication_generation"]):
        raise Failure("fixture publication lacks actual local-owner/history proof", "ABORTED_SAFETY")


def canonical_sound_pair(spec, original):
    import yaml
    if set(spec) != {"on", "off", "on_sha256", "off_sha256"}:
        raise Failure("predeclared canonical config pair missing")
    values = {}
    for mode in ("on", "off"):
        raw = base64.b64decode(spec[mode], validate=True)
        if not raw or len(raw) > 65536 or hashlib.sha256(raw).hexdigest() != spec[mode + "_sha256"]:
            raise Failure("predeclared canonical config hash/size differs")
        parsed = yaml.safe_load(raw)
        if not isinstance(parsed, dict) or not isinstance(parsed.get("sound"), dict) or type(parsed["sound"].get("enabled")) is not bool or parsed["sound"] != {"enabled": mode == "on"}:
            raise Failure("canonical sound boolean/schema differs")
        nonsound = {key: value for key, value in parsed.items() if key != "sound"}
        if nonsound != {key: value for key, value in original.items() if key != "sound"}:
            raise Failure("canonical pair changes nonsound configuration")
        values[mode] = raw
    if values["on"].replace(b"sound:\n  enabled: true", b"sound:\n  enabled: false") != values["off"]:
        raise Failure("canonical pair is not one exact sound delta")
    return values


def phase_capabilities(manifest):
    """Admit requested fixed phase inputs before staging or guest effects."""
    def digest(value):
        return isinstance(value, str) and len(value) == 64 and all(char in "0123456789abcdef" for char in value)
    def absolute(value):
        return isinstance(value, str) and Path(value).is_absolute() and ".." not in Path(value).parts
    if manifest.get("terminal_phase"):
        terminal = manifest.get("terminal")
        if (not isinstance(terminal, dict) or not absolute(manifest.get("python")) or
                any(not absolute(terminal.get(name)) for name in ("client", "server")) or
                any(not digest(terminal.get(name)) for name in ("client_sha256", "server_sha256", "python_sha256"))):
            raise Failure("terminal capability paths/hashes missing or invalid")
    if manifest.get("lifecycle_phase"):
        spec = manifest.get("lifecycle")
        if (not isinstance(spec, dict) or "original_config_sha256" not in spec or
                (spec["original_config_sha256"] is not None and not digest(spec["original_config_sha256"])) or
                not absolute(spec.get("tray")) or not digest(spec.get("tray_sha256")) or
                spec["tray"] != manifest.get("processes", {}).get("tray") or
                spec["tray_sha256"] != manifest.get("installed", {}).get(spec["tray"]) or
                not isinstance(spec.get("tray_bus_name"), str) or not spec["tray_bus_name"] or
                not isinstance(spec.get("tray_menu_path"), str) or not spec["tray_menu_path"].startswith("/")):
            raise Failure("lifecycle capability config/tray declaration missing or invalid")
        audio = spec.get("audio")
        tools = {"/usr/bin/pactl", "/usr/bin/parec", "/usr/bin/paplay"}
        waves = {"/usr/share/punto-switcher/sounds/en_ru.wav", "/usr/share/punto-switcher/sounds/ru_en.wav"}
        if (not isinstance(audio, dict) or not isinstance(audio.get("tools"), dict) or
                not isinstance(audio.get("waves"), dict) or set(audio["tools"]) != tools or set(audio["waves"]) != waves or
                any(not digest(value) for group in ("tools", "waves") for value in audio[group].values())):
            raise Failure("lifecycle capability audio declaration missing or invalid")
        import yaml
        try:
            pair = spec["config_pair"]
            canonical_sound_pair(pair, yaml.safe_load(base64.b64decode(pair["on"], validate=True)))
        except (KeyError, TypeError, ValueError, yaml.YAMLError, Failure) as error:
            raise Failure("lifecycle capability canonical config pair invalid") from error


# Fixed three-object root capability; never accepts a caller-selected store/unit.
LIFECYCLE_FILES = r"""
import base64, hashlib, json, os, re, stat, sys
from pathlib import Path
v = json.load(sys.stdin)
root = Path("/etc/punto") / (".acceptance-" + v["run_id"])
assert re.fullmatch(r"[0-9a-f-]{36}", v["run_id"]) and root.parent == Path("/etc/punto")
cfg = Path(v["config"])
assert cfg == Path("/home/punto/.config/punto/config.yaml")
paths = [Path("/etc/punto/undo_exclusions.txt"), Path("/etc/punto/.undo_exclusions.txt.reset"), cfg]
def meta(path):
    if not path.exists() and not path.is_symlink():
        return {"absent": True}
    s = path.lstat()
    assert stat.S_ISREG(s.st_mode) and s.st_nlink == 1 and s.st_size <= 65536
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NOATIME)
    try:
        raw = os.read(fd, 65537)
        assert len(raw) == s.st_size and os.fstat(fd).st_ino == s.st_ino
    finally:
        os.close(fd)
    return {"sha256": hashlib.sha256(raw).hexdigest(), "uid": s.st_uid, "gid": s.st_gid,
            "mode": stat.S_IMODE(s.st_mode), "type": "regular", "mtime_ns": s.st_mtime_ns,
            "atime_ns": s.st_atime_ns, "xattrs": {k: hashlib.sha256(os.getxattr(path, k)).hexdigest()
                                               for k in os.listxattr(path)}}
def same_original(actual, expected):
    return {k: value for k, value in actual.items() if k != "atime_ns"} == \
           {k: value for k, value in expected.items() if k != "atime_ns"}
def admit_original(path, expected, exact=False):
    actual = meta(path)
    assert actual == expected if exact else same_original(actual, expected), "original metadata differs"
    if expected.get("absent"):
        return None
    s = path.lstat()
    return (s.st_dev, s.st_ino)
def restore_timestamps(path, expected, identity):
    if expected.get("absent"):
        assert meta(path) == expected
        return
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NOATIME)
    try:
        s = os.fstat(fd)
        assert (s.st_dev, s.st_ino) == identity and stat.S_ISREG(s.st_mode) and s.st_nlink == 1
        raw = os.read(fd, 65537)
        actual = {"sha256": hashlib.sha256(raw).hexdigest(), "uid": s.st_uid, "gid": s.st_gid,
                  "mode": stat.S_IMODE(s.st_mode), "type": "regular", "mtime_ns": s.st_mtime_ns,
                  "xattrs": {k: hashlib.sha256(os.getxattr(fd, k)).hexdigest() for k in os.listxattr(fd)}}
        assert len(raw) == s.st_size <= 65536 and same_original(actual, expected)
        current = path.lstat()
        assert (current.st_dev, current.st_ino) == identity
        os.utime(fd, ns=(expected["atime_ns"], expected["mtime_ns"]))
        after = os.fstat(fd)
        assert (after.st_dev, after.st_ino) == identity and after.st_mtime_ns == expected["mtime_ns"]
        current = path.lstat()
        assert (current.st_dev, current.st_ino) == identity and same_original(meta(path), expected)
    finally:
        os.close(fd)
def admit_scratch(path, i):
    current = meta(path)
    if current.get("absent"):
        return
    assert (current["uid"] == (v["uid"] if i == 2 else 0) and
            current["gid"] == (v["gid"] if i == 2 else 0) and current["mode"] == 0o600)
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NOATIME)
    try:
        raw = os.read(fd, 65537)
    finally:
        os.close(fd)
    if i == 0:
        assert raw in (b"", b"ghbdtn\n", b"hello\n", b"ghbdtn\nhello\n")
    elif i == 2:
        assert current["sha256"] in v["config_hashes"]
    else:
        assert re.fullmatch(rb"PUNTO_RESET_V1 [0-9a-f-]{36} [1-9][0-9]*\n", raw)
for path in paths:
    for parent in path.parents:
        assert not parent.is_symlink() and (not parent.exists() or parent.is_dir())
if v["action"] == "inspect":
    source = cfg if cfg.exists() else Path("/etc/punto/config.yaml")
    fd = os.open(source, os.O_RDONLY | os.O_NOFOLLOW | os.O_NOATIME)
    try:
        raw = os.read(fd, 65537)
        assert 0 < len(raw) <= 65536
    finally:
        os.close(fd)
    print(json.dumps({"objects": [meta(p) for p in paths], "config": base64.b64encode(raw).decode()}))
    sys.exit(0)
# Any writer outside the stopped unit also blocks replacement/restoration.
for item in Path("/proc").glob("[0-9]*/exe"):
    try:
        assert str(item.resolve()) not in v["writers"], "live Punto writer"
    except (FileNotFoundError, ProcessLookupError):
        continue
receipt = root / "originals.json"
if v["action"] == "restore" and (not root.exists() or not receipt.exists()):
    assert isinstance(v["original"], list) and len(v["original"]) == len(paths)
    identities = [admit_original(p, record) for p, record in zip(paths, v["original"])]
    for path, record, identity in zip(paths, v["original"], identities):
        restore_timestamps(path, record, identity)
    print(json.dumps({"restored": True, "objects": v["original"], "backup_retained": None}))
    sys.exit(0)
if v["action"] == "isolate":
    records = v["original"]
    assert isinstance(records, list) and len(records) == len(paths)
    identities = [admit_original(p, record) for p, record in zip(paths, records)]
    for i, record in enumerate(records):
        if not record.get("absent"):
            assert record["uid"] == (v["uid"] if i == 2 else 0)
            assert record["gid"] == (v["gid"] if i == 2 else 0)
            assert (record["mode"] & 0o022) == 0 if i == 2 else record["mode"] == 0o600
    root.mkdir(mode=0o700)
    receipt.write_text(json.dumps({"objects": records, "config_parent_created": not cfg.parent.exists()}))
    os.chmod(receipt, 0o600)
    fd = os.open(receipt, os.O_RDONLY)
    os.fsync(fd)
    os.close(fd)
    for i, path in enumerate(paths):
        if not records[i].get("absent"):
            os.rename(path, root / str(i))
            restore_timestamps(root / str(i), records[i], identities[i])
            assert meta(root / str(i)) == records[i]
    if not cfg.parent.exists():
        assert cfg.parent.parent.is_dir()
        cfg.parent.mkdir(mode=0o700)
        os.chown(cfg.parent, v["uid"], v["gid"])
    assert cfg.parent.is_dir()
    for path, raw, uid, gid in ((paths[0], b"", 0, 0), (cfg, base64.b64decode(v["off"]), v["uid"], v["gid"])):
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        try:
            os.fchown(fd, uid, gid)
            assert os.write(fd, raw) == len(raw)
            os.fsync(fd)
        finally:
            os.close(fd)
    print(json.dumps({"objects": records, "isolated": [meta(p) for p in paths]}))
elif v["action"] == "restore":
    assert root.lstat().st_uid == 0 and stat.S_IMODE(root.lstat().st_mode) == 0o700
    journal = json.loads(receipt.read_text())
    records = journal["objects"]
    assert isinstance(records, list) and len(records) == len(paths)
    identities = []
    # Admit every original and scratch object before moving or normalizing any.
    for i, path in enumerate(paths):
        backup = root / str(i)
        if backup.exists() or backup.is_symlink():
            identities.append(admit_original(backup, records[i], exact=True))
            admit_scratch(path, i)
        elif records[i].get("absent"):
            identities.append(None)
            admit_scratch(path, i)
        else:
            identities.append(admit_original(path, records[i]))
    for i, path in enumerate(paths):
        backup = root / str(i)
        if backup.exists():
            assert meta(backup) == records[i]
            current = meta(path)
            if not current.get("absent"):
                admit_scratch(path, i)
                os.rename(path, root / ("scratch-" + str(i)))
            os.rename(backup, path)
        elif records[i].get("absent"):
            if path.exists():
                admit_scratch(path, i)
                os.rename(path, root / ("scratch-" + str(i)))
        else:
            assert same_original(meta(path), records[i]), "original neither in place nor in backup"
        restore_timestamps(path, records[i], identities[i])
        assert same_original(meta(path), records[i])
    if journal["config_parent_created"]:
        assert cfg.parent.lstat().st_uid == v["uid"] and stat.S_IMODE(cfg.parent.lstat().st_mode) == 0o700
        cfg.parent.rmdir()
    print(json.dumps({"restored": True, "objects": records, "backup_retained": str(root)}))
else:
    raise AssertionError("fixed file action required")
"""


def validate_audio_player(player, daemon, spec):
    if (player["parent"] != daemon[0] or player["uid"] != os.getuid() or
            not player["start"].isdecimal() or int(player["start"]) <= 0 or len(player["argv"]) != 2 or
            player["argv"][0] != "/usr/bin/paplay" or player["argv"][1] not in spec["waves"] or
            player.get("elf_sha256") != spec["tools"]["/usr/bin/paplay"] or
            player.get("wave_sha256") != spec["waves"].get(player["argv"][1])):
        raise Failure("paplay is not the attributed daemon correction child", "ABORTED_SAFETY")


def validate_audio_window(window, players, streams, sequence):
    if sequence <= window["sequence"]:
        raise Failure("OFF/ON audio observer did not advance", "ABORTED_SAFETY")
    if window["enabled"]:
        expected = "/usr/share/punto-switcher/sounds/" + ("en_ru.wav" if window["direction"] == 0 else "ru_en.wav")
        if (len(players) != 1 or players[0]["argv"][1] != expected or len(streams) != 1 or
                streams[0]["player"] != players[0] or streams[0]["frames"] <= 0 or not streams[0]["nonzero"]):
            raise Failure("exclusive attributed correction PCM missing", "FAIL_PRODUCT")
    elif players or streams:
        raise Failure("sound OFF dispatched attributed correction audio", "FAIL_PRODUCT")

class Guest:
    """Projection only: existing UI observers plus actual guest X11/proc state."""
    def __init__(self, manifest):
        profile_contract(manifest)
        self.m, self.gtk, self.chrome, self.browser, self.playwright = manifest, None, None, None, None
        self.clipboard_reader, self.deadline_ns = None, None
        self.surface = "gtk"
        self.learning_phase = self.audio = None
        self.terminal = None
        self.terminal_observing = False
        self.x = C.CDLL("libX11.so.6")
        self.x.XOpenDisplay.restype = C.c_void_p
        self.display = self.x.XOpenDisplay(None)
        if not self.display:
            raise Failure("ordinary guest X11 display unavailable")
        for name in ("XInternAtom", "XGetSelectionOwner", "XStringToKeysym"):
            getattr(self.x, name).restype = C.c_ulong
        self.x.XKeysymToKeycode.restype = C.c_ubyte
        self.x.XInternAtom.argtypes = [C.c_void_p, C.c_char_p, C.c_int]
        self.x.XGetSelectionOwner.argtypes = [C.c_void_p, C.c_ulong]
        self.x.XStringToKeysym.argtypes = [C.c_char_p]
        self.x.XKeysymToKeycode.argtypes = [C.c_void_p, C.c_ulong]
        self.x.XQueryKeymap.argtypes = [C.c_void_p, C.c_void_p]
        self.x.XkbGetState.argtypes = [C.c_void_p, C.c_uint, C.c_void_p]
        self.x.XGetInputFocus.argtypes = [C.c_void_p, C.c_void_p, C.c_void_p]
        self.x.XQueryTree.argtypes = [C.c_void_p, C.c_ulong, C.c_void_p, C.c_void_p, C.c_void_p, C.c_void_p]
        self.x.XDefaultRootWindow.argtypes, self.x.XDefaultRootWindow.restype = [C.c_void_p], C.c_ulong
        self.x.XGetWindowProperty.argtypes = [C.c_void_p, C.c_ulong, C.c_ulong, C.c_long, C.c_long, C.c_int,
                                            C.c_ulong, C.c_void_p, C.c_void_p, C.c_void_p, C.c_void_p, C.c_void_p]
        self.x.XFree.argtypes = [C.c_void_p]


    def lifecycle_file(self, action):
        phase = self.learning_phase
        value = {"action": action, "run_id": self.m["run_id"], "config": phase["config"],
                 "uid": os.getuid(), "gid": os.getgid(),
                 "writers": [str(Path(self.m["processes"][r]).resolve()) for r in PIPELINE_ROLES],
                 "off": self.m["lifecycle"]["config_pair"]["off"],
                 "config_hashes": [hashlib.sha256(raw).hexdigest() for raw in phase["pair"].values()],
                 "original": phase.get("original")}
        return json.loads(checked_run(["sudo", "-n", self.m["python"], "-c", LIFECYCLE_FILES],
                                     input=json.dumps(value).encode(), timeout=3))

    def lifecycle_stop(self):
        checked_run(["sudo", "-n", "systemctl", "stop", "udevmon.service"], timeout=5)
        deadline = time.monotonic() + 2
        old = self.learning_phase.get("restoration_pipeline", self.learning_phase["current"]["identity"])
        while any(Path("/proc", str(old[r][0]), "stat").exists() and
                  Path("/proc", str(old[r][0]), "stat").read_text().rsplit(")", 1)[1].split()[19] == old[r][1]
                  for r in PIPELINE_ROLES):
            if time.monotonic() >= deadline:
                raise Failure("old pipeline writers remain alive", "ABORTED_SAFETY")
            time.sleep(.02)
        self.learning_phase["old_dead"] = True

    def lifecycle_start_pipeline(self, restoration_only=False):
        if not self.learning_phase.pop("old_dead", False):
            raise Failure("successor lacks old-writer death proof", "ABORTED_SAFETY")
        previous = self.learning_phase.get("restoration_pipeline", self.learning_phase["current"]["identity"])
        checked_run(["sudo", "-n", "systemctl", "start", "udevmon.service"], timeout=5)
        deadline = time.monotonic() + 3
        readiness_identity, published = None, False
        construction_identity = {role: value for role, value in previous.items() if role not in PIPELINE_ROLES}
        while True:
            if time.monotonic() >= deadline:
                raise Failure("successor pipeline readiness deadline", "ABORTED_SAFETY")
            try:
                processes, identity = (self.pipeline_admission(construction_identity=construction_identity)
                                       if readiness_identity is None else self.pipeline_admission())
            except ProcessesNotPublished as pending:
                if readiness_identity is not None:
                    raise
                construction_identity.update(pending.identity)
                time.sleep(.02)
                continue
            if not restoration_only:
                after = self.system_observation(processes, identity, prepare_pending=True)
                identity = after["identity"]
            if readiness_identity is None:
                readiness_identity = identity.copy()
            elif identity != readiness_identity:
                raise Failure("successor changed during IPC readiness", "ABORTED_SAFETY")
            try:
                stats = public_stats(identity["daemon"][0], _deadline=deadline)
            except StatsNotPublished:
                if published:
                    raise
                if time.monotonic() >= deadline:
                    raise Failure("successor pipeline readiness deadline", "ABORTED_SAFETY")
                time.sleep(.02)
                continue
            published = True
            if time.monotonic() >= deadline:
                raise Failure("successor pipeline readiness deadline", "ABORTED_SAFETY")
            failed = [name for name in ("x11_health", "analysis_health", "input_health") if stats[name] == "failed"]
            failed += [name for name in ("learning_failed", "config_result")
                       if stats[name] == (1 if name == "learning_failed" else "error")]
            if failed:
                raise Failure("successor terminal STATS: " + ", ".join(f"{name}={stats[name]}" for name in failed), "ABORTED_SAFETY")
            if (all(stats[name] == "ready" for name in ("x11_health", "analysis_health", "input_health")) and
                    stats["learning_ready"] and not stats["learning_pending"] and not stats["learning_failed"] and
                    stats["learning_completed_generation"] >= stats["learning_generation"] and not stats["config_pending"]):
                processes, identity = self.pipeline_admission()
                if not restoration_only:
                    after = self.system_observation(processes, identity, prepare_pending=True)
                    identity = after["identity"]
                if identity != readiness_identity:
                    raise Failure("successor changed after STATS readiness", "ABORTED_SAFETY")
                if time.monotonic() >= deadline:
                    raise Failure("successor pipeline readiness deadline", "ABORTED_SAFETY")
                break
            time.sleep(.02)
        if restoration_only:
            if set(identity) != set(self.m["processes"]) or not set(identity) <= set(previous):
                raise Failure("restoration pipeline role set differs", "ABORTED_SAFETY")
            for role in identity:
                if (identity[role] != previous[role]) != (role in PIPELINE_ROLES):
                    raise Failure("restoration pipeline or unrelated identity differs: " + role, "ABORTED_SAFETY")
            self.lifecycle_tray_identity()
            return {"restoration_only": True, "pipeline": identity, "stats": stats, "old_dead": True,
                    "_readiness_deadline_ns": int(deadline * 1e9)}
        successor_identity(self.learning_phase["current"], after)
        self.lifecycle_tray_identity()
        self.learning_phase["current"] = after
        return {"before": self.learning_phase["transition_from"], "after": after, "stats": stats, "old_dead": True}

    def lifecycle_tray_identity(self):
        spec = self.m["lifecycle"]
        owner = self.terminal_bus("GetNameOwner", spec["tray_bus_name"])
        pid = self.terminal_bus("GetConnectionUnixProcessID", owner)
        identity = self.terminal_process(pid, spec["tray"])
        check_files({spec["tray"]: spec["tray_sha256"], f"/proc/{pid}/exe": spec["tray_sha256"]})
        value = {"owner": owner, "identity": identity}
        if self.learning_phase.get("tray") and value != self.learning_phase["tray"]:
            raise Failure("tray changed across lifecycle", "ABORTED_SAFETY")
        return value

    def lifecycle_fixture(self, original, reassert=False):
        deadline = getattr(self, "deadline_ns", None) or time.monotonic_ns() + 5_000_000_000
        if type(reassert) is not bool:
            raise Failure("fixture reassert authorization is not boolean", "INVALID_OBSERVATION")
        phase = self.learning_phase
        if reassert:
            if (not phase or not phase.get("armed") or not getattr(self, "lifecycle_fixture_started", False) or
                    not getattr(self, "lifecycle_fixture_observer", None)):
                raise Failure("fixture reassert outside owned lifecycle", "INVALID_OBSERVATION")
        elif getattr(self, "lifecycle_fixture_started", False) or phase:
            raise Failure("lifecycle fixture already started or phase active", "INVALID_OBSERVATION")
        self.deadline_ns = deadline
        lifecycle_fixture_profile(original)
        before = self.system(prepare_pending=True)
        if before["clipboard"] != original or before["pressed"]:
            raise Failure("lifecycle fixture preflight drift or held keys", "INVALID_OBSERVATION")
        if reassert and before["identity"] != phase["current"]["identity"]:
            raise Failure("fixture reassert process identity changed", "ABORTED_SAFETY")
        def target(action, **value):
            if self.surface == "gtk":
                return self.target(action, **value)
            return self.gtk.call({"action": action, **value}, deadline_ns=deadline)
        observer = target("identity")
        if reassert and observer["observer_instance_id"] != self.lifecycle_fixture_observer:
            raise Failure("fixture reassert GTK observer changed", "ABORTED_SAFETY")
        if time.monotonic_ns() >= deadline:
            raise Failure("lifecycle fixture preflight exceeded shared deadline", "INVALID_OBSERVATION")
        self.lifecycle_fixture_started = True
        try:
            publication = target("lifecycle-fixture", clipboard=original,
                observer_instance_id=observer["observer_instance_id"], deadline_ns=deadline,
                **({"reassert": True} if reassert else {}))["publication"]
            after = self.system(prepare_pending=True)
            if time.monotonic_ns() >= deadline or publication["observer_instance_id"] != observer["observer_instance_id"]:
                raise Failure("lifecycle fixture response late or observer changed", "INVALID_OBSERVATION")
            verify_lifecycle_fixture(before, after, publication)
            self.lifecycle_fixture_observer = observer["observer_instance_id"]
            if reassert:
                phase["current"] = {**phase["current"], "clipboard": after["clipboard"]}
            return {"before": before, "after": after, "publication": publication}
        except Exception as error:
            failure = Failure("lifecycle fixture publication uncertain", "ABORTED_SAFETY")
            failure.add_note(f"Fixture failure [{getattr(error, 'kind', 'unclassified')}]: {str(error)[:512]}")
            for note in getattr(error, "__notes__", [])[:3]:
                failure.add_note(note[:1024])
            raise failure from error

    def lifecycle_begin(self):
        import yaml
        if self.learning_phase is not None or not self.m.get("lifecycle_phase"):
            raise Failure("learning transaction not admitted")
        if not callable(getattr(getattr(self, "clipboard_reader", None), "call", None)):
            raise Failure("approved independent clipboard boundary missing")
        self.learning_phase = {"config": "/home/punto/.config/punto/config.yaml", "pair": {}, "isolated": False}
        phase = self.learning_phase
        if self.m["session"].get("HOME") != "/home/punto" or self.m["session"].get("XDG_CONFIG_HOME", "/home/punto/.config") != "/home/punto/.config":
            self.learning_phase = None
            raise Failure("tray/daemon config roots do not agree")
        try:
            original = self.lifecycle_file("inspect")
            if original["objects"][2].get("sha256") != self.m["lifecycle"]["original_config_sha256"]:
                raise Failure("canonical fixture original file/absence differs")
            effective = yaml.safe_load(base64.b64decode(original.pop("config"), validate=True))
            phase["pair"] = canonical_sound_pair(self.m["lifecycle"]["config_pair"], effective)
            phase["original"] = original["objects"]
            phase["current"] = self.system(prepare_pending=True)
            phase["transition_from"] = phase["current"]
            phase["original_enabled"] = runtime_status(expected_pid=phase["current"]["identity"]["daemon"][0])
            phase["tray"] = self.lifecycle_tray_identity()
            phase["boot"] = Path("/proc/sys/kernel/random/boot_id").read_text().strip()
            phase["armed"] = True  # Cleanup owns partial isolation before the first mutation.
            self.lifecycle_stop()
            backup = self.lifecycle_file("isolate")
            if backup["objects"] != phase["original"] or backup["isolated"][0]["sha256"] != hashlib.sha256(b"").hexdigest():
                raise Failure("backup/original changed or store not empty", "ABORTED_SAFETY")
            phase["isolated"] = True
            result = self.lifecycle_start_pipeline()
            if self.require_owned_learning_phase()["store"] != "" or result["stats"]["exclusions"] != 0:
                raise Failure("fresh successor did not load the isolated empty store", "ABORTED_SAFETY")
            result["backup"] = backup
            return result
        except Exception as primary:
            if phase.get("armed"):
                try:
                    self.lifecycle_restore()
                except Exception as cleanup:
                    failure = Failure("learning setup restoration failed", "ABORTED_SAFETY")
                    failure.add_note(f"Primary setup failure: {type(primary).__name__} "
                                     f"[{getattr(primary, 'kind', 'unclassified')}]: {str(primary)[:512]}")
                    failure.add_note(f"Setup cleanup failure: {type(cleanup).__name__} "
                                     f"[{getattr(cleanup, 'kind', 'unclassified')}]: {str(cleanup)[:512]}")
                    for note in getattr(cleanup, "__notes__", [])[:4]:
                        failure.add_note(note[:1024])
                    raise failure from primary
            else:
                self.learning_phase = None
            raise

    def require_owned_learning_phase(self):
        phase = getattr(self, "learning_phase", None)
        if not phase or not phase.get("armed"):
            raise Failure("automatic inverse requires the owned learning transaction")
        record = json.loads(checked_run(["sudo", "-n", self.m["python"], "-c",
            "import pathlib,json,os,stat; p=pathlib.Path('/etc/punto/undo_exclusions.txt'); s=p.lstat(); "
            "assert stat.S_ISREG(s.st_mode) and s.st_uid==s.st_gid==0 and stat.S_IMODE(s.st_mode)==0o600 and s.st_nlink==1; "
            "b=p.read_bytes(); assert len(b)<=65536; print(json.dumps({'store':b.decode('ascii')}))"], timeout=1))
        if record["store"] not in ("", "ghbdtn\n", "hello\n", "ghbdtn\nhello\n"):
            raise Failure("unowned learned word/store change", "ABORTED_SAFETY")
        return record

    def lifecycle_restart(self):
        self.require_owned_learning_phase()
        observed = self.system()
        if observed["clipboard"] != self.learning_phase["current"]["clipboard"]:
            raise Failure("retained clipboard changed before qualification restart", "ABORTED_SAFETY")
        self.learning_phase["transition_from"] = observed
        self.learning_phase["current"] = self.learning_phase["transition_from"]
        self.lifecycle_stop()
        return self.lifecycle_start_pipeline()

    def lifecycle_stats(self):
        self.require_owned_learning_phase()
        now = self.system()
        if Path("/proc/sys/kernel/random/boot_id").read_text().strip() != self.learning_phase["boot"]:
            raise Failure("kernel boot identity changed", "ABORTED_SAFETY")
        if now["identity"] != self.learning_phase["current"]["identity"] or now["clipboard"] != self.learning_phase["current"]["clipboard"] or now["pressed"]:
            raise Failure("learning peer/context drift", "ABORTED_SAFETY")
        return {"stats": public_stats(now["identity"]["daemon"][0]),
                **self.require_owned_learning_phase()}

    def lifecycle_reset(self):
        before = self.lifecycle_stats()
        output = checked_run(["/usr/bin/punto", "reset-learning"], timeout=5)
        after = self.lifecycle_stats()
        if output != "OK EXCLUSIONS CLEARED\n" or after["stats"]["learning_generation"] <= before["stats"]["learning_generation"]:
            raise Failure("documented reset command/generation not confirmed", "FAIL_PRODUCT")
        durable_learning(after["stats"], before["stats"]["daemon_epoch"], after["stats"]["learning_generation"], 0)
        marker = checked_run(["sudo", "-n", "cat", "/etc/punto/.undo_exclusions.txt.reset"], timeout=1)
        fields = marker.rstrip("\n").split(" ")
        if (after["store"] != "" or len(marker.encode()) > 128 or len(fields) != 3 or fields[:2] != ["PUNTO_RESET_V1", self.learning_phase["boot"]] or
                not fields[2].isdecimal() or int(fields[2]) <= 0):
            raise Failure("reset store/current-boot marker differs", "FAIL_PRODUCT")
        return {"before_stats": before["stats"], **after, "cli": output, "reset_marker": marker,
                "ticket_proof": "documented_cli_internal_epoch_ticket_and_public_post_cli_generation"}

    def lifecycle_restore(self):
        phase = self.learning_phase
        if not phase or not phase.get("armed"):
            self.learning_phase = None
            return {}
        try:
            audio_error = None
            try:
                self.audio_close()
            except Exception as error:
                audio_error = error
            sound_error = None
            try:
                self.lifecycle_sound_close()
            except Exception as error:
                sound_error = error
            phase["transition_from"] = phase["current"]
            self.lifecycle_stop()  # All accepted writers die before any restore.
            if sound_error:
                raise sound_error  # A live dialog can still overwrite the restored user config.
            restored = self.lifecycle_file("restore")
            if not restored.get("restored") or restored["objects"] != phase["original"]:
                raise Failure("exact owned originals not restored")
            phase["isolated"] = False
            terminal = getattr(getattr(self, "clipboard_reader", None), "terminal_failure", None)
            result = self.lifecycle_start_pipeline(restoration_only=True)
            observation_deadline = result.pop("_readiness_deadline_ns")
            identity = result["pipeline"]
            runtime_status(phase["original_enabled"], identity["daemon"][0])
            acknowledged = public_stats(identity["daemon"][0])
            if acknowledged["config_pending"] or acknowledged["config_result"] == "error" or acknowledged["enabled"] != phase["original_enabled"]:
                raise Failure("restored config/status not acknowledged")
            observation_error = terminal if isinstance(terminal, Failure) else None
            if observation_error is None:
                processes, admitted = self.pipeline_admission()  # Core/file faults are never observer fallback.
                if admitted != identity:
                    raise Failure("restoration pipeline changed after acknowledgement", "ABORTED_SAFETY")
                previous_deadline = getattr(self, "deadline_ns", None)
                self.deadline_ns = min(previous_deadline or observation_deadline, observation_deadline)
                try:
                    after = self.system_observation(processes, admitted, prepare_pending=True)
                    if time.monotonic_ns() >= self.deadline_ns:
                        raise Failure("restoration observation exceeded original readiness deadline", "INVALID_OBSERVATION")
                    successor_identity(phase["current"], after)
                except Failure as error:
                    observation_error = error
                finally:
                    self.deadline_ns = previous_deadline
                if observation_error is None:
                    result = {"before": phase["transition_from"], "after": after,
                              "stats": result["stats"], "old_dead": True}
                    phase["current"] = after
            if observation_error is not None:
                result["observation_failure"] = {"kind": observation_error.kind, "error": str(observation_error)[:1024],
                    "notes": [note[:1024] for note in getattr(observation_error, "__notes__", [])[:8]]}
            result["restoration"] = restored
            result["original_enabled"] = phase["original_enabled"]
            result["acknowledged"] = acknowledged
            self.learning_phase = None
            if audio_error:
                raise audio_error
            return result
        except Exception as error:
            failure = Failure("owned learning/config restoration incomplete; backup retained", "ABORTED_SAFETY")
            failure.add_note(f"Restoration failure: {type(error).__name__} "
                             f"[{getattr(error, 'kind', 'unclassified')}]: {str(error)[:512]}")
            raise failure from error

    def lifecycle_sound_save(self, enabled):
        if type(enabled) is not bool:
            raise Failure("sound flag must be boolean")
        self.require_owned_learning_phase()
        self.lifecycle_tray_identity()
        import gi
        gi.require_version("Gio", "2.0")
        gi.require_version("Atspi", "2.0")
        from gi.repository import Atspi, Gio, GLib
        phase, spec = self.learning_phase, self.m["lifecycle"]
        before = self.lifecycle_stats()["stats"]
        cfg = Path(phase["config"])
        initial = cfg.read_bytes()
        current = initial == phase["pair"]["on"]
        if initial != phase["pair"]["on" if current else "off"] or current == enabled:
            raise Failure("sound Save initial owned bytes/flag differ")
        deadline = time.monotonic() + 3
        cancellable = Gio.Cancellable.new()
        timer = threading.Timer(3, cancellable.cancel)
        timer.daemon = True
        timer.start()
        try:
            bus = Gio.bus_get_sync(Gio.BusType.SESSION, cancellable)
        finally:
            timer.cancel()
        def dbus(method, params, reply):
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise Failure("public sound menu deadline")
            value = bus.call_sync(phase["tray"]["owner"], spec["tray_menu_path"], "com.canonical.dbusmenu",
                                  method, params, GLib.VariantType.new(reply), Gio.DBusCallFlags.NONE,
                                  max(1, int(remaining * 1000)), cancellable)
            if time.monotonic() >= deadline:
                raise Failure("late sound menu response")
            return value.unpack()
        revision, layout = dbus("GetLayout", GLib.Variant("(iias)", (0, -1, ["label"])), "(u(ia{sv}av))")
        pending, sound = [layout], []
        while pending:
            item_id, props, children = pending.pop()
            if props.get("label") == "Звук исправлений...":
                sound.append(item_id)
            if len(pending) + len(children) > 256:
                raise Failure("public sound menu fanout exceeds bound")
            pending.extend(child.unpack() if hasattr(child, "unpack") else child for child in children)
        if len(sound) != 1:
            raise Failure("public sound menu label not unique")
        phase["dialog_pending"] = True
        dbus("Event", GLib.Variant("(isvu)", (sound[0], "clicked", GLib.Variant("i", 0), 0)), "()")
        def native(method, *args):
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise Failure("sound native API deadline")
            ms = max(1, min(100, int(remaining * 1000)))
            Atspi.set_timeout(ms, ms)
            if time.monotonic() >= deadline:
                raise Failure("sound native timeout deadline")
            value = method(*args)
            if time.monotonic() >= deadline:
                raise Failure("late sound native API")
            return value
        discovered = 0
        def children(node):
            nonlocal discovered
            count = native(node.get_child_count)
            if count < 0 or count > 256 - discovered:
                raise Failure("sound native enumeration exceeds bound")
            discovered += count
            return [native(node.get_child_at_index, i) for i in range(count)]
        apps = [a for a in children(native(Atspi.get_desktop, 0))
                if native(a.get_process_id) == phase["tray"]["identity"][0]]
        nodes, matches = apps[:], {}
        while nodes:
            node = nodes.pop()
            name = native(node.get_name)
            if name in ("Звук исправлений Punto Switcher", "Звук при смене раскладки", "Сохранить"):
                if name in matches:
                    raise Failure("duplicate sound native control")
                matches[name] = node
            nodes.extend(children(node))
        if len(apps) != 1 or len(matches) != 3:
            raise Failure("ordinary same-PID Sound dialog controls absent")
        title = "Звук исправлений Punto Switcher"
        while True:
            self.lifecycle_tray_identity()
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise Failure("sound dialog publication deadline", "INVALID_OBSERVATION")
            visible = subprocess.run(["xdotool", "search", "--all", "--onlyvisible", "--pid",
                str(phase["tray"]["identity"][0]), "--name", "^" + title + "$"], capture_output=True,
                timeout=remaining, check=False)
            if time.monotonic() >= deadline:
                raise Failure("late sound dialog publication", "INVALID_OBSERVATION")
            if visible.returncode == 1 and not visible.stdout and not visible.stderr:
                time.sleep(min(.02, max(0, deadline - time.monotonic())))
                continue
            xids = visible.stdout.decode().split()
            if (visible.returncode != 0 or visible.stderr or len(xids) != 1 or
                    not xids[0].isascii() or not xids[0].isdecimal()):
                raise Failure("sound dialog publication ambiguous", "ABORTED_SAFETY")
            break
        if checked_run(["xdotool", "getwindowpid", xids[0]], timeout=max(.001, deadline - time.monotonic())).strip() != str(phase["tray"]["identity"][0]):
            raise Failure("sound dialog XID/PID mismatch", "ABORTED_SAFETY")
        checkbox, save = matches["Звук при смене раскладки"], matches["Сохранить"]
        def state(node, flag):
            return native(native(node.get_state_set).contains, flag)
        def act(node):
            interface = native(node.get_action_iface)
            if native(interface.get_n_actions) != 1 or not native(interface.do_action, 0):
                raise Failure("sound action unavailable")
        if state(checkbox, Atspi.StateType.CHECKED) != current or state(save, Atspi.StateType.SENSITIVE):
            raise Failure("sound initial checkbox/Save state differs", "FAIL_PRODUCT")
        for expected in (enabled, current, enabled):
            act(checkbox)
            if state(checkbox, Atspi.StateType.CHECKED) != expected or state(save, Atspi.StateType.SENSITIVE) != (expected != current):
                raise Failure("sound Save dirty-state sensitivity differs", "FAIL_PRODUCT")
            now = public_stats(phase["current"]["identity"]["daemon"][0], _deadline=deadline)
            if now["daemon_epoch"] != before["daemon_epoch"]:
                raise Failure("daemon changed during sound Save", "ABORTED_SAFETY")
            if cfg.read_bytes() != initial or now["config_generation"] != before["config_generation"]:
                raise Failure("unsaved sound edit changed config", "FAIL_PRODUCT")
        act(save)
        expected = phase["pair"]["on" if enabled else "off"]
        while True:
            now = public_stats(phase["current"]["identity"]["daemon"][0], _deadline=deadline)
            if now["daemon_epoch"] != before["daemon_epoch"]:
                raise Failure("daemon changed during sound Save", "ABORTED_SAFETY")
            if (cfg.read_bytes() == expected and now["config_generation"] > before["config_generation"] and
                    not now["config_pending"] and now["config_result"] == "ok"):
                break
            if time.monotonic() >= deadline:
                raise Failure("sound Save bytes/public commit not confirmed", "FAIL_PRODUCT")
            time.sleep(.02)
        if time.monotonic() >= deadline:
            raise Failure("late sound Save acknowledgement cannot pass")
        visible = subprocess.run(["xdotool", "search", "--all", "--onlyvisible", "--pid", str(phase["tray"]["identity"][0]),
                                  "--name", "^" + title + "$"], capture_output=True,
                                 timeout=max(.001, deadline - time.monotonic()), check=False)
        if visible.returncode != 1 or visible.stdout or time.monotonic() >= deadline:
            raise Failure("saved Sound dialog did not close", "FAIL_PRODUCT")
        phase["dialog_pending"] = False
        self.lifecycle_tray_identity()
        return {"sound": enabled, "config_sha256": hashlib.sha256(expected).hexdigest(),
                "before_stats": before, "stats": now, "menu_id": sound[0], "menu_revision": revision, "dialog_xid": int(xids[0])}

    def lifecycle_sound_close(self):
        phase = self.learning_phase
        if not phase or not phase.get("dialog_pending"):
            return
        self.lifecycle_tray_identity()
        pid = phase["tray"]["identity"][0]
        title = "^Звук исправлений Punto Switcher$"
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            remaining = max(.001, deadline - time.monotonic())
            visible = subprocess.run(["xdotool", "search", "--all", "--onlyvisible", "--pid", str(pid), "--name", title],
                                     capture_output=True, timeout=remaining, check=False)
            if visible.returncode == 1 and not visible.stdout:
                phase["dialog_pending"] = False
                return
            xids = visible.stdout.decode().split()
            if visible.returncode != 0 or len(xids) != 1 or not xids[0].isascii() or not xids[0].isdecimal():
                raise Failure("owned Sound dialog cleanup ambiguous", "ABORTED_SAFETY")
            if checked_run(["xdotool", "getwindowpid", xids[0]], timeout=remaining).strip() != str(pid):
                raise Failure("refusing foreign dialog cleanup", "ABORTED_SAFETY")
            checked_run(["xdotool", "windowclose", xids[0]], timeout=max(.001, deadline - time.monotonic()))
            time.sleep(.01)
        raise Failure("owned Sound dialog cleanup deadline", "ABORTED_SAFETY")


    def audio_children(self):
        return [str(pid) for pid, (_, parent, _) in process_snapshot(Path).items()
                if parent == self.audio["daemon"][0]]

    def audio_start(self):
        self.require_owned_learning_phase()
        if self.audio is not None:
            raise Failure("audio observer already active")
        spec = self.m["lifecycle"]["audio"]
        if (set(spec["tools"]) != {"/usr/bin/pactl", "/usr/bin/parec", "/usr/bin/paplay"} or
                set(spec["waves"]) != {"/usr/share/punto-switcher/sounds/en_ru.wav",
                                      "/usr/share/punto-switcher/sounds/ru_en.wav"}):
            raise Failure("fixed installed audio paths not declared")
        check_files(spec["tools"])
        check_files(spec["waves"])
        now = self.system()
        server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        server.settimeout(1)
        try:
            server.connect("/run/user/" + str(os.getuid()) + "/pulse/native")
            pid, uid, _ = struct.unpack("3i", server.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))
        finally:
            server.close()
        if uid != os.getuid():
            raise Failure("ordinary Pulse server owner differs", "ABORTED_SAFETY")
        pulse_start = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()[19]
        child = subprocess.Popen([sys.executable, __file__, "--exec", "/usr/bin/pactl", "subscribe"], stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE, start_new_session=True)
        item = self.audio = {"child": child, "server": [pid, pulse_start], "daemon": now["identity"]["daemon"],
                            "stop": threading.Event(), "ready": threading.Event(), "events": [], "error": None,
                            "players": {}, "streams": {}, "buffer": b"", "window": None, "sequence": 0}
        item["observer"] = [child.pid, Path(f"/proc/{child.pid}/stat").read_text().rsplit(")", 1)[1].split()[19]]
        os.set_blocking(child.stdout.fileno(), False)
        os.set_blocking(child.stderr.fileno(), False)
        def observe():
            try:
                deadline = time.monotonic() + 3
                while True:
                    clients = json.loads(checked_run(["/usr/bin/pactl", "-f", "json", "list", "clients"], timeout=1))
                    if item["stop"].is_set():
                        return
                    if len(clients) > 256:
                        raise Failure("Pulse client budget exceeded")
                    if any(str(c.get("properties", {}).get("application.process.id")) == str(child.pid) for c in clients):
                        break
                    if child.poll() is not None or time.monotonic() >= deadline:
                        raise Failure("Pulse subscription not ready before input")
                    time.sleep(.01)
                check_files({f"/proc/{child.pid}/exe": spec["tools"]["/usr/bin/pactl"]})
                item["ready"].set()
                while not item["stop"].is_set():
                    if (child.poll() is not None or Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()[19] != pulse_start or
                            Path(f"/proc/{item['daemon'][0]}/stat").read_text().rsplit(")", 1)[1].split()[19] != item["daemon"][1]):
                        raise Failure("continuous audio server/daemon observer changed")
                    if select.select([child.stderr], [], [], 0)[0] and os.read(child.stderr.fileno(), 1024):
                        raise Failure("Pulse subscription error/ambiguous coverage")
                    if select.select([child.stdout], [], [], .01)[0]:
                        item["buffer"] += os.read(child.stdout.fileno(), 4096)
                        if len(item["buffer"]) > 65536:
                            raise Failure("Pulse subscription overflow")
                        lines = item["buffer"].split(b"\n")
                        item["buffer"] = lines.pop()
                        for line in lines:
                            if b" on sink-input #" in line and b"'new'" in line:
                                item["events"].append(int(line.rsplit(b"#", 1)[1]))
                    children = self.audio_children()
                    if len(children) > 16 or len(item["events"]) > 256:
                        raise Failure("audio child/event budget exceeded")
                    for value in children:
                        path = Path("/proc") / value
                        try:
                            args = (path / "cmdline").read_bytes().split(b"\0")[:-1]
                            if not args or args[0] != b"/usr/bin/paplay":
                                continue
                            raw = (path / "stat").read_text().rsplit(")", 1)[1].split()
                            player = {"pid": int(value), "start": raw[19], "parent": int(raw[1]),
                                      "uid": path.stat().st_uid, "argv": [a.decode() for a in args]}
                            key = (player["pid"], player["start"])
                            if key not in item["players"]:
                                player["elf_sha256"] = hashlib.sha256((path / "exe").read_bytes()).hexdigest()
                                if len(player["argv"]) == 2:
                                    player["wave_sha256"] = hashlib.sha256(Path(player["argv"][1]).read_bytes()).hexdigest()
                                validate_audio_player(player, item["daemon"], spec)
                                if (path / "stat").read_text().rsplit(")", 1)[1].split()[19] != player["start"]:
                                    raise Failure("paplay identity changed while attributing")
                                item["players"][key] = player
                                player["window"] = item["window"]
                        except FileNotFoundError:
                            raise Failure("paplay vanished before exact attribution")
                    if item["events"]:
                        inputs = json.loads(checked_run(["/usr/bin/pactl", "-f", "json", "list", "sink-inputs"], timeout=1))
                        if item["stop"].is_set():
                            return
                        if len(inputs) > 256:
                            raise Failure("Pulse input budget exceeded")
                        while item["events"]:
                            index = item["events"].pop(0)
                            matches = [s for s in inputs if int(s["index"]) == index]
                            if len(matches) != 1:
                                raise Failure("new Pulse stream vanished before attribution")
                            stream = matches[0]
                            if index in item["streams"]:
                                raise Failure("Pulse stream index was reused", "ABORTED_SAFETY")
                            value = stream.get("properties", {}).get("application.process.id", "")
                            if not str(value).isascii() or not str(value).isdecimal():
                                raise Failure("Pulse stream lacks PID attribution")
                            players = [p for p in item["players"].values() if p["pid"] == int(value)]
                            if not players:
                                # A live foreign process is not Punto audio.
                                if int(value) in [int(c) for c in children] or not Path("/proc", str(value), "stat").exists():
                                    raise Failure("ambiguous/late daemon child stream")
                                continue
                            if len(players) != 1 or players[0]["window"] is None:
                                raise Failure("Punto audio outside admitted correction window")
                            player = players[0]
                            if Path(f"/proc/{player['pid']}/stat").read_text().rsplit(")", 1)[1].split()[19] != player["start"]:
                                raise Failure("stream player PID was reused")
                            recorder = subprocess.Popen([sys.executable, __file__, "--exec", "/usr/bin/parec", f"--monitor-stream={index}",
                                "--client-name=punto-acceptance-" + self.m["run_id"],
                                "--stream-name=punto-correction-proof", "--raw", "--format=s16le",
                                "--rate=48000", "--channels=2", "--latency-msec=5"],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
                            os.set_blocking(recorder.stdout.fileno(), False)
                            item["streams"][index] = {"player": player, "window": player["window"],
                                "recorder": recorder, "frames": 0, "nonzero": False, "pcm_verified": False}
                    for stream in item["streams"].values():
                        recorder = stream["recorder"]
                        if recorder.poll() is None and select.select([recorder.stdout], [], [], 0)[0]:
                            if not stream["pcm_verified"]:
                                check_files({f"/proc/{recorder.pid}/exe": spec["tools"]["/usr/bin/parec"]})
                                stream["recorder_identity"] = [recorder.pid,
                                    Path(f"/proc/{recorder.pid}/stat").read_text().rsplit(")", 1)[1].split()[19]]
                                stream["pcm_verified"] = True
                            data = os.read(recorder.stdout.fileno(), 65536)
                            stream["frames"] += len(data) // 4
                            stream["nonzero"] |= any(data)
                    item["sequence"] += 1
            except Exception as error:
                item["error"] = str(error)
                item["ready"].set()
        item["thread"] = threading.Thread(target=observe, daemon=True)
        item["thread"].start()
        if not item["ready"].wait(3) or item["error"]:
            raise Failure("audio observer could not establish continuous ready coverage")
        return self.audio_health()

    def audio_health(self):
        item = self.audio
        if (not item or item["error"] or not item["ready"].is_set() or not item["thread"].is_alive() or
                item["child"].poll() is not None or
                Path(f"/proc/{item['observer'][0]}/stat").read_text().rsplit(")", 1)[1].split()[19] != item["observer"][1]):
            raise Failure("audio observation unhealthy/ambiguous", "ABORTED_SAFETY")
        return {"observer_identity": item["observer"], "pulse_server": item["server"],
                "sequence": item["sequence"]}

    def audio_window(self, action, direction=None, enabled=None):
        item = self.audio
        before = self.audio_health()
        if action == "begin":
            if item["window"] is not None or type(enabled) is not bool or direction not in (0, 1):
                raise Failure("audio correction window not admitted")
            item["window"] = {"id": str(uuid.uuid4()), "direction": direction, "enabled": enabled,
                              "sequence": before["sequence"], "started_ns": time.monotonic_ns()}
            return item["window"].copy()
        if action != "finish" or item["window"] is None:
            raise Failure("audio window action differs")
        window = item["window"]
        deadline = time.monotonic() + 2.5
        while time.monotonic() < deadline:
            self.audio_health()
            time.sleep(.02)
        players = [p for p in item["players"].values() if p["window"] == window]
        streams = [s for s in item["streams"].values() if s["window"] == window]
        for player in players:
            path = Path("/proc", str(player["pid"]), "stat")
            if path.exists() and path.read_text().rsplit(")", 1)[1].split()[19] == player["start"]:
                raise Failure("prior correction player did not retire before the next sound phase")
        validate_audio_window(window, players, streams, self.audio_health()["sequence"])
        result = {"window": window, "players": players,
                  "streams": [{"frames": s["frames"], "nonzero": s["nonzero"], "player": s["player"],
                               "recorder_identity": s.get("recorder_identity")}
                              for s in streams], **self.audio_health()}
        item["window"] = None
        return result

    def audio_close(self):
        item = self.audio
        if item is None:
            return
        item["stop"].set()
        item["thread"].join(timeout=2)
        errors = []
        for child in [item["child"]] + [s["recorder"] for s in item["streams"].values()]:
            try:
                if child.poll() is None:
                    child.terminate()
                try:
                    child.wait(timeout=1)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait(timeout=1)
                child.stdout.close()
                child.stderr.close()
            except Exception as error:
                errors.append(error)
        self.audio = None
        if item["thread"].is_alive() or errors:
            raise Failure("owned audio observer cleanup incomplete", "ABORTED_SAFETY")


    def setup(self):
        legacy = profile_contract(self.m)
        ordinary_environment(os.environ)
        self.gtk = Lines([sys.executable, "-u", __file__, "--gtk"])
        if not self.gtk.read().get("ready"):
            raise Failure("GTK observer did not become ready")
        self.clipboard_reader = Lines([sys.executable, "-u", __file__, "--clipboard-reader"])
        if not self.clipboard_reader.read().get("ready"):
            raise Failure("independent clipboard reader did not become ready")
        if legacy:
            return
        from playwright.sync_api import sync_playwright
        self.playwright = sync_playwright().start()
        profile = Path(self.m["staged_dir"]) / "chrome-profile"
        self.chrome = subprocess.Popen([sys.executable, __file__, "--exec", self.m["chromium"], "--no-first-run", "--no-default-browser-check",
            "--password-store=basic",
            f"--user-data-dir={profile}", "--remote-debugging-address=127.0.0.1", "--remote-debugging-port=0",
            "--app=" + (Path(__file__).parent / "chromium_target.html").as_uri() + "?run=" + self.m["run_id"]],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
        deadline = time.monotonic() + 15
        while not (profile / "DevToolsActivePort").is_file():
            if self.chrome.poll() is not None or time.monotonic() >= deadline:
                raise Failure("ordinary Chromium/CDP startup unavailable")
            time.sleep(0.05)
        port = int((profile / "DevToolsActivePort").read_text().splitlines()[0])
        self.browser = self.playwright.chromium.connect_over_cdp(f"http://127.0.0.1:{port}", timeout=5000)
        expected = (Path(__file__).parent / "chromium_target.html").as_uri() + "?run=" + self.m["run_id"]
        pages = [page for context in self.browser.contexts for page in context.pages if page.url == expected]
        if len(pages) != 1:
            raise Failure("exact owned Chromium URL/run nonce not uniquely present")
        self.page = pages[0]
        self.page.set_default_timeout(5000)
        self.page.wait_for_function("typeof window.puntoProbe === 'object'", timeout=5000)

    def page_identity(self):
        expected = (Path(__file__).parent / "chromium_target.html").as_uri() + "?run=" + self.m["run_id"]
        if self.page.url != expected:
            raise Failure("owned Chromium URL/run nonce differs", "ABORTED_SAFETY")


    @staticmethod
    def terminal_process(pid, executable):
        return process_identity(pid, executable, failure=Failure, path=Path)


    def terminal_pin(self, role, pid, executable, expected=None):
        identity = self.terminal_process(pid, executable)
        if expected is not None and identity != expected:
            raise Failure("terminal scratch PID/start no longer exists", "ABORTED_SAFETY")
        fd = os.pidfd_open(pid)
        try:
            if self.terminal_process(pid, executable) != identity or (role == "server" and
                    self.terminal_bus("GetNameOwner", self.terminal["app_id"]) != self.terminal["owner"]):
                raise Failure("terminal ownership changed while pinning", "ABORTED_SAFETY")
        except Exception:
            os.close(fd)
            raise
        self.terminal["identity"][role] = identity
        self.terminal["fds"][role] = fd

    @staticmethod
    def terminal_bus(method, name):
        import ast
        result = checked_run(["gdbus", "call", "--session", "--dest", "org.freedesktop.DBus",
            "--object-path", "/org/freedesktop/DBus", "--method", "org.freedesktop.DBus." + method,
            name], timeout=1).strip()
        if result in ("(true,)", "(false,)"):
            return result == "(true,)"
        return ast.literal_eval(result.replace("uint32 ", ""))[0]

    def terminal_validate(self):
        import ast
        import termios
        item = self.terminal
        if item is None:
            raise Failure("terminal target has not been admitted")
        config = self.m["terminal"]
        check_files({config["client"]: config["client_sha256"], config["server"]: config["server_sha256"],
                     self.m["python"]: config["python_sha256"]})
        if (self.terminal_bus("GetNameOwner", item["app_id"]) != item["owner"] or
                self.terminal_bus("GetConnectionUnixProcessID", item["owner"]) != item["identity"]["server"][0] or
                self.terminal_bus("GetConnectionUnixUser", item["owner"]) != os.getuid()):
            raise Failure("private terminal bus owner changed", "ABORTED_SAFETY")
        for role, executable in (("server", config["server"]), ("reader", self.m["python"]), ("monitor", self.m["python"])):
            if self.terminal_process(item["identity"][role][0], executable) != item["identity"][role]:
                raise Failure("terminal PID/start identity changed", "ABORTED_SAFETY")
        xid = item["identity"]["xid"]
        wm_class = ast.literal_eval(checked_run(["xprop", "-id", str(xid), "WM_CLASS"]).partition(" = ")[2].strip())
        if (list(wm_class) != item["identity"]["wm_class"] or
                int(checked_run(["xdotool", "getwindowpid", str(xid)])) != item["identity"]["server"][0] or
                checked_run(["xdotool", "getwindowname", str(xid)]).strip() != item["title"]):
            raise Failure("terminal XID/class/title ownership changed", "ABORTED_SAFETY")
        ready = terminal_ready(json.loads((item["directory"] / "ready.json").read_text()),
                               item["identity"]["nonce"], item["directory"])
        if (ready["pid"] != item["identity"]["reader"][0] or ready["monitor"] != item["identity"]["monitor"][0] or
                os.getpgid(ready["pid"]) != ready["pgrp"] or os.getsid(ready["pid"]) != ready["sid"] or
                os.getpgid(ready["monitor"]) != ready["monitor_pgrp"] or
                os.getsid(ready["monitor"]) != ready["monitor_sid"] or
                int((Path("/proc") / str(ready["pid"]) / "stat").read_text().rsplit(")", 1)[1].split()[1]) != ready["monitor"] or
                (item["directory"] / "signals").exists()):
            raise Failure("reader changed or terminal control signal escaped", "FAIL_PRODUCT")
        reader_path = Path("/proc") / str(ready["pid"])
        if (reader_path / "fd/0").resolve() != Path(ready["tty"]):
            raise Failure("reader canonical PTY descriptor changed", "ABORTED_SAFETY")
        fd = os.open(reader_path / "fd/0", os.O_RDONLY | os.O_NOCTTY | os.O_CLOEXEC)
        try:
            attributes = termios.tcgetattr(fd)
            required = termios.ICANON | termios.ISIG | termios.ECHO
            raw = (reader_path / "stat").read_text().rsplit(")", 1)[1].split()
            expected = [str(ready["pgrp"]), str(ready["sid"]), str(os.fstat(fd).st_rdev),
                        str(ready["pgrp"]), item["identity"]["reader"][1]]
            if (os.ttyname(fd) != ready["tty"] or raw[2:6] + [raw[19]] != expected or
                    attributes[3] & required != required or attributes[6][termios.VSUSP] != b"\x1a"):
                raise Failure("live canonical PTY foreground/control policy changed", "ABORTED_SAFETY")
        finally:
            os.close(fd)
        raw = (reader_path / "stat").read_text().rsplit(")", 1)[1].split()
        if (raw[2:6] + [raw[19]] != expected or (reader_path / "fd/0").resolve() != Path(ready["tty"]) or
                self.terminal_process(ready["pid"], self.m["python"]) != item["identity"]["reader"]):
            raise Failure("reader changed during live PTY observation", "ABORTED_SAFETY")
        return dict(item["identity"])

    def terminal_window(self, server, deadline):
        """Wait only for an absent window while the private server stays pinned."""
        item = self.terminal
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0 or item["launcher"].poll() is not None:
                raise Failure("private terminal window publication deadline")
            if (self.terminal_bus("GetNameOwner", item["app_id"]) != item["owner"] or
                    self.terminal_bus("GetConnectionUnixProcessID", item["owner"]) != server or
                    self.terminal_bus("GetConnectionUnixUser", item["owner"]) != os.getuid() or
                    self.terminal_process(server, self.m["terminal"]["server"]) != item["identity"]["server"]):
                raise Failure("private terminal window owner changed", "ABORTED_SAFETY")
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise Failure("private terminal window publication deadline")
            try:
                result = subprocess.run(["xdotool", "search", "--all", "--onlyvisible", "--pid", str(server),
                    "--name", "^" + item["title"] + "$"], capture_output=True, timeout=min(1, remaining), check=False)
            except (OSError, subprocess.TimeoutExpired) as error:
                raise Failure("bounded terminal window search failed") from error
            windows = result.stdout.split()
            if result.returncode == 1 and not windows and not result.stderr:
                time.sleep(.02)
                continue
            if result.returncode or result.stderr or time.monotonic() >= deadline:
                raise Failure("private terminal window search failed")
            if len(windows) != 1 or not windows[0].isdigit() or int(windows[0]) <= 0:
                raise Failure("private terminal nonce XID is not unique")
            return int(windows[0])

    def terminal_start(self, nonce, automatic=False):
        import ast
        from probe import ObservationProtocol
        if self.terminal is not None or not self.m.get("terminal_phase") or not nonce.isalnum():
            raise Failure("terminal phase/nonce is not admitted")
        if not callable(getattr(getattr(self, "clipboard_reader", None), "call", None)):
            raise Failure("terminal phase requires the approved independent clipboard reader")
        config = self.m["terminal"]
        check_files({config["client"]: config["client_sha256"], config["server"]: config["server_sha256"],
                     self.m["python"]: config["python_sha256"]})
        if automatic:
            admission = getattr(self, "require_owned_learning_phase", None)
            if not callable(admission):
                raise Failure("automatic inverse requires the admitted isolated learning store")
            admission()
        app_id = "org.punto.Acceptance.r" + nonce
        if self.terminal_bus("NameHasOwner", app_id):
            raise Failure("private terminal application ID already has an owner")
        self.terminal_observing = True
        before = self.system(prepare_pending=True)
        if before["pressed"]:
            raise Failure("held key before terminal target admission", "ABORTED_SAFETY")
        directory = Path(self.m["staged_dir"]) / ("terminal-" + nonce)
        directory.mkdir(mode=0o700)
        item = {"app_id": app_id, "directory": directory, "title": "Punto acceptance " + nonce,
                "identity": {"nonce": nonce}, "fds": {}, "protocol": ObservationProtocol("terminal")}
        self.terminal = item
        item["launcher"] = subprocess.Popen([sys.executable, __file__, "--exec", config["client"],
            "--app-id=" + app_id, "--wait", "--window", "--title=" + item["title"], "--",
            self.m["python"], "-u", __file__, "--terminal-reader", str(directory), nonce],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
        deadline = time.monotonic() + 5
        while not self.terminal_bus("NameHasOwner", app_id):
            if item["launcher"].poll() is not None or time.monotonic() >= deadline:
                raise Failure("private ordinary gnome-terminal did not acquire its ID")
            time.sleep(0.02)
        item["owner"] = self.terminal_bus("GetNameOwner", app_id)
        if self.terminal_bus("GetConnectionUnixUser", item["owner"]) != os.getuid():
            raise Failure("private terminal owner UID differs")
        server = self.terminal_bus("GetConnectionUnixProcessID", item["owner"])
        self.terminal_pin("server", server, config["server"])
        while not (directory / "ready.json").is_file():
            if item["launcher"].poll() is not None or time.monotonic() >= deadline:
                raise Failure("canonical reader readiness deadline")
            time.sleep(0.02)
        ready = terminal_ready(json.loads((directory / "ready.json").read_text()), nonce, directory)
        reader = ready["pid"]
        self.terminal_pin("monitor", ready["monitor"], self.m["python"], [ready["monitor"], ready["monitor_start"]])
        self.terminal_pin("reader", reader, self.m["python"], [reader, ready["start"]])
        parent = reader
        for _ in range(30):
            parent = int((Path("/proc") / str(parent) / "stat").read_text().rsplit(")", 1)[1].split()[1])
            if parent in (1, server):
                break
        if parent != server:
            raise Failure("canonical reader is not descended from the private server")
        xid = self.terminal_window(server, deadline)
        wm_class = list(ast.literal_eval(checked_run(["xprop", "-id", str(xid), "WM_CLASS"]).partition(" = ")[2].strip()))
        if not any(part.lower() in ("gnome-terminal", "gnome-terminal-server", "org.gnome.terminal") for part in wm_class):
            raise Failure("ordinary terminal WM_CLASS is not recognized; no class rewriting")
        item["identity"].update(xid=xid, wm_class=wm_class)
        self.terminal_validate()
        # Ownership precedes presentation and every physical key admission.
        checked_run(["xdotool", "windowactivate", "--sync", str(xid)], timeout=1)
        self.surface = "terminal"
        after = self.system()
        if any(before[key] != after[key] for key in ("identity", "clipboard")) or not after["foreground"] or after["pressed"]:
            raise Failure("terminal admission changed core/clipboard authority", "ABORTED_SAFETY")
        self.terminal_text()  # Ordinary native provider must expose the owned canonical prompt.
        return {**after, "terminal_admission": dict(item["identity"])}

    def terminal_text(self):
        deadline = time.monotonic() + 1
        import gi
        gi.require_version("Atspi", "2.0")
        from gi.repository import Atspi
        def read(method, *args):
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise Failure("terminal native text read exceeded its bound")
            timeout_ms = max(1, min(250, int(remaining * 1000)))
            Atspi.set_timeout(timeout_ms, timeout_ms)
            if time.monotonic() >= deadline:
                raise Failure("terminal native timeout setup exceeded its bound")
            value = method(*args)
            if time.monotonic() >= deadline:
                raise Failure("terminal native text read exceeded its bound")
            return value
        discovered = 0
        def children(node):
            nonlocal discovered
            count = read(node.get_child_count)
            if count < 0 or count > 256 - discovered:
                raise Failure("terminal native enumeration/fanout exceeded its bound")
            discovered += count
            return [read(node.get_child_at_index, i) for i in range(count)]
        item = self.terminal
        apps = [app for app in children(read(Atspi.get_desktop, 0))
                if read(app.get_process_id) == item["identity"]["server"][0]]
        if len(apps) != 1:
            raise Failure("ordinary terminal native provider is not uniquely available")
        nodes, matches = [apps[0]], []
        while nodes:
            node = nodes.pop()
            if read(node.get_role) == Atspi.Role.TERMINAL:
                matches.append(node)
            nodes.extend(children(node))
        if len(matches) != 1:
            raise Failure("owned terminal native text surface is ambiguous")
        native = read(matches[0].get_text_iface)
        count = read(Atspi.Text.get_character_count, native)
        if count < 0 or count > 4096:
            raise Failure("owned terminal native text exceeds its scratch bound")
        text = read(Atspi.Text.get_text, native, 0, count)
        prompt = "PUNTO-" + item["identity"]["nonce"] + ">"
        if prompt not in text or "PUNTO-" + item["identity"]["nonce"] + "\n" not in text:
            raise Failure("terminal visible history/prompt nonce changed", "FAIL_PRODUCT")
        base = text.rindex(prompt) + len(prompt)
        line = text[base:].split("\n", 1)[0]
        caret = read(Atspi.Text.get_caret_offset, native) - base
        if read(Atspi.Text.get_n_selections, native) != 0:
            raise Failure("terminal unexpectedly selected foreign text", "FAIL_PRODUCT")
        return {"text": line, "selection": [caret, caret], "following_text": "PUNTO-" + item["identity"]["nonce"]}

    def terminal_target(self, action, **value):
        self.terminal_validate()
        protocol = self.terminal["protocol"]
        if action == "identity":
            return {"observer_id": protocol.observer_id, "observer_instance_id": protocol.observer_instance_id,
                    "observer_ns": time.monotonic_ns()}
        if action == "arm":
            protocol.arm(value["run_id"], value["case_id"], value["attempt_id"], value["clock_mapping"])
            return {}
        if action == "snapshot":
            state = self.terminal_text()
            if state != self.terminal.get("last_state"):
                protocol.record_state_change()
                self.terminal["last_state"] = state
            result = protocol.snapshot(state)
            submitted = self.terminal["directory"] / "submitted"
            result["submitted_base64"] = base64.b64encode(submitted.read_bytes()).decode() if submitted.exists() else None
            return result
        if action == "complete":
            protocol.complete()
            return {}
        raise Failure("unsupported terminal action")

    def terminal_close(self, force=False):
        item = self.terminal
        if item is None:
            return
        launcher = item.get("launcher")
        if launcher and launcher.poll() is None:
            launcher.kill()  # This unreaped child cannot have a reused PID.
        if "server" not in item["fds"] and self.terminal_bus("NameHasOwner", item["app_id"]):
            item["owner"] = self.terminal_bus("GetNameOwner", item["app_id"])
            if self.terminal_bus("GetConnectionUnixUser", item["owner"]) != os.getuid():
                raise Failure("unconfirmed private terminal startup cleanup", "ABORTED_SAFETY")
            self.terminal_pin("server", self.terminal_bus("GetConnectionUnixProcessID", item["owner"]), self.m["terminal"]["server"])
        for fd in item["fds"].values():
            try:
                signal.pidfd_send_signal(fd, signal.SIGKILL if force else signal.SIGTERM)
            except ProcessLookupError:
                pass
        if not force:
            deadline = time.monotonic() + 2
            for fd in item["fds"].values():
                if not select.select([fd], [], [], max(0, deadline - time.monotonic()))[0]:
                    signal.pidfd_send_signal(fd, signal.SIGKILL)
            for fd in item["fds"].values():
                if not select.select([fd], [], [], 1)[0]:
                    raise Failure("owned terminal cleanup deadline", "ABORTED_SAFETY")
        launcher = item.get("launcher")
        if launcher and not force:
            try:
                launcher.wait(timeout=1)
            except subprocess.TimeoutExpired:
                launcher.kill()
                launcher.wait(timeout=1)
        self.terminal = None
        for fd in item["fds"].values():
            os.close(fd)
        self.surface = "chromium"

    def target(self, action, **value):
        if self.surface == "terminal":
            return self.terminal_target(action, **value)
        if self.surface == "gtk":
            return self.gtk.call({"action": action, **value}, deadline_ns=self.deadline_ns)
        self.page_identity()
        expression = {"identity": "({...puntoProbe.observerIdentity(),observer_ns:Math.round(performance.now()*1e6)})",
                      "prepare": "puntoProbe.prepare(v.target,v.text)",
                      "arm": "puntoProbe.arm(v.run_id,v.case_id,v.attempt_id,v.clock_mapping)",
                      "snapshot": "puntoProbe.snapshot(v.target)", "complete": "puntoProbe.complete()"}[action]
        return self.page.evaluate("v => " + expression, value)

    def active_window(self):
        actual, size, count, after = C.c_ulong(), C.c_int(), C.c_ulong(), C.c_ulong()
        data = C.POINTER(C.c_ubyte)()
        try:
            status = self.x.XGetWindowProperty(self.display, self.x.XDefaultRootWindow(self.display),
                self.x.XInternAtom(self.display, b"_NET_ACTIVE_WINDOW", 0), 0, 1, 0, 33,
                C.byref(actual), C.byref(size), C.byref(count), C.byref(after), C.byref(data))
            if status:
                raise Failure("active-client property read failed")
            if (actual.value, size.value, count.value, after.value) == (0, 0, 0, 0):
                return None
            if (actual.value, size.value, count.value, after.value) != (33, 32, 1, 0) or not data:
                raise Failure("malformed active-client WINDOW property", "INVALID_OBSERVATION")
            return C.cast(data, C.POINTER(C.c_ulong))[0] or None
        finally:
            if data:
                self.x.XFree(data)

    def pipeline_admission(self, construction_identity=None):
        legacy = profile_contract(self.m)
        if legacy:
            reject_candidate_writers()
        installed = self.m["installed"]
        phase = getattr(self, "learning_phase", None)
        if phase and phase.get("armed") and phase.get("isolated"):
            check_files({name: digest for name, digest in installed.items() if name != phase["config"]})
            if self.deadline_ns is not None and time.monotonic_ns() >= self.deadline_ns:
                raise Failure("owned config inspection deadline expired", "INVALID_OBSERVATION")
            self.require_owned_learning_phase()
            inspected = self.lifecycle_file("inspect")
            if self.deadline_ns is not None and time.monotonic_ns() >= self.deadline_ns:
                raise Failure("owned config inspection completed late", "INVALID_OBSERVATION")
            config = inspected["objects"][2]
            pair = phase["pair"]
            raw = base64.b64decode(inspected["config"], validate=True)
            if (set(pair) != {"on", "off"} or raw not in pair.values() or
                    config.get("sha256") not in {hashlib.sha256(value).hexdigest() for value in pair.values()} or
                    (config.get("type"), config.get("uid"), config.get("gid"), config.get("mode")) !=
                    ("regular", os.getuid(), os.getgid(), 0o600)):
                raise Failure("isolated config is not the owned canonical pair", "ABORTED_SAFETY")
        else:
            check_files(installed)
        status = checked_run(["dpkg-query", "-W", "-f=${db:Status-Abbrev} ${Version}", "punto-switcher"])
        if status.strip() != "ii  " + self.m["package"]["version"]:
            raise Failure("installed package state/version differs")
        processes = process_snapshot(Path)
        construction = ({"not_yet_published_roles": PIPELINE_ROLES, "expected": construction_identity}
                        if construction_identity is not None else {})
        identity = process_identities(processes, self.m["processes"], self.m["installed"],
                                      checked_run, failure=Failure, path=Path, **construction)
        pipeline_facts(processes, identity, self.m["pipeline"]["evdev"], checked_run,
                       failure=Failure)
        if phase and phase.get("armed"):
            # Preserve writer death targets even if a subsequent observer gate fails.
            phase["restoration_pipeline"] = identity.copy()
        return processes, identity

    def system(self, prepare_pending=False):
        processes, identity = self.pipeline_admission()
        return self.system_observation(processes, identity, prepare_pending)

    def system_observation(self, processes, identity, prepare_pending=False):
        identity = identity.copy()
        legacy = profile_contract(self.m)
        observers = [("gtk_observer", self.gtk.child.pid), ("clipboard_reader", self.clipboard_reader.child.pid)]
        if not legacy:
            observers.insert(1, ("chromium_observer", self.chrome.pid))
        for role, pid in observers:
            if pid not in processes:
                raise Failure(f"owned {role} process disappeared")
            identity[role] = [pid, processes[pid][2]]
        keymap, xkb = C.create_string_buffer(32), XkbState()
        if not self.x.XQueryKeymap(self.display, keymap) or self.x.XkbGetState(self.display, 0x100, C.byref(xkb)):
            raise Failure("actual X11 keyboard state unavailable")
        pressed = []
        for name in ("Pause", "Shift_L", "Shift_R", "Control_L", "Control_R", "Alt_L", "Alt_R", "Super_L", "Super_R"):
            code = self.x.XKeysymToKeycode(self.display, self.x.XStringToKeysym(name.encode()))
            if not code:
                raise Failure(f"X11 keycode missing: {name}")
            if keymap.raw[code // 8] & (1 << (code % 8)):
                pressed.append(name)
        deadline_ns = self.deadline_ns or time.monotonic_ns() + 5_000_000_000
        clipboard = self.clipboard_reader.call({"deadline_ns": deadline_ns}, deadline_ns=deadline_ns)["clipboard"]
        if self.m.get("features") or self.terminal_observing or self.learning_phase:
            if any(keymap.raw):
                pressed.append("physical-key-down")
        terminal_identity = self.terminal_validate() if self.surface == "terminal" else None
        expected_pid = (terminal_identity["server"][0] if terminal_identity else
                        self.gtk.child.pid if self.surface == "gtk" else self.chrome.pid)
        foreground = True
        windows = []
        for _ in range(2):
            active = self.active_window()
            if active is None:
                if not prepare_pending:
                    raise Failure("active owned client unavailable")
                foreground = False
                break
            windows.append(active)
            if terminal_identity:
                foreground &= active == terminal_identity["xid"]
            foreground &= int(checked_run(["xdotool", "getwindowpid", str(active)]).strip()) == expected_pid
            focus, revert = C.c_ulong(), C.c_int()
            self.x.XGetInputFocus(self.display, C.byref(focus), C.byref(revert))
            for _ in range(30):
                if focus.value in (0, 1, active):
                    break
                root, parent, children, count = C.c_ulong(), C.c_ulong(), C.c_void_p(), C.c_uint()
                if not self.x.XQueryTree(self.display, focus.value, C.byref(root), C.byref(parent), C.byref(children), C.byref(count)):
                    raise Failure("input-focus ancestry unavailable")
                if children.value:
                    self.x.XFree(children)
                focus = parent
            foreground &= focus.value == active
        foreground &= len(windows) == 2 and windows[0] == windows[1]
        if self.surface == "chromium":
            self.page_identity()
        result = {"identity": identity, "pressed": pressed, "group": xkb.group, "clipboard": clipboard, "foreground": foreground, **({"terminal_identity": terminal_identity} if terminal_identity else {})}
        if self.m.get("desktop_parity"):
            result["desktop"] = self.desktop(identity["helper"])
            result["xkb"] = {name: getattr(xkb, name) for name, _ in XkbState._fields_}
        return result

    def desktop(self, expected_helper):
        if profile_contract(self.m):
            raise Failure("desktop observation is outside exact285 profile")
        from gi.repository import Gio, GLib
        deadline_us = time.monotonic_ns() // 1000 + 100_000
        if self.deadline_ns is not None:
            deadline_us = min(deadline_us, self.deadline_ns // 1000)
        def remaining():
            value = deadline_us - time.monotonic_ns() // 1000
            if value <= 0:
                raise Failure("desktop observation exceeded its deadline", "INVALID_OBSERVATION")
            return value
        cancellable = Gio.Cancellable()
        timer = threading.Timer(remaining() / 1_000_000, cancellable.cancel)
        timer.start()
        primary = None
        stage = "session bus acquisition"
        try:
            directory = Path(os.environ["XDG_RUNTIME_DIR"]) / "punto"
            descriptor_path = directory / "session-bridge.json"
            if directory.is_symlink() or descriptor_path.is_symlink():
                raise Failure("desktop descriptor path is unsafe", "ABORTED_SAFETY")
            directory_stat, descriptor_stat = directory.stat(), descriptor_path.stat()
            if (directory_stat.st_uid != os.getuid() or directory_stat.st_mode & 0o777 != 0o700 or
                    not stat.S_ISREG(descriptor_stat.st_mode) or descriptor_stat.st_uid != os.getuid() or descriptor_stat.st_mode & 0o777 != 0o600 or
                    descriptor_stat.st_nlink != 1 or descriptor_stat.st_size > 4096):
                raise Failure("desktop descriptor metadata differs", "ABORTED_SAFETY")
            descriptor_bytes = descriptor_path.read_bytes()
            descriptor = json.loads(descriptor_bytes)
            helper = descriptor.get("helper_instance")
            if (descriptor.get("protocol_version") != 1 or not isinstance(helper, str) or len(helper) != 32 or
                    any(char not in "0123456789abcdef" for char in helper)):
                raise Failure("desktop descriptor protocol/instance differs", "ABORTED_SAFETY")
            path = directory / ("session-bridge-" + helper + ".sock")
            if descriptor.get("socket_path") != str(path) or path.is_symlink():
                raise Failure("desktop endpoint escaped owned runtime", "ABORTED_SAFETY")
            metadata = path.stat()
            if not stat.S_ISSOCK(metadata.st_mode) or metadata.st_uid != os.getuid() or metadata.st_mode & 0o777 != 0o600:
                raise Failure("desktop endpoint metadata differs", "ABORTED_SAFETY")
            if [descriptor.get("pid"), str(descriptor.get("start_time"))] != expected_helper:
                raise Failure("desktop descriptor process differs", "ABORTED_SAFETY")
            remaining()
            bus = Gio.bus_get_sync(Gio.BusType.SESSION, cancellable)
            remaining()
            def call(destination, object_path, interface, method, parameters, signature):
                nonlocal stage
                stage = method
                reply = bus.call_sync(destination, object_path, interface, method, parameters,
                    GLib.VariantType.new(signature), Gio.DBusCallFlags.NO_AUTO_START,
                    max(1, (remaining() + 999) // 1000), cancellable)
                remaining()
                return reply
            def helper_owner():
                daemon = ("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus")
                owner, = call(*daemon, "GetNameOwner",
                    GLib.Variant("(s)", ("org.punto.Switcher.SessionBridge1",)), "(s)").unpack()
                if not Gio.dbus_is_unique_name(owner):
                    raise Failure("desktop bus owner is not unique", "ABORTED_SAFETY")
                credentials = call(*daemon, "GetConnectionCredentials", GLib.Variant("(s)", (owner,)),
                                   "(a{sv})").get_child_value(0)
                for key, expected in (("ProcessID", expected_helper[0]), ("UnixUserID", os.getuid())):
                    value = credentials.lookup_value(key, None)
                    if value is None or value.get_type_string() != "u":
                        raise Failure("desktop bus credential type differs", "ABORTED_SAFETY")
                    actual = value.unpack()
                    if type(actual) is not int or not 0 <= actual < 2**32 or actual != expected:
                        raise Failure("desktop bus credentials differ", "ABORTED_SAFETY")
                return owner
            owner = helper_owner()
            values = call(owner, "/org/punto/Switcher/DesktopObserver1", "org.punto.Switcher.DesktopObserver1",
                "ReadState", GLib.Variant("(t)", (deadline_us,)), "(usstty)").unpack()
            if helper_owner() != owner:
                raise Failure("desktop bus owner changed during read", "ABORTED_SAFETY")
            if (descriptor_path.read_bytes() != descriptor_bytes or
                    Path(f"/proc/{expected_helper[0]}/stat").read_text().rsplit(")", 1)[1].split()[19] != expected_helper[1]):
                raise Failure("desktop descriptor/process changed during read", "ABORTED_SAFETY")
            remaining()
            receipt = desktop_receipt(values, descriptor, expected_helper, time.monotonic_ns() // 1000)
            receipt.update(session_bus_owner=owner, deadline_us=deadline_us)
        except BaseException as error:
            primary = error
            if isinstance(error, GLib.Error):
                error.add_note(f"Desktop Gio call: {stage}.")
                if any(error.matches(Gio.io_error_quark(), code) for code in
                       (Gio.IOErrorEnum.CANCELLED, Gio.IOErrorEnum.TIMED_OUT)):
                    primary = Failure("desktop Gio observation cancelled or timed out", "INVALID_OBSERVATION")
                    primary.add_note(f"Desktop Gio call: {stage}: {error}")
                    raise primary from error
            raise
        finally:
            timer.cancel()
            timer.join(timeout=max(0, (deadline_us - time.monotonic_ns() // 1000) / 1_000_000))
            if timer.is_alive():
                if primary is None:
                    raise Failure("desktop cancellation did not finish within deadline", "INVALID_OBSERVATION")
                primary.add_note("Desktop cancellation remained active at deadline.")
        remaining()
        return receipt

    def prepare(self, text, target=None):
        deadline = time.monotonic() + 5
        before = self.system(prepare_pending=True)
        if before["pressed"]:
            raise Failure("held key before preparation", "ABORTED_SAFETY")
        first, second = ("entry", "text_view") if self.surface == "gtk" else ("input", "textarea")
        target = target or first
        if target not in (first, second):
            raise Failure("unsupported owned target")
        self.target("prepare", target=second if target == first else first, text="untouched-sentinel")
        result = self.target("prepare", target=target, text=text)
        if self.surface == "chromium":
            self.page.bring_to_front()
        while time.monotonic() < deadline:
            observed = self.system(prepare_pending=True)
            if observed["pressed"] or any(observed[field] != before[field] for field in ("clipboard", "identity")):
                raise Failure("preparation authority drift or held key", "ABORTED_SAFETY")
            if time.monotonic() >= deadline:
                break
            if observed["foreground"]:
                return {**result, **observed}
            time.sleep(0.05)
        raise Failure("owned foreground preparation deadline expired")

    def call(self, request):
        legacy = profile_contract(getattr(self, "m", {}))
        if legacy and (request["surface"] != "gtk" or request["action"] not in
                ("prepare", "status", "snapshot", "identity", "arm", "complete", "check") or
                request.get("target") not in (None, "entry", "text_view")):
            raise Failure("RPC is outside exact285 GTK16 profile")
        self.deadline_ns = request.get("deadline_ns")
        if request["action"].startswith("lifecycle-"):
            if not self.m.get("lifecycle_phase") or request["surface"] != "gtk":
                raise Failure("lifecycle RPC outside owned phase")
            action = request["action"][10:]
            if not (action == "fixture" and request.get("reassert") is True):
                self.surface = "gtk"
            if action == "begin":
                result = self.lifecycle_begin()
            elif action == "fixture":
                result = self.lifecycle_fixture(request["original"], request.get("reassert", False))
            elif action == "restart":
                result = self.lifecycle_restart()
            elif action == "restore":
                result = self.lifecycle_restore()
            elif action == "stats":
                result = self.lifecycle_stats()
            elif action == "reset":
                result = self.lifecycle_reset()
            elif action == "sound-save":
                result = self.lifecycle_sound_save(request["enabled"])
            elif action == "audio-start":
                result = self.audio_start()
            elif action in ("audio-begin", "audio-finish"):
                result = self.audio_window(action[6:], request.get("direction"), request.get("enabled"))
            else:
                raise Failure("unsupported fixed lifecycle action")
            if action not in ("begin", "restart", "restore", "fixture"):
                result.update(self.system(prepare_pending=True))
            result["guest_monotonic_ns"] = time.monotonic_ns()
            return result
        if request["surface"] == "terminal" and request["action"] == "start":
            result = self.terminal_start(request["nonce"], request.get("automatic", False))
            result["guest_monotonic_ns"] = time.monotonic_ns()
            return result
        if request["surface"] == "terminal" and request["action"] == "close":
            self.terminal_close()
            return {**self.system(prepare_pending=True), "terminal_closed": True}
        self.surface = request["surface"]
        action = request["action"]
        if action == "prepare":
            result = self.prepare(request.get("text", ""), request.get("target"))
        elif action == "status":
            if not (legacy or self.m.get("features") or self.m.get("terminal_phase") or self.m.get("lifecycle_phase")):
                raise Failure("status mutation outside admitted cohort")
            before = self.system()
            if legacy and before["pressed"]:
                raise Failure("held key during legacy status", "ABORTED_SAFETY")
            wire = {"profile": "exact285-gtk16"} if legacy else {}
            result = {"enabled": runtime_status(request.get("enabled"), expected_pid=before["identity"]["daemon"][0], **wire)}
            after = self.system()
            if (legacy and after["pressed"]) or any(before[field] != after[field] for field in ("identity", "clipboard")):
                raise Failure("authority changed during status command", "ABORTED_SAFETY")
        elif action == "stats":
            if not self.m.get("features"):
                raise Failure("STATS outside admitted feature cohort")
            before = self.system()
            if before["pressed"]:
                raise Failure("held key during STATS", "ABORTED_SAFETY")
            result = {"stats": public_stats(before["identity"]["daemon"][0])}
            after = self.system()
            if after["pressed"] or any(before[field] != after[field] for field in ("identity", "clipboard")):
                raise Failure("authority changed during STATS", "ABORTED_SAFETY")
        elif action == "check":
            result = {"ok": True}
        else:
            value = {k: v for k, v in request.items() if k not in {"action", "surface", "request_id", "deadline_ns"}}
            result = self.target(action, **value)
        if action != "prepare":
            result.update(self.system())
        result["guest_monotonic_ns"] = time.monotonic_ns()
        return result

    def close(self):
        def close_chrome():
            if self.chrome and self.chrome.poll() is None:
                self.chrome.terminate()
                try:
                    self.chrome.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    self.chrome.kill()
                    self.chrome.wait(timeout=2)
        try:
            with ExitStack() as cleanup:
                if self.playwright:
                    cleanup.callback(self.playwright.stop)
                cleanup.callback(close_chrome)
                if self.clipboard_reader:
                    cleanup.callback(self.clipboard_reader.close)
                if self.gtk:
                    cleanup.callback(self.gtk.close)
                cleanup.callback(self.terminal_close)
                cleanup.callback(self.lifecycle_restore)
        except Exception as error:
            raise Failure("owned guest cleanup failed", "ABORTED_SAFETY") from error

def clipboard_reader_main():
    from gtk_target import ClipboardPublicationWatch, ClipboardUnavailable, Gdk, Gtk, independent_clipboard_snapshot
    watch = ClipboardPublicationWatch()
    clipboard = Gtk.Clipboard.get(Gdk.SELECTION_CLIPBOARD)
    signal.signal(signal.SIGALRM, signal.SIG_DFL)
    print(json.dumps({"ok": True, "ready": True}), flush=True)
    try:
        for line in sys.stdin:
            try:
                deadline_ns = json.loads(line)["deadline_ns"]
                remaining = (deadline_ns - time.monotonic_ns()) / 1e9
                if remaining <= 0:
                    raise Failure("clipboard shared deadline expired", "INVALID_OBSERVATION")
                signal.setitimer(signal.ITIMER_REAL, remaining)
                result = independent_clipboard_snapshot(clipboard, watch, deadline_ns)
                print(json.dumps({"ok": True, "clipboard": result}), flush=True)
                signal.setitimer(signal.ITIMER_REAL, 0)
            except Exception as error:
                response = {"ok": False, "terminal": True, "error": str(error), "kind": "INVALID_OBSERVATION"}
                if isinstance(error, ClipboardUnavailable):
                    notes = [note for note in getattr(error, "__notes__", [])[:8]
                             if isinstance(note, str) and len(note) <= 1024 and re.fullmatch(
                                 r"clipboard_fence before_owner=[0-9]+ before_generation=[0-9]+ "
                                 r"before_timestamp=[0-9]+ after_owner=[0-9]+ after_generation=[0-9]+ "
                                 r"after_timestamp=[0-9]+", note)]
                    if notes:
                        response["notes"] = notes
                print(json.dumps(response), flush=True)
                break
    finally:
        watch.close()


def gtk_main():
    from gtk_target import ScratchTarget, ClipboardPublicationWatch, Gdk, GLib, Gtk
    import gi
    gi.require_version("GdkX11", "3.0")
    from gi.repository import GdkX11
    class Target(ScratchTarget):
        def prepare(self, target, text):
            super().prepare(target, text)
            window = self.window.get_window()
            if window is None:
                raise Failure("owned GTK window is not realized")
            window.set_events(window.get_events() | Gdk.EventMask.PROPERTY_CHANGE_MASK)
            stamp = GdkX11.x11_get_server_time(window)
            if not stamp:
                raise Failure("owned GTK server timestamp unavailable")
            self.window.present_with_time(stamp)
        def handle_line(self, line):
            if not line:
                Gtk.main_quit()
                return False
            command = json.loads(line)
            if command.get("action") == "lifecycle-fixture":
                watch, written = None, False
                try:
                    deadline = command["deadline_ns"]
                    self.protocol.ensure_prepare_allowed()
                    if (bool(getattr(self, "fixture_started", False)) != (command.get("reassert") is True) or
                            command["observer_instance_id"] != self.protocol.observer_instance_id):
                        raise Failure("fixture already started or observer instance differs")
                    text = lifecycle_fixture_profile(command["clipboard"])
                    if time.monotonic_ns() >= deadline:
                        raise Failure("fixture deadline expired")
                    watch = ClipboardPublicationWatch()
                    initial = watch.fence(deadline)
                    display = Gdk.Display.get_default()
                    clipboard = Gtk.Clipboard.get(Gdk.SELECTION_CLIPBOARD)
                    if time.monotonic_ns() >= deadline:
                        raise Failure("fixture deadline expired before publication")
                    self.fixture_started = written = True
                    clipboard.set_text(text, -1)
                    display.flush()
                    published = watch.fence(deadline)
                    window = GdkX11.X11Window.lookup_for_display(display, published["owner"])
                    if (not window or window.is_destroyed() or window.get_window_type() == Gdk.WindowType.FOREIGN or
                            window.get_display() != display or GdkX11.X11Window.get_xid(window) != published["owner"] or
                            not published["timestamp"] or published["publication_generation"] <= initial["publication_generation"] or
                            time.monotonic_ns() >= deadline):
                        raise Failure("fixture publication is not fresh and locally owned")
                    result = {"ok": True, "publication": {**published, "pid": os.getpid(),
                              "observer_instance_id": self.protocol.observer_instance_id}}
                except Exception as error:
                    result = {"ok": False, "error": str(error),
                              "kind": "ABORTED_SAFETY" if written else "INVALID_OBSERVATION"}
                finally:
                    if watch is not None:
                        watch.close()
                print(json.dumps(result), flush=True)
                return True
            if command.get("action") == "identity":
                print(json.dumps({"ok": True, "observer_id": "gtk", "observer_instance_id": self.protocol.observer_instance_id,
                                  "observer_ns": time.monotonic_ns()}), flush=True)
                return True
            return super().handle_line(line)
    target = Target()
    GLib.io_add_watch(sys.stdin, GLib.IO_IN | GLib.IO_HUP, lambda stream, _: target.handle_line(stream.readline()))
    target.window.show_all()
    print(json.dumps({"ok": True, "ready": True}), flush=True)
    Gtk.main()


def guest_main():
    owned_group = guest_group()
    manifest = json.loads(sys.stdin.readline())
    actual_uuid = checked_run(["sudo", "-n", "cat", "/sys/class/dmi/id/product_uuid"]).strip().lower()
    if actual_uuid != manifest["domain_uuid"].lower():
        raise Failure("guest DMI UUID differs from the guarded disposable domain")
    os.environ.update(manifest["session"])
    check_files({str(Path(__file__).parent / name): digest for name, digest in manifest["observer_assets"].items()})
    guest = Guest(manifest)
    def expired(_signum, _frame):
        signal.setitimer(signal.ITIMER_REAL, 0)
        with ExitStack() as cleanup:
            cleanup.callback(kill_guest_group, owned_group)
            for process in (guest.gtk.child if guest.gtk else None, guest.chrome,
                            guest.clipboard_reader.child if guest.clipboard_reader else None):
                if process and process.poll() is None:
                    cleanup.callback(os.killpg, process.pid, signal.SIGKILL)
            cleanup.callback(guest.terminal_close, force=True)
            cleanup.callback(guest.lifecycle_restore)
    signal.signal(signal.SIGALRM, expired)
    try:
        signal.setitimer(signal.ITIMER_REAL, 30)
        cold = {"toolkit_accessibility": checked_run(["gsettings", "get", "org.gnome.desktop.interface", "toolkit-accessibility"]).strip(),
                "a11y_bus_owned": checked_run(["gdbus", "call", "--session", "--dest", "org.freedesktop.DBus", "--object-path", "/org/freedesktop/DBus", "--method", "org.freedesktop.DBus.NameHasOwner", "org.a11y.Bus"]).strip(),
                **({} if profile_contract(manifest) else
                   {"chromium_version": checked_run([manifest["chromium"], "--version"]).strip()})}
        guest.setup()
        print(json.dumps({"ok": True, "ready": True, "cold_preconditions": cold}), flush=True)
        for line in sys.stdin:
            try:
                request = json.loads(line)
                if request.get("deadline_ns") is not None:
                    remaining = (request["deadline_ns"] - time.monotonic_ns()) / 1e9
                    if remaining <= 0:
                        raise Failure("guest shared deadline expired", "INVALID_OBSERVATION")
                    signal.setitimer(signal.ITIMER_REAL, min(10, remaining))
                elif request["action"].startswith("lifecycle-"):
                    signal.setitimer(signal.ITIMER_REAL, 25)
                result = guest.call(request)
                print(json.dumps({"ok": True, "request_id": request["request_id"], **result}), flush=True)
                signal.setitimer(signal.ITIMER_REAL, 30)
            except Exception as error:
                print(json.dumps({"ok": False, "error": str(error), "kind": getattr(error, "kind", "BLOCKED_PRECONDITION"),
                                  "notes": [note[:1024] for note in getattr(error, "__notes__", [])[:8]]}), flush=True)
                break
    finally:
        signal.setitimer(signal.ITIMER_REAL, 25 if guest.learning_phase else 5)
        guest.close()
        signal.setitimer(signal.ITIMER_REAL, 0)


class Collector:
    def __init__(self, manifest, guard, channel, output):
        self.m, self.guard, self.channel, self.output = manifest, guard, channel, output
        self.completed = 0
        self.baseline = None
        self.last_group = None
        self.serials = {}
        self.original_status = None
        self.guest_offset_upper_ns = None
        self.pipeline_successors = {}
        self.lifecycle_terminal_cases = 0
        self.terminal_admission = None

        self.desktop_previous = None

    def authority_identity(self):
        return {**self.baseline["identity"], **self.pipeline_successors}

    def lifecycle_fixture(self, reassert=False, deadline_ns=None):
        deadline = time.monotonic_ns() + 5_000_000_000 if deadline_ns is None else deadline_ns
        if type(reassert) is not bool:
            raise Failure("fixture reassert authorization is not boolean", "INVALID_OBSERVATION")
        if (not self.m.get("lifecycle_phase") or
                bool(getattr(self, "lifecycle_fixture_started", False)) != reassert):
            raise Failure("lifecycle fixture outside its single admitted phase", "INVALID_OBSERVATION")
        lifecycle_fixture_profile(self.baseline["clipboard"])
        self.guard.check()
        check_files(self.m["sources"])
        check_files({self.m["package"]["path"]: self.m["package"]["sha256"]})
        self.lifecycle_fixture_started = True
        record = self.rpc("gtk", "lifecycle-fixture", original=self.baseline["clipboard"], deadline_ns=deadline,
                          **({"reassert": True} if reassert else {}))
        self.authority(record["before"])
        verify_lifecycle_fixture(record["before"], record["after"], record["publication"])
        with (self.output / ("lifecycle-fixture-reassert.jsonl" if reassert else "lifecycle-fixture.json")).open("a" if reassert else "x") as stream:
            stream.write(json.dumps(record, ensure_ascii=False) + "\n")
        self.baseline["clipboard"] = record["after"]["clipboard"]

    def lifecycle_rpc(self, action, **values):
        self.guard.check()
        check_files(self.m["sources"])
        check_files({self.m["package"]["path"]: self.m["package"]["sha256"]})
        record = self.rpc("gtk", "lifecycle-" + action, **values)
        if action == "restore" and record.get("restoration_only"):
            with (self.output / "lifecycle-transitions.jsonl").open("a") as stream:
                stream.write(json.dumps({"action": action, **record}, ensure_ascii=False) + "\n")
            failure = Failure("restoration acknowledged without full observer authority", "INVALID_OBSERVATION")
            failure.add_note("Original files, pipeline and config/status acknowledged; no input authority or baseline accepted.")
            observation = record.get("observation_failure", {})
            if observation:
                if observation.get("kind") == "ABORTED_SAFETY":
                    failure.kind = "ABORTED_SAFETY"
                failure.add_note(f"Original observer failure [{observation.get('kind', 'unclassified')}]: {observation.get('error', '')[:512]}")
                for note in observation.get("notes", [])[:6]:
                    failure.add_note(note[:1024])
            raise failure
        if action in ("begin", "restart", "restore"):
            successor_identity(record["before"], record["after"])
            if (not record.get("old_dead") or record["before"]["identity"] != self.authority_identity() or
                    record["before"]["clipboard"] != self.baseline["clipboard"]):
                raise Failure("successor lacks exact prior identity/death proof", "ABORTED_SAFETY")
            self.pipeline_successors.update({r: record["after"]["identity"][r] for r in PIPELINE_ROLES})
            self.authority(record["after"])
            with (self.output / "lifecycle-transitions.jsonl").open("a") as stream:
                stream.write(json.dumps({"action": action, **record}, ensure_ascii=False) + "\n")
        else:
            self.authority(record)
        return record

    def learning_wait(self, epoch, generation, words):
        expected = "".join(word + "\n" for word in sorted(words))
        deadline = time.monotonic() + 5
        while True:
            record = self.lifecycle_rpc("stats")
            stats = record["stats"]
            if stats["daemon_epoch"] != epoch or stats["learning_failed"]:
                raise Failure("learning epoch/failure changed", "FAIL_PRODUCT")
            if (record["store"] == expected and stats["learning_completed_generation"] >= generation and
                    not stats["learning_pending"]):
                durable_learning(stats, epoch, generation, len(words))
                return record
            if time.monotonic() >= deadline:
                raise Failure("exact durable learned record timeout", "FAIL_PRODUCT")
            time.sleep(.02)

    def lifecycle_word(self, words, *, learn=False, excluded=False, manual=False, sound=None):
        widget, before, group = self.feature_start("gtk", "owned-learning-audio")
        source, keys, target, tail = ("ghbdtn", "ghbdtn", "привет", "ф") if group == 0 else ("руддщ", "hello", "hello", "a")
        stats_before = self.lifecycle_rpc("stats")["stats"]
        if learn and keys in words:
            raise Failure("training word was not absent in isolated store")
        audio = self.lifecycle_rpc("audio-begin", direction=group, enabled=sound) if sound is not None else None
        self.type_keys("gtk", keys)
        self.wait_state("gtk", widget, before, source, [len(source)] * 2, group)
        trigger = self.key("gtk", "pause" if manual else "space")
        expected, newgroup = (target, 1 - group) if manual else ((source + " ", group) if excluded else (target + " ", 1 - group))
        observed = self.wait_state("gtk", widget, before, expected, [len(expected)] * 2, newgroup)
        if learn:
            self.key("gtk", "ctrl", "z", deadline_ns=trigger + 2_000_000_000)
            observed = self.wait_state("gtk", widget, before, source + " ", [len(source) + 1] * 2, group)
            self.require_key_count(before, observed, "z", 0)
            words.add(keys)
            learned = self.learning_wait(stats_before["daemon_epoch"], stats_before["learning_generation"] + 1, words)
            self.key("gtk", "pause")
            expected, newgroup = target + " ", 1 - group
            observed = self.wait_state("gtk", widget, before, expected, [len(expected)] * 2, newgroup)
        elif excluded:
            deadline = time.monotonic() + 5
            while True:
                stats = self.lifecycle_rpc("stats")["stats"]
                if stats["analyzed"] > stats_before["analyzed"] and not any(stats[n] for n in
                        ("pending_words", "ready_results", "analysis_outstanding", "input_in_flight", "queued_tasks")):
                    break
                if time.monotonic() >= deadline:
                    raise Failure("excluded word not analyzed/drained")
                time.sleep(.02)
            if stats["word_dispatches"] != stats_before["word_dispatches"]:
                raise Failure("excluded word dispatched automatic mutation", "FAIL_PRODUCT")
            self.wait_state("gtk", widget, before, expected, [len(expected)] * 2, group)
            self.key("gtk", "pause")
            expected, newgroup = target + " ", 1 - group
            observed = self.wait_state("gtk", widget, before, expected, [len(expected)] * 2, newgroup)
        self.key("gtk", "a")
        observed = self.wait_state("gtk", widget, before, expected + tail, [len(expected) + 1] * 2, newgroup)
        details = {"cohort": "learning-tray-audio", "learn": learn, "excluded": excluded,
                   "manual": manual, "direction": group}
        if learn:
            details["learning"] = learned
        if audio is not None:
            details["audio"] = self.lifecycle_rpc("audio-finish")
        self.feature_finish("gtk", before, observed, **details)
        return observed

    def lifecycle(self):
        if not self.m.get("lifecycle_phase"):
            raise Failure("lifecycle cohort not admitted")
        primary = None
        self.lifecycle_fixture()
        self.lifecycle_rpc("begin")
        try:
            # The frozen terminal cases run only after the owned learning store exists.
            if self.m.get("terminal_phase"):
                start = self.completed
                self.terminals()
                self.lifecycle_terminal_cases = self.completed - start
            self.lifecycle_rpc("reset")
            if self.lifecycle_rpc("stats")["store"] != "":
                raise Failure("owned store not empty before training")
            self.rpc("gtk", "status", enabled=True)
            words = set()
            for _ in (0, 1):
                self.lifecycle_word(words, learn=True)
            for _ in (0, 1):
                self.lifecycle_word(words, excluded=True)
            for _ in (0, 1):
                self.lifecycle_word(words, excluded=True)
            self.lifecycle_rpc("restart")
            for _ in (0, 1):
                self.lifecycle_word(words, excluded=True)
            self.lifecycle_rpc("reset")
            self.lifecycle_rpc("restart")
            words.clear()
            for _ in (0, 1):
                self.lifecycle_word(words)
            self.lifecycle_rpc("audio-start")
            for enabled in (True, False, True):
                save = self.lifecycle_rpc("sound-save", enabled=enabled)
                with (self.output / "lifecycle-save.jsonl").open("a") as stream:
                    stream.write(json.dumps(save, ensure_ascii=False) + "\n")
                for _ in (0, 1):
                    self.lifecycle_word(words, manual=True, sound=enabled)
        except Exception as error:
            primary = error
            raise
        finally:
            try:
                restored = self.lifecycle_rpc("restore")
                if not restored["restoration"]["restored"]:
                    raise Failure("owned originals not restored", "ABORTED_SAFETY")
                prepared = self.rpc("chromium", "prepare", text="")
                self.authority(prepared, foreground=True)
            except Exception as error:
                failure = Failure("lifecycle cleanup failed", "ABORTED_SAFETY")
                if primary is not None:
                    failure.add_note(f"Primary lifecycle failure: {type(primary).__name__} "
                                     f"[{getattr(primary, 'kind', 'unclassified')}]: {str(primary)[:512]}")
                failure.add_note(f"Lifecycle cleanup failure: {type(error).__name__} "
                                 f"[{getattr(error, 'kind', 'unclassified')}]: {str(error)[:512]}")
                for note in [*getattr(primary, "__notes__", [])[:3], *getattr(error, "__notes__", [])[:3]]:
                    failure.add_note(note[:1024])
                raise failure from (primary if primary is not None else error)


    def rpc(self, surface, action, *, deadline_ns=None, **value):
        request = {"surface": surface, "action": action, "request_id": str(uuid.uuid4()), **value}
        if deadline_ns is not None:
            if self.guest_offset_upper_ns is None:
                raise Failure("guest deadline mapping unavailable", "INVALID_OBSERVATION")
            request["deadline_ns"] = deadline_ns - self.guest_offset_upper_ns
            record = self.channel.call(request, deadline_ns=deadline_ns)
        else:
            record = self.channel.call(request)
        if "host_interval_ns" in record and "guest_monotonic_ns" in record:
            self.guest_offset_upper_ns = record["host_interval_ns"][1] - record["guest_monotonic_ns"]
        if action == "snapshot":
            instance, serial = record["observer_instance_id"], record["observation_id"]
            if serial <= self.serials.get(instance, 0):
                raise Failure("non-increasing observer snapshot serial", "INVALID_OBSERVATION")
            self.serials[instance] = serial
        with (self.output / "observations.jsonl").open("a") as stream:
            stream.write(json.dumps({"surface": surface, "action": action, **record}, ensure_ascii=False) + "\n")
        return record

    def key(self, surface, *keys, deadline_ns=None):
        check_files(self.m["sources"])
        check_files({self.m["package"]["path"]: self.m["package"]["sha256"]})
        self.check(surface, foreground=True)
        start = time.monotonic_ns()
        if deadline_ns is not None and start >= deadline_ns:
            raise Failure("undo admission exceeded 2500ms")
        if deadline_ns is None:
            self.guard.send(list(keys))
        else:
            self.guard.send(list(keys), deadline_ns=deadline_ns)
        with (self.output / "input.jsonl").open("a") as stream:
            stream.write(json.dumps({"uuid": self.guard.identity, "keys": keys, "started_ns": start,
                                     "finished_ns": time.monotonic_ns()}) + "\n")
        return start

    def check(self, surface, foreground=False, idle=False):
        authority = self.rpc(surface, "check")
        self.authority(authority, foreground, idle)
        return authority

    def authority(self, authority, foreground=False, idle=False):
        if isinstance(authority["clipboard"], dict) and authority["clipboard"].get("publication_pending"):
            raise Failure("clipboard publication is not input authority", "ABORTED_SAFETY")
        if self.baseline is None:
            self.baseline = authority
        if authority["pressed"] or (foreground and not authority["foreground"]):
            raise Failure("physical input not admitted to owned released foreground", "ABORTED_SAFETY")
        for field in ("clipboard", "identity"):
            expected = self.authority_identity() if field == "identity" else self.baseline[field]
            if authority[field] != expected:
                raise Failure(f"{field} changed since run admission", "ABORTED_SAFETY")
        if authority.get("terminal_identity") != self.terminal_admission:
            raise Failure("owned terminal admission changed", "ABORTED_SAFETY")
        if self.m.get("desktop_parity"):
            verify_desktop(self.desktop_previous or self.baseline, authority, authority["group"])
            self.desktop_previous = authority
        if idle and authority["group"] != self.last_group:
            raise Failure("layout changed during idle interval", "FAIL_PRODUCT")

    def wait_state(self, surface, widget, before, text, selection, group, **expectations):
        deadline = time.monotonic() + 5
        deadline_ns = int(deadline * 1e9)
        while True:
            observed = self.rpc(surface, "snapshot", target=widget, deadline_ns=deadline_ns)
            if expectations.get("clipboard_payload_restore"):
                if self.m.get("desktop_parity"):
                    verify_desktop_authority(self.desktop_previous or before, observed)
                    self.desktop_previous = observed
                state = observed["state"]
                if observed["group"] not in (before["group"], group):
                    raise Failure("unexpected layout during clipboard restoration", "FAIL_PRODUCT")
                allowed_focus = {before["state"].get("focus_target"), expectations.get("expected_focus")}
                if state.get("focus_target") not in allowed_focus:
                    raise Failure("foreign focus during clipboard restoration", "ABORTED_SAFETY")
                following = state["following_text"]
                allowed_following = {before["state"]["following_text"]}
                if expectations.get("expected_following") is not None:
                    allowed_following.add(expectations["expected_following"])
                if following not in allowed_following:
                    raise Failure("foreign control changed during clipboard restoration", "ABORTED_SAFETY")
                verify_state(before, {**observed, "clipboard": before["clipboard"]}, state["text"],
                             state["selection"], observed["group"], expected_focus=state.get("focus_target"),
                             expected_following=following, desktop_pending_group=group)
            if isinstance(observed["clipboard"], dict) and observed["clipboard"].get("publication_pending"):
                if not expectations.get("clipboard_payload_restore"):
                    raise Failure("unexpected clipboard publication", "ABORTED_SAFETY")
                publication = observed["clipboard"]
                if (type(publication.get("before_generation")) is not int
                        or type(publication.get("after_generation")) is not int
                        or publication["after_generation"] <= publication["before_generation"]):
                    raise Failure("invalid publication-pending evidence", "INVALID_OBSERVATION")
                if time.monotonic() >= deadline:
                    raise Failure("clipboard publication did not settle", "INVALID_OBSERVATION")
                time.sleep(min(0.05, max(0, deadline - time.monotonic())))
                continue
            matched = (observed["state"]["text"], observed["state"]["selection"], observed["group"]) == (text, selection, group)
            if expectations.get("expected_focus") is not None:
                matched = matched and observed["state"].get("focus_target") == expectations["expected_focus"]
            if expectations.get("expected_following") is not None:
                matched = matched and observed["state"]["following_text"] == expectations["expected_following"]
            if self.m.get("desktop_parity"):
                matched = matched and observed["desktop"]["current_role"] == group and all(
                    observed["xkb"][field] == group for field in ("group", "locked_group"))
            stable = (expectations.get("clipboard_payload_restore") or observed["clipboard"] == before["clipboard"]) and not observed["pressed"]
            if matched and stable and expectations.get("clipboard_payload_restore"):
                verify_state(before, {**observed, "clipboard": before["clipboard"]},
                             text, selection, group, **expectations)
                try:
                    verify_state(before, observed, text, selection, group, **expectations)
                except ClipboardNotRestored:
                    stable = False
            if matched and stable and time.monotonic() >= deadline:
                raise Failure("feature observation exceeded shared deadline", "INVALID_OBSERVATION")
            if matched and stable or time.monotonic() >= deadline:
                break
            time.sleep(min(0.05, max(0, deadline - time.monotonic())))
        verify_state(before, observed, text, selection, group, **expectations)
        if self.m.get("desktop_parity"):
            verify_desktop(self.desktop_previous or before, observed, group)
            self.desktop_previous = observed
        return observed

    def arm(self, surface, case, widget):
        identity = self.rpc(surface, "identity")
        lower, upper = identity["host_interval_ns"]
        offset = (lower + upper) // 2 - identity["observer_ns"]
        mapping = {"observer_id": identity["observer_id"], "observer_instance_id": identity["observer_instance_id"],
            "observer_domain": "collector_monotonic_ns" if surface in ("gtk", "terminal") else "performance_now_ns",
            "collector_domain": "collector_monotonic_ns", "offset_ns": offset, "mapping_id": str(uuid.uuid4()),
            "calibration": {"source": "collector_handshake", "calibration_id": str(uuid.uuid4()), "before": identity,
                            "uncertainty_ns": (upper - lower + 1) // 2, "kind": "measured_interval_estimate_not_R10_attestation"}}
        attempt = str(uuid.uuid4())
        explicit = {"target": widget} if profile_contract(self.m) else {}
        self.rpc(surface, "arm", run_id=self.m["run_id"], case_id=case, attempt_id=attempt, clock_mapping=mapping, **explicit)
        return attempt, self.rpc(surface, "snapshot", target=widget)


    def terminal_enter(self):
        check_files(self.m["sources"])
        check_files({self.m["package"]["path"]: self.m["package"]["sha256"]})
        authority = self.check("terminal", foreground=True)
        if not self.terminal_admission or authority.get("terminal_identity") != self.terminal_admission:
            raise Failure("Enter target does not match terminal phase", "ABORTED_SAFETY")
        started = time.monotonic_ns()
        admission = {**self.terminal_admission, "surface": "terminal", "run_id": self.m["run_id"],
                     "expires_ns": started + 1_000_000_000}
        self.guard.send(["enter"], deadline_ns=admission["expires_ns"], terminal_admission=admission)
        with (self.output / "input.jsonl").open("a") as stream:
            stream.write(json.dumps({"uuid": self.guard.identity, "keys": ["enter"],
                "terminal_admission": self.terminal_admission, "started_ns": started,
                "finished_ns": time.monotonic_ns()}) + "\n")

    def terminal_case(self, mode):
        nonce = uuid.uuid4().hex
        prepared = self.rpc("terminal", "start", nonce=nonce, automatic=mode == "auto-undo")
        admission = prepared.get("terminal_admission")
        if (self.terminal_admission is not None or not isinstance(admission, dict) or
                admission.get("nonce") != nonce or prepared.get("terminal_identity") != admission):
            raise Failure("new terminal identity lacks its explicit admission", "ABORTED_SAFETY")
        self.terminal_admission = admission
        try:
            self.authority(prepared, foreground=True)
            group = prepared["group"]
            if group not in (0, 1) or self.last_group is not None and group != self.last_group:
                raise Failure("unexpected terminal source layout; no layout repair")
            self.rpc("terminal", "status", enabled=mode == "auto-undo")
            source, keys, target, tail = ("ghbdtn", "ghbdtn", "привет", "ф") if group == 0 else (
                "руддщ", "hello", "hello", "a")
            _, before = self.arm("terminal", "terminal-" + str(group) + "-" + mode, "terminal")
            def paste(text, selection, group):
                nonlocal before
                deadline = time.monotonic_ns() + 5_000_000_000
                restored = self.wait_state("terminal", "terminal", before, text, selection, group,
                                           clipboard_payload_restore=True)
                self.baseline["clipboard"] = restored["clipboard"]
                if self.m.get("lifecycle_phase"):
                    self.lifecycle_fixture(reassert=True, deadline_ns=deadline)
                before = {**before, "clipboard": self.baseline["clipboard"]}
                return restored
            self.wait_state("terminal", "terminal", before, "", [0, 0], group)
            if mode == "empty-recovery":
                self.key("terminal", "pause")
                verify_state(before, self.rpc("terminal", "snapshot"), "", [0, 0], group)
            self.type_keys("terminal", keys)
            self.wait_state("terminal", "terminal", before, source, [len(source)] * 2, group)
            suffix = ""
            if mode == "auto-undo":
                trigger = self.key("terminal", "space")
                suffix = " "
                corrected = paste(target + suffix, [len(target) + 1] * 2, 1 - group)
                deadline = trigger + 2_500_000_000
                self.key("terminal", "ctrl", "z", deadline_ns=deadline)
                paste(source + suffix, [len(source) + 1] * 2, group)
            self.key("terminal", "pause")
            paste(target + suffix, [len(target + suffix)] * 2, 1 - group)
            if mode == "repeat":
                self.key("terminal", "pause")
                paste(source, [len(source)] * 2, group)
                self.key("terminal", "pause")
                paste(target, [len(target)] * 2, 1 - group)
                trigger = self.key("terminal", "ctrl", "pause")
                changed = paste(target.upper(), [len(target)] * 2, 1 - group)
                self.key("terminal", "ctrl", "z", deadline_ns=trigger + 2_500_000_000)
                paste(target, [len(target)] * 2, 1 - group)
            self.key("terminal", "a")
            expected = target + suffix + tail
            observed = self.wait_state("terminal", "terminal", before, expected, [len(expected)] * 2, 1 - group)
            self.terminal_enter()
            deadline = time.monotonic() + 5
            expected_bytes = (expected + "\n").encode()
            while True:
                received = self.rpc("terminal", "snapshot")
                self.authority(received, foreground=True)
                actual = received["submitted_base64"]
                if terminal_submission(actual, expected_bytes):
                    break
                if time.monotonic() >= deadline:
                    raise Failure("canonical PTY submission was not observed", "FAIL_PRODUCT")
                time.sleep(0.02)
            self.feature_finish("terminal", before, observed, canonical_bytes_base64=actual,
                                terminal_identity=admission, cohort="terminal")
        finally:
            primary = sys.exc_info()[1]
            try:
                closed = self.rpc("terminal", "close")
                if not closed.get("terminal_closed"):
                    raise Failure("owned terminal server/reader cleanup is unconfirmed", "ABORTED_SAFETY")
                self.terminal_admission = None
                self.authority(closed)
            except Exception as error:
                if primary is None:
                    raise
                if getattr(error, "kind", None) == "ABORTED_SAFETY":
                    raise error from primary
                primary.add_note("Terminal cleanup failed: " + type(error).__name__)

    def terminals(self):
        original = self.rpc("chromium", "status")["enabled"]
        if type(original) is not bool:
            raise Failure("original runtime status unavailable")
        self.original_status = original
        (self.output / "status-recovery.json").write_text(json.dumps({
            "enabled": original, "daemon": self.authority_identity()["daemon"]}) + "\n")
        primary_error = None
        try:
            for mode in ("word", "repeat", "empty-recovery", "auto-undo"):
                for _ in (0, 1):
                    self.terminal_case(mode)
        except Exception as error:
            primary_error = error
            raise
        finally:
            try:
                restored = self.rpc("chromium", "status", enabled=original)
                self.authority(restored)
                if restored["enabled"] != original:
                    raise Failure("terminal runtime status restoration failed", "ABORTED_SAFETY")
                prepared = self.rpc("chromium", "prepare", text="")
                self.authority(prepared, foreground=True)
            except Exception as error:
                if primary_error is None:
                    raise
                if getattr(error, "kind", None) == "ABORTED_SAFETY":
                    raise error from primary_error
                primary_error.add_note("Terminal phase status/foreground restoration failed: " + type(error).__name__)

    def legacy(self):
        if not profile_contract(self.m):
            raise Failure("exact285 GTK16 cohort is not admitted")
        self.check("gtk")
        self.original_status = self.rpc("gtk", "status")["enabled"]
        if type(self.original_status) is not bool:
            raise Failure("original legacy runtime status unavailable")
        primary_error = None
        started = self.completed
        try:
            if self.rpc("gtk", "status", enabled=False)["enabled"] is not False:
                raise Failure("legacy automatic correction not disabled", "FAIL_PRODUCT")
            for widget in ("entry", "text_view"):
                for mode in ("word", "selection", "empty", "repeat"):
                    group = self.check("gtk")["group"]
                    for direction in (group, 1 - group):
                        self.operation("gtk", direction, mode, widget=widget)
            if self.completed - started != 16:
                raise Failure("legacy cohort did not complete exactly sixteen cases")
        except Exception as error:
            primary_error = error
            raise
        finally:
            try:
                result = self.rpc("gtk", "status", enabled=self.original_status)
                if result["enabled"] is not self.original_status:
                    raise Failure("original legacy runtime status not restored", "ABORTED_SAFETY")
            except Failure as error:
                if primary_error is None or error.kind == "ABORTED_SAFETY":
                    raise error from primary_error
                primary_error.add_note("Runtime status restoration failed after the original legacy failure.")

    def operation(self, surface, direction=None, mode="word", *, widget=None):
        explicit = {"target": widget} if widget is not None else {}
        if widget is not None and (not profile_contract(self.m) or surface != "gtk" or
                                   widget not in ("entry", "text_view") or mode not in ("word", "selection", "empty", "repeat")):
            raise Failure("explicit widget is outside exact285 GTK16 profile")
        started = time.monotonic_ns()
        prepared = self.rpc(surface, "prepare", text="", **explicit)
        self.authority(prepared, foreground=True)
        actual = prepared["group"]
        if actual not in (0, 1) or (self.last_group is not None and actual != self.last_group) or (direction is not None and direction != actual):
            raise Failure("unexpected initial layout; no source repair permitted")
        direction = actual
        source, keys, target, group, tail = ("ghbdtn", "ghbdtn", "привет", 1, "ф") if direction == 0 else ("руддщ", "hello", "hello", 0, "a")
        suffix = " suffix" if mode == "selection" else ""
        widget = widget or ("entry" if surface == "gtk" else "input")
        case = f"{surface}-{widget}-{direction}-{mode}" if explicit else f"{surface}-{direction}-{mode}"
        _, before = self.arm(surface, case + "-physical-clear", widget)
        if before["group"] != direction:
            raise Failure("layout changed before physical clear")
        self.key(surface, "ctrl", "a")
        self.key(surface, "backspace")
        self.wait_state(surface, widget, before, "", [0, 0], direction)
        terminal = self.rpc(surface, "complete", **explicit)
        self.authority(terminal, foreground=True)
        if terminal["group"] != direction:
            raise Failure("layout changed at preparation completion")
        if suffix:
            selected = self.rpc(surface, "prepare", text=source + suffix, **explicit)
            self.authority(selected, foreground=True)
            if selected["group"] != direction:
                raise Failure("layout changed before selection staging")
        attempt, before = self.arm(surface, case, widget)
        if before["group"] != direction:
            raise Failure("initial layout differs; no source repair permitted")
        if mode == "empty":
            self.key(surface, "pause")
            verify_state(before, self.rpc(surface, "snapshot", target=widget), "", [0, 0], direction)
        if suffix:
            self.key(surface, "home")
            for _ in source:
                self.key(surface, "right")
            self.key(surface, "shift", "home")
            self.wait_state(surface, widget, before, source + suffix, [0, len(source)], direction)
        else:
            for key in keys:
                self.key(surface, key)
            self.wait_state(surface, widget, before, source, [len(source), len(source)], direction)
        self.key(surface, *(["shift", "pause"] if suffix else ["space" if mode == "auto" else "pause"]))
        text = target + (" " if mode == "auto" else "")
        self.wait_state(surface, widget, before, text + suffix, [len(text), len(text)], group)
        if mode == "repeat":
            self.key(surface, "pause")
            self.wait_state(surface, widget, before, source, [len(source)] * 2, direction)
            self.key(surface, "pause")
            self.wait_state(surface, widget, before, target, [len(target)] * 2, group)
        self.key(surface, "a")
        self.wait_state(surface, widget, before, text + tail + suffix, [len(text) + 1] * 2, group)
        self.authority(self.rpc(surface, "identity"), foreground=True)
        self.authority(self.rpc(surface, "complete", **explicit), foreground=True)
        with (self.output / "operations.jsonl").open("a") as stream:
            stream.write(json.dumps({"attempt_id": attempt, "case_id": case,
                "started_ns": started, "finished_ns": time.monotonic_ns(), "status": "PASS"}) + "\n")
        self.completed += 1
        self.last_group = group

    @staticmethod
    def require_key_count(before, after, key, count):
        for phase in ("down", "up"):
            name = key + "_" + phase
            if after["state"]["keys"][name] - before["state"]["keys"][name] != count:
                raise Failure(f"unexpected native {key} {phase} lifecycle", "FAIL_PRODUCT")

    def feature_start(self, surface, mode, widget=None):
        widget = widget or ("entry" if surface == "gtk" else "input")
        prepared = self.rpc(surface, "prepare", target=widget, text="")
        self.authority(prepared, foreground=True)
        group = prepared["group"]
        if group not in (0, 1) or (self.last_group is not None and group != self.last_group):
            raise Failure("feature layout continuity lost")
        case = f"features-{surface}-{group}-{mode}"
        _, before = self.arm(surface, case + "-physical-clear", widget)
        self.key(surface, "ctrl", "a")
        self.key(surface, "backspace")
        self.wait_state(surface, widget, before, "", [0, 0], group)
        self.authority(self.rpc(surface, "complete"), foreground=True)
        _, before = self.arm(surface, case, widget)
        return widget, before, group

    def type_keys(self, surface, text):
        for key in text:
            self.key(surface, *(["space"] if key == " " else ["shift", key.lower()] if key.isupper() else [key]))

    def feature_finish(self, surface, before, observed, **details):
        self.authority(self.rpc(surface, "complete"), foreground=True)
        with (self.output / "operations.jsonl").open("a") as stream:
            stream.write(json.dumps({"attempt_id": before["attempt_id"], "case_id": before["case_id"],
                                    "status": "PASS", "cohort": "features", **details}) + "\n")
        self.completed += 1
        self.last_group = observed["group"]

    def feature_edit(self, surface, mode):
        widget, before, group = self.feature_start(surface, mode)
        source, keys, expected = ("hELLo", "hELLo", "HellO") if group == 0 else ("пРИвет", "gHBdtn", "ПриВЕТ")
        if mode == "translit":
            source, keys, expected = ("privet", "privet", "привет") if group == 0 else ("привет", "ghbdtn", "privet")
        suffix = "" if mode == "case-word" else (" zz" if group == 0 else " яя")
        self.type_keys(surface, keys + (" zz" if suffix else ""))
        self.wait_state(surface, widget, before, source + suffix, [len(source + suffix)] * 2, group)
        if suffix:
            self.key(surface, "home")
            for _ in source:
                self.key(surface, "right")
            self.key(surface, "shift", "home")
            self.wait_state(surface, widget, before, source + suffix, [0, len(source)], group)
        self.key(surface, *( ["ctrl", "pause"] if mode == "case-word" else
                            ["alt", "pause"] if mode == "case-selection" else ["ctrl", "alt", "pause"]))
        restored = self.wait_state(surface, widget, before, expected + suffix, [len(expected)] * 2, group,
                                   clipboard_payload_restore=mode == "translit")
        if mode == "translit":
            self.baseline["clipboard"] = restored["clipboard"]
            before = {**before, "clipboard": restored["clipboard"]}
        self.key(surface, "a")
        tail = "a" if group == 0 else "ф"
        observed = self.wait_state(surface, widget, before, expected + tail + suffix, [len(expected) + 1] * 2, group)
        self.feature_finish(surface, before, observed)

    def feature_tab(self, surface, literal=False):
        widget, before, group = self.feature_start(surface, "literal-tab" if literal else "tab-field",
                                                  "text_view" if literal else None)
        source, keys, expected = ("ghbdtn", "ghbdtn", "привет") if group == 0 else ("руддщ", "hello", "hello")
        self.type_keys(surface, keys)
        self.wait_state(surface, widget, before, source, [len(source)] * 2, group)
        self.key(surface, "tab")
        focus = "gtk_text_view" if surface == "gtk" else "chromium_textarea"
        text = expected + ("\t" if literal else "")
        observed = self.wait_state(surface, widget, before, text, [len(text)] * 2, 1 - group,
                                   expected_focus=focus, clipboard_payload_restore=True)
        self.require_key_count(before, observed, "tab", 1)
        # Only a completed, exact Tab text/focus/representation oracle can adopt
        # the restored clipboard owner. No process or other authority is reset.
        following = observed if literal else self.rpc(surface, "snapshot", target="text_view" if surface == "gtk" else "textarea")
        destination = widget if literal else ("text_view" if surface == "gtk" else "textarea")
        if not literal:
            verify_state(observed, following, before["state"]["following_text"],
                         [len(before["state"]["following_text"])] * 2, 1 - group,
                         expected_focus=focus, expected_following=expected)
        self.baseline["clipboard"] = observed["clipboard"]
        self.key(surface, "a")
        tail = "ф" if group == 0 else "a"
        final = self.wait_state(surface, destination, following, following["state"]["text"] + tail,
                                [len(following["state"]["text"]) + 1] * 2, 1 - group)
        self.require_key_count(before, final, "tab", 1)
        self.feature_finish(surface, before, final, clipboard_proof="all_plain_representations_restored")

    def feature_undo(self, surface, mode):
        widget, before, group = self.feature_start(surface, "undo-" + mode)
        source, keys, corrected = ("hELLo", "hELLo", "HellO") if group == 0 else ("пРИвет", "gHBdtn", "ПриВЕТ")
        self.type_keys(surface, keys)
        self.wait_state(surface, widget, before, source, [len(source)] * 2, group)
        if mode == "expiry":
            drain_fields = ("input_in_flight", "pending_words", "ready_results", "analysis_outstanding", "queued_tasks")
            baseline_deadline_ns = time.monotonic_ns() + 5_000_000_000
            self.guard.check()
            baseline = self.rpc(surface, "stats", deadline_ns=baseline_deadline_ns)
            self.authority(baseline, foreground=True)
            stats_before = baseline["stats"]
            if (time.monotonic_ns() >= baseline_deadline_ns or not stats_before["daemon_epoch"] or
                    any(stats_before[field] for field in drain_fields)):
                raise Failure("undo expiry baseline is not quiescent")
        trigger = self.key(surface, "ctrl", "pause")
        changed = self.wait_state(surface, widget, before, corrected, [len(corrected)] * 2, group)
        if mode == "immediate":
            self.key(surface, "ctrl", "z", deadline_ns=trigger + 2_500_000_000)
            changed = self.wait_state(surface, widget, before, source, [len(source)] * 2, group)
            self.require_key_count(before, changed, "z", 0)
        elif mode == "expiry":
            completion_deadline_ns = trigger + 5_000_000_000
            seen_dispatches = stats_before["word_dispatches"]
            while True:
                if time.monotonic_ns() >= completion_deadline_ns:
                    raise Failure("word dispatch completion did not quiesce", "FAIL_PRODUCT")
                self.guard.check()
                record = self.rpc(surface, "stats", deadline_ns=completion_deadline_ns)
                self.authority(record, foreground=True)
                completed_ns = time.monotonic_ns()
                if completed_ns >= completion_deadline_ns:
                    raise Failure("late dispatch completion cannot authorize undo", "INVALID_OBSERVATION")
                stats = record["stats"]
                if (stats["daemon_epoch"] != stats_before["daemon_epoch"] or
                        not seen_dispatches <= stats["word_dispatches"] <= stats_before["word_dispatches"] + 1):
                    raise Failure("undo dispatch epoch/count drift", "ABORTED_SAFETY")
                seen_dispatches = stats["word_dispatches"]
                if (stats["word_dispatches"] == stats_before["word_dispatches"] + 1 and
                        not any(stats[field] for field in drain_fields)):
                    break
                time.sleep(min(.05, (completion_deadline_ns - completed_ns) / 1e9))
            # The owning event-loop thread answers STATS after complete_word_edit.
            expiry_ns = completed_ns + 2_600_000_000
            while time.monotonic_ns() < expiry_ns:
                time.sleep(min(.05, max(0, (expiry_ns - time.monotonic_ns()) / 1e9)))
        else:
            self.key(surface, "a")
            tail = "a" if group == 0 else "ф"
            changed = self.wait_state(surface, widget, before, corrected + tail, [len(corrected) + 1] * 2, group)
        self.key(surface, "ctrl", "z")
        deadline = time.monotonic() + 5
        while True:
            observed = self.rpc(surface, "snapshot", target=widget)
            if observed["state"]["keys"]["z_up"] > before["state"]["keys"]["z_up"] or time.monotonic() >= deadline:
                break
            time.sleep(0.05)
        self.require_key_count(before, observed, "z", 1)
        # GtkEntry has no native undo binding. Chromium owns its native undo
        # grouping; retain its actual disposition, not a claimed Punto inverse.
        state = observed["state"]
        if surface == "gtk":
            verify_state(before, observed, changed["state"]["text"], changed["state"]["selection"], group)
        else:
            verify_state(before, observed, state["text"], state["selection"], group)
        self.feature_finish(surface, before, observed, native_undo_disposition=state,
                            native_undo_text_asserted=surface == "gtk",
                            native_undo_semantics="EXACT_NOOP" if surface == "gtk" else "OBSERVED_DISPOSITION_ONLY",
                            trigger_started_ns=trigger,
                            **({"dispatch_completed_observed_ns": completed_ns, "undo_expiry_ns": expiry_ns,
                                "dispatch_daemon_epoch": stats["daemon_epoch"], "dispatch_count": stats["word_dispatches"]}
                               if mode == "expiry" else {}))

    def feature_status(self, surface):
        for enabled in (False, True):
            result = self.rpc(surface, "status", enabled=enabled)
            if result["enabled"] != enabled:
                raise Failure("runtime status readback mismatch", "FAIL_PRODUCT")
            widget, before, group = self.feature_start(surface, "auto-on" if enabled else "auto-off-manual-on")
            source, keys, target = ("ghbdtn", "ghbdtn", "привет") if group == 0 else ("руддщ", "hello", "hello")
            self.type_keys(surface, keys)
            self.key(surface, "space")
            self.wait_state(surface, widget, before, (target if enabled else source) + " ",
                            [len(target if enabled else source) + 1] * 2, 1 - group if enabled else group)
            if not enabled:
                self.key(surface, "pause")
                self.wait_state(surface, widget, before, target + " ", [len(target) + 1] * 2, 1 - group)
            self.key(surface, "a")
            observed = self.wait_state(surface, widget, before, target + " " + ("ф" if group == 0 else "a"),
                                       [len(target) + 2] * 2, 1 - group)
            self.feature_finish(surface, before, observed)

    def features(self):
        self.original_status = self.rpc("chromium", "status")["enabled"]
        if type(self.original_status) is not bool:
            raise Failure("original runtime status unavailable")
        if self.output:
            (self.output / "status-recovery.json").write_text(json.dumps({"enabled": self.original_status,
                "daemon": self.authority_identity()["daemon"]}) + "\n")
        primary_error = None
        try:
            for surface in ("gtk", "chromium"):
                for _ in (0, 1):
                    # A physical suffix includes Space: isolate the manual
                    # command from independently configured typo/Shift fixes.
                    self.rpc(surface, "status", enabled=False)
                    for mode in ("case-word", "case-selection", "translit"):
                        self.feature_edit(surface, mode)
                    for mode in ("immediate", "expiry", "new-input"):
                        self.feature_undo(surface, mode)
                    self.rpc(surface, "status", enabled=True)
                    self.feature_tab(surface)
                if surface == "gtk":
                    for _ in (0, 1):
                        self.feature_tab(surface, literal=True)
                self.feature_status(surface)
        except Exception as error:
            primary_error = error
            raise
        finally:
            try:
                result = self.rpc("chromium", "status", enabled=self.original_status)
            except Failure as error:
                if primary_error is None or error.kind == "ABORTED_SAFETY":
                    if primary_error is not None:
                        error.add_note(f"Original feature failure: {type(primary_error).__name__} "
                                       f"[{getattr(primary_error, 'kind', 'unclassified')}]: {primary_error}")
                        raise error from primary_error
                    raise
                primary_error.add_note("Runtime status restoration failed after the original feature failure.")
            else:
                if result["enabled"] != self.original_status:
                    raise Failure("original runtime status not restored", "ABORTED_SAFETY")


def main():
    parser = argparse.ArgumentParser()
    for name in ("vm", "uuid", "host", "key", "known-hosts", "expected-payload-manifest", "output"):
        parser.add_argument("--" + name, required=True)
    parser.add_argument("--soak", action="store_true")
    parser.add_argument("--features", action="store_true")
    parser.add_argument("--terminal", action="store_true")
    parser.add_argument("--lifecycle", action="store_true")
    parser.add_argument("--lifecycle-only", action="store_true")
    parser.add_argument("--desktop-parity", action="store_true")
    parser.add_argument("--profile", choices=("modern", "exact285-gtk16"), default="modern")
    args = parser.parse_args()
    if args.lifecycle_only and (not args.lifecycle or args.profile != "modern" or args.soak):
        raise Failure("--lifecycle-only requires modern --lifecycle without --soak")
    manifest = json.loads(Path(args.expected_payload_manifest).read_text())
    manifest["features"] = args.features
    manifest["terminal_phase"] = args.terminal
    manifest["lifecycle_phase"] = args.lifecycle
    manifest["lifecycle_only"] = args.lifecycle_only
    if args.terminal and not args.lifecycle:
        raise Failure("terminal auto-inverse requires --lifecycle owned store")
    manifest["desktop_parity"] = args.desktop_parity
    manifest["profile"] = args.profile
    manifest["soak"] = args.soak
    legacy = profile_contract(manifest)
    phase_capabilities(manifest)
    if args.uuid != manifest["domain_uuid"] or args.host != manifest["ssh_host"]:
        raise Failure("CLI target differs from pinned manifest")
    user, address = args.host.split("@")
    if user != "punto" or ipaddress.ip_address(address).is_loopback:
        raise Failure("only pinned non-host guest SSH is allowed")
    for field in ("sources", "installed", "processes", "session"):
        if not manifest.get(field):
            raise Failure(f"pinned manifest lacks {field}")
    roles = {"udevmon", "interception", "daemon", "uinput", "gnome", "ibus"}
    if not legacy:
        roles.add("helper")
    if not roles.issubset(manifest["processes"]):
        raise Failure("pinned process map must cover the whole pipeline/helper/Shell/IBus")
    ordinary_environment(manifest["session"])
    for name in (args.key, args.known_hosts):
        if not Path(name).is_file() or Path(name).is_symlink():
            raise Failure("pinned SSH key/known_hosts must be regular files")
    guard = Guard(args.vm, args.uuid)
    guard.check()
    check_files(manifest["sources"])
    check_files({manifest["package"]["path"]: manifest["package"]["sha256"]})
    output = Path(args.output)
    output.mkdir(mode=0o700, parents=True, exist_ok=False)
    manifest["run_id"] = str(uuid.uuid4())
    ssh = ["ssh", "-i", args.key, "-o", "IdentitiesOnly=yes", "-o", "BatchMode=yes", "-o", "ConnectTimeout=5",
           "-o", "StrictHostKeyChecking=yes", "-o", "GlobalKnownHostsFile=/dev/null", "-o", f"UserKnownHostsFile={args.known_hosts}", args.host]
    stage = Path(manifest["staged_dir"])
    if stage.parent != Path("/home/punto") or not stage.name.startswith("punto-acceptance-"):
        raise Failure("staging directory is outside the unique guest scope")
    files = {name: base64.b64encode((Path(__file__).parent / name).read_bytes()).decode()
             for name in ("installed_guest.py", "runtime_facts.py", "gtk_target.py", "chromium_target.html", "probe.py")}
    manifest["observer_assets"] = {name: hashlib.sha256(base64.b64decode(content)).hexdigest() for name, content in files.items()}
    code = "import sys,json,pathlib,base64; v=json.load(sys.stdin); p=pathlib.Path(v['path']); p.mkdir(mode=0o700); [(p/k).write_bytes(base64.b64decode(b)) for k,b in v['files'].items()]"
    channel, runner = None, None
    summary = {"scope": "installed-module-free-gtk-chromium-vertical", "manifest": manifest, "status": "BLOCKED_PRECONDITION"}
    summary["execution_mode"] = "lifecycle-only" if args.lifecycle_only else "full"
    if args.lifecycle_only:
        summary.update(scope="installed-lifecycle-only-diagnostic", skipped_cohorts=["base", "features"])
    if legacy:
        summary["scope"] = "exact285-installed-gtk16-rollback-baseline"
    summary["unaccepted_gates"] = ["active_gnome_source_parity", "same_pid_provider_enablement_recovery", "full_product_delivery"]
    try:
        checked_run(ssh + [shlex.join([manifest["python"], "-c", code])], input=json.dumps({"path": str(stage), "files": files}).encode())
        summary["guest_stderr"] = str(output / "guest-stderr.log")
        channel = Lines(ssh + [shlex.join([manifest["python"], "-u", str(stage / "installed_guest.py"), "--guest"])],
                        stderr_path=output / "guest-stderr.log")
        channel.send(manifest)
        runner = Collector(manifest, guard, channel, output)
        ready = channel.read(timeout=30)
        summary["startup"] = ready
        if not ready.get("ready"):
            raise Failure("guest collector unavailable")
        if legacy:
            runner.legacy()
        for surface in (() if legacy or args.lifecycle_only else ("gtk", "chromium")):
            for mode in ("word", "selection", "auto", "empty", "repeat"):
                for _ in (0, 1):
                    runner.operation(surface, mode=mode)
        if args.features and not args.lifecycle_only:
            feature_start = runner.completed
            runner.features()
            summary["verified_feature_cases"] = runner.completed - feature_start
            summary["unaccepted_gates"] += ["terminal_pty", "learning_persistence_reset", "tray_audio", "rich_clipboard",
                                            "chromium_native_undo_text_semantics"]
        if args.lifecycle:
            if args.lifecycle_only:
                runner.check("gtk")
                original = runner.rpc("gtk", "status")["enabled"]
                if type(original) is not bool:
                    raise Failure("original lifecycle runtime status unavailable")
                runner.original_status = original
                (output / "status-recovery.json").write_text(json.dumps({"enabled": original,
                    "daemon": runner.authority_identity()["daemon"]}) + "\n")
            lifecycle_start = runner.completed
            runner.lifecycle()
            summary["verified_lifecycle_cases"] = runner.completed - lifecycle_start
            summary["verified_terminal_cases"] = runner.lifecycle_terminal_cases
            accepted = {"learning_persistence_reset", "tray_audio"}
            if args.terminal:
                accepted.add("terminal_pty")
            summary["unaccepted_gates"] = [gate for gate in summary["unaccepted_gates"] if gate not in accepted]
        if args.soak:
            summary["soak"] = []
            soak_started = runner.completed
            for block, count in enumerate((68, 68, 64)):
                started = time.monotonic_ns()
                completed_before = runner.completed
                for index in range(count):
                    runner.operation("gtk" if index % 4 < 2 else "chromium")
                    target = started / 1e9 + (index + 1) * 400 / count
                    while time.monotonic() < target:
                        runner.check("gtk" if index % 4 < 2 else "chromium")
                        time.sleep(max(0, min(1, target - time.monotonic())))
                summary["soak"].append({"kind": "active", "start_ns": started, "end_ns": time.monotonic_ns(), "operations": runner.completed - completed_before})
                if block < 2:
                    started = time.monotonic_ns()
                    while time.monotonic_ns() - started < 300_000_000_000:
                        runner.check("chromium", idle=True)
                        time.sleep(1)
                    summary["soak"].append({"kind": "idle", "start_ns": started, "end_ns": time.monotonic_ns()})
            summary["soak_verified_operations"] = runner.completed - soak_started
        check_files(manifest["sources"])
        check_files({manifest["package"]["path"]: manifest["package"]["sha256"]})
        runner.check("gtk" if legacy else "chromium", foreground=True)
        if args.desktop_parity:
            summary["unaccepted_gates"] = [gate for gate in summary["unaccepted_gates"] if gate != "active_gnome_source_parity"]
        summary["status"] = "PASS"
    except Exception as error:
        summary.update(status=getattr(error, "kind", "BLOCKED_PRECONDITION"), error=str(error),
                       notes=list(getattr(error, "__notes__", [])))
    finally:
        if channel:
            try:
                channel.close()
            except Exception as error:
                summary.update(status="ABORTED_SAFETY", cleanup_error=type(error).__name__)
        if runner and runner.original_status is not None:
            try:
                guard.check()
                check_files(manifest["sources"])
                recovery = {"manifest": manifest, "enabled": runner.original_status,
                            "daemon": runner.authority_identity()["daemon"]}
                restored = json.loads(checked_run(ssh + [shlex.join([manifest["python"], str(stage / "installed_guest.py"),
                    "--restore-status"])], timeout=10, input=json.dumps(recovery).encode()))
                if restored != {"enabled": runner.original_status}:
                    raise Failure("independent status recovery readback differs")
                summary["original_status_restored"] = runner.original_status
            except Exception as error:
                summary.update(status="ABORTED_SAFETY", status_cleanup_error=str(error))
        summary["verified_operations"] = runner.completed if runner else 0
        (output / "summary.json").write_text(json.dumps(summary, indent=2, ensure_ascii=False) + "\n")
    return 0 if summary["status"] == "PASS" else 1


def restore_status_main():
    """Independent channel-loss recovery; never launch UI or restart services."""
    request = json.loads(sys.stdin.readline())
    manifest = request["manifest"]
    legacy = profile_contract(manifest)
    if checked_run(["sudo", "-n", "cat", "/sys/class/dmi/id/product_uuid"]).strip().lower() != manifest["domain_uuid"].lower():
        raise Failure("recovery domain UUID mismatch")
    check_files(manifest["installed"])
    check_files({str(Path(__file__).parent / name): digest for name, digest in manifest["observer_assets"].items()})
    pid, start = request["daemon"]
    path = manifest["processes"]["daemon"]
    if (Path(f"/proc/{int(pid)}/stat").read_text().rsplit(")", 1)[1].split()[19] != start or
            checked_run(["sudo", "-n", "readlink", f"/proc/{int(pid)}/exe"]).strip() != str(Path(path).resolve()) or
            checked_run(["sudo", "-n", "sha256sum", f"/proc/{int(pid)}/exe"]).split()[0] != manifest["installed"][path]):
        raise Failure("recovery daemon identity drift")
    if legacy:
        reject_candidate_writers()
        if checked_run(["dpkg-query", "-W", "-f=${db:Status-Abbrev} ${Version}", "punto-switcher"]).strip() != "ii  2.8.5":
            raise Failure("legacy recovery package is not configured exact285")
    wire = {"profile": "exact285-gtk16"} if legacy else {}
    print(json.dumps({"enabled": runtime_status(request["enabled"], expected_pid=int(pid), **wire)}), flush=True)


if __name__ == "__main__":
    if sys.argv[1:2] == ["--exec"]:
        owned_exec(sys.argv[2:])
    elif sys.argv[1:] == ["--gtk"]:
        gtk_main()
    elif sys.argv[1:] == ["--clipboard-reader"]:
        clipboard_reader_main()
    elif sys.argv[1:2] == ["--terminal-reader"] and len(sys.argv) == 4:
        terminal_reader_main(sys.argv[2], sys.argv[3])
    elif sys.argv[1:] == ["--guest"]:
        guest_main()
    elif sys.argv[1:] == ["--restore-status"]:
        restore_status_main()
    else:
        raise SystemExit(main())
