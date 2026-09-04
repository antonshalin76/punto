#!/usr/bin/env python3
"""Hermetic process-level contracts for the Punto daemon lifecycle."""

from __future__ import annotations

import argparse
import ctypes
import errno
import fcntl
import json
import os
import select
import shutil
import signal
import socket
import stat
import struct
import subprocess
import sys
import tempfile
import termios
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import BinaryIO

INIT_TIMEOUT_SECONDS = 20.0
EXIT_TIMEOUT_SECONDS = 10.0
INFO_TIMEOUT_SECONDS = 5.0
IPC_TIMEOUT_SECONDS = 0.5
IPC_FRAME_EXPIRY_SECONDS = 0.250
IPC_FREEZE_BUDGET_SECONDS = 0.200
IPC_RESPONSE_MAX_BYTES = 2048
IPC_SOCKET_NAME = "punto.sock"
SKIP_EXIT_CODE = 77


class ContractSkip(RuntimeError):
    """The host cannot provide the isolation required by this contract."""


class Timeval(ctypes.Structure):
    _fields_ = [("tv_sec", ctypes.c_long), ("tv_usec", ctypes.c_long)]


class InputEvent(ctypes.Structure):
    _fields_ = [
        ("time", Timeval),
        ("type", ctypes.c_uint16),
        ("code", ctypes.c_uint16),
        ("value", ctypes.c_int32),
    ]


@dataclass
class SandboxedDaemon:
    supervisor: subprocess.Popen[bytes]
    supervisor_pidfd: int | None
    sandbox_pid: int
    daemon_pid: int
    daemon_pidfd: int | None
    binary: Path
    socket_fd_baseline: int | None = None


def fail(message: str, stderr: bytes = b"") -> None:
    detail = stderr.decode("utf-8", errors="replace")[-4000:]
    if detail:
        message = f"{message}\nstderr tail:\n{detail}"
    raise AssertionError(message)


def drain_available(stream: object) -> bytes:
    if stream is None:
        return b""
    fd = stream.fileno()  # type: ignore[attr-defined]
    captured = bytearray()
    while True:
        readable, _, _ = select.select([fd], [], [], 0)
        if not readable:
            return bytes(captured)
        chunk = os.read(fd, 4096)
        if not chunk:
            return bytes(captured)
        captured.extend(chunk)


def pidfd_is_alive(pidfd: int | None) -> bool:
    if pidfd is None:
        return False
    readable, _, _ = select.select([pidfd], [], [], 0)
    return not readable


def send_pidfd_signal(pidfd: int | None, sig: signal.Signals) -> bool:
    if pidfd is None:
        return False
    try:
        signal.pidfd_send_signal(pidfd, sig)
        return True
    except ProcessLookupError:
        return False


def close_fd(fd: int | None) -> None:
    if fd is None:
        return
    try:
        os.close(fd)
    except OSError as error:
        if error.errno != errno.EBADF:
            raise


def close_pidfd(pidfd: int | None) -> None:
    close_fd(pidfd)


def close_popen_pipes(supervisor: subprocess.Popen[bytes]) -> None:
    for stream_name in ("stdin", "stdout", "stderr"):
        stream = getattr(supervisor, stream_name)
        if stream is not None and not stream.closed:
            try:
                stream.close()
            except (OSError, ValueError):
                continue


def drain_until_terminal(
    supervisor: subprocess.Popen[bytes],
    timeout: float,
    *,
    drain_stdout: bool = True,
) -> tuple[bool, bytes, bytes]:
    """Wait without touching stdin while preventing stdout/stderr backpressure."""
    if supervisor.poll() is not None:
        return True, b"", b""
    deadline = time.monotonic() + timeout
    stdout = bytearray()
    stderr = bytearray()
    streams: dict[int, tuple[str, BinaryIO]] = {}
    if drain_stdout and supervisor.stdout is not None and not supervisor.stdout.closed:
        streams[supervisor.stdout.fileno()] = ("stdout", supervisor.stdout)
    if supervisor.stderr is not None and not supervisor.stderr.closed:
        streams[supervisor.stderr.fileno()] = ("stderr", supervisor.stderr)

    while supervisor.poll() is None:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return False, bytes(stdout), bytes(stderr)
        if not streams:
            time.sleep(min(0.01, remaining))
            continue
        readable, _, _ = select.select(list(streams), [], [], min(0.05, remaining))
        for fd in readable:
            chunk = os.read(fd, 65536)
            if not chunk:
                streams.pop(fd, None)
            elif streams[fd][0] == "stdout":
                stdout.extend(chunk)
            else:
                stderr.extend(chunk)

    return True, bytes(stdout), bytes(stderr)


def communicate_after_exit(
    supervisor: subprocess.Popen[bytes],
) -> tuple[bytes, bytes]:
    if supervisor.poll() is None:
        raise AssertionError("communicate attempted before terminal process state")
    output_streams = (supervisor.stdout, supervisor.stderr)
    if all(stream is None or stream.closed for stream in output_streams):
        if supervisor.stdin is not None and not supervisor.stdin.closed:
            supervisor.stdin.close()
        return b"", b""
    stdout, stderr = supervisor.communicate(timeout=1.0)
    return stdout or b"", stderr or b""


def write_fixture(path: Path, contents: str, mode: int = 0o644) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(contents, encoding="utf-8")
    path.chmod(mode)


def prepare_sandbox_layout(root: Path) -> None:
    for directory in (
        root / "run",
        root / "home" / "test",
        root / "etc-punto",
        root / "usr-share-hunspell",
        root / "usr-share-dict",
    ):
        directory.mkdir(parents=True, exist_ok=True)

    (root / "run").chmod(0o755)
    write_fixture(root / "passwd", "root:x:0:0:root:/root:/bin/sh\n")
    write_fixture(root / "group", "root:x:0:\npunto:x:0:\n")
    write_fixture(root / "nsswitch.conf", "passwd: files\ngroup: files\n")
    write_fixture(root / "etc-punto" / "runtime-gid", "0\n")


def prepare_fixture(root: Path, daemon: str, with_dictionaries: bool) -> Path:
    prepare_sandbox_layout(root)
    run_dir = root / "run"
    config_dir = root / "etc-punto"
    dictionary_dir = root / "usr-share-dict"
    fixture_dir = run_dir / "fixture"
    fixture_dir.mkdir(parents=True, exist_ok=True)

    binary = fixture_dir / "punto"
    shutil.copy2(daemon, binary)
    binary.chmod(0o755)
    os.mkfifo(fixture_dir / "start", 0o600)

    write_fixture(
        config_dir / "config.yaml",
        """hotkey:
  modifier: leftctrl
  key: grave
auto_switch:
  enabled: true
  threshold: 3.5
  min_word_len: 2
  min_score: 5.0
  max_rollback_words: 5
  typo_correction_enabled: false
  max_typo_diff: 2
  sticky_shift_correction_enabled: true
sound:
  enabled: false
logging:
  level: info
runtime:
  analysis_threads: 1
  max_analysis_threads_per_daemon: 1
""",
    )
    # A privileged service may inherit an arbitrary HOME.  This valid but
    # conflicting per-user snapshot must never override the system config
    # before X11 session discovery establishes the active desktop identity.
    write_fixture(
        root / "home" / "test" / ".config" / "punto" / "config.yaml",
        """hotkey:
  modifier: leftctrl
  key: grave
auto_switch:
  enabled: false
  threshold: 3.5
  min_word_len: 2
  min_score: 5.0
  max_rollback_words: 5
  typo_correction_enabled: false
  max_typo_diff: 2
  sticky_shift_correction_enabled: true
sound:
  enabled: false
logging:
  level: info
runtime:
  analysis_threads: 1
  max_analysis_threads_per_daemon: 1
""",
    )
    if with_dictionaries:
        write_fixture(dictionary_dir / "words", "hello\nworld\n")
        write_fixture(dictionary_dir / "russian", "привет\nмир\n")

    if not binary.is_file() or not os.access(binary, os.X_OK):
        raise AssertionError("sandbox daemon fixture is not executable")
    if stat.S_IMODE(run_dir.stat().st_mode) != 0o755:
        raise AssertionError("sandbox /run fixture has unexpected mode")
    if with_dictionaries and not (dictionary_dir / "words").is_file():
        raise AssertionError("sandbox dictionary fixture is incomplete")
    if "punto:x:0:" not in (root / "group").read_text(encoding="utf-8"):
        raise AssertionError("sandbox punto group fixture is incomplete")
    return binary


def add_ro_bind_if_present(command: list[str], source: str, target: str) -> None:
    if Path(source).exists():
        command.extend(("--ro-bind", source, target))


def sandbox_prefix(root: Path) -> list[str]:
    command = [
        shutil.which("bwrap") or "bwrap",
        "--unshare-user",
        "--uid",
        "0",
        "--gid",
        "0",
        "--unshare-pid",
        "--unshare-net",
        "--unshare-ipc",
        "--unshare-uts",
        "--unshare-cgroup-try",
        "--hostname",
        "punto-contract",
        "--die-with-parent",
        "--new-session",
        "--disable-userns",
        "--proc",
        "/proc",
        "--dev",
        "/dev",
        "--tmpfs",
        "/tmp",
        "--ro-bind",
        "/usr",
        "/usr",
    ]
    for path in ("/bin", "/sbin", "/lib", "/lib64"):
        add_ro_bind_if_present(command, path, path)

    command.extend(("--dir", "/etc"))
    for path in (
        "/etc/alternatives",
        "/etc/ld.so.cache",
        "/etc/ld.so.conf",
        "/etc/ld.so.conf.d",
    ):
        add_ro_bind_if_present(command, path, path)

    command.extend(
        (
            "--ro-bind",
            str(root / "passwd"),
            "/etc/passwd",
            "--ro-bind",
            str(root / "group"),
            "/etc/group",
            "--ro-bind",
            str(root / "nsswitch.conf"),
            "/etc/nsswitch.conf",
            "--ro-bind",
            str(root / "etc-punto"),
            "/etc/punto",
            "--bind",
            str(root / "run"),
            "/run",
            "--dir",
            "/var",
            "--symlink",
            "/run",
            "/var/run",
            "--bind",
            str(root / "home"),
            "/home",
            "--ro-bind",
            str(root / "usr-share-hunspell"),
            "/usr/share/hunspell",
            "--ro-bind",
            str(root / "usr-share-dict"),
            "/usr/share/dict",
            "--clearenv",
            "--setenv",
            "HOME",
            "/home/test",
            "--setenv",
            "PATH",
            "/usr/bin:/bin",
            "--setenv",
            "LC_ALL",
            "C.UTF-8",
            "--setenv",
            "PUNTO_LOG_STDERR",
            "1",
            "--chdir",
            "/",
        )
    )
    return command


def sandbox_command(root: Path, info_fd: int, start_gate: str) -> list[str]:
    return sandbox_prefix(root) + [
        "--setenv",
        "PUNTO_TEST_START_GATE",
        f"/run/fixture/{start_gate}",
        "--info-fd",
        str(info_fd),
        "--",
        "/bin/sh",
        "-ceu",
        'IFS= read -r gate <"$PUNTO_TEST_START_GATE"; exec /run/fixture/punto',
    ]


def read_sandbox_pid(supervisor: subprocess.Popen[bytes], info_fd: int) -> int:
    deadline = time.monotonic() + INFO_TIMEOUT_SECONDS
    payload = bytearray()
    os.set_blocking(info_fd, False)
    while time.monotonic() < deadline:
        readable, _, _ = select.select([info_fd], [], [], 0.05)
        if readable:
            chunk = os.read(info_fd, 4096)
            if not chunk:
                break
            payload.extend(chunk)
            if b"}" in payload:
                break
        if supervisor.poll() is not None:
            break

    try:
        document = json.loads(payload.decode("utf-8"))
        daemon_pid = int(document["child-pid"])
    except (UnicodeDecodeError, ValueError, KeyError, TypeError) as error:
        stderr = drain_available(supervisor.stderr)
        fail(f"bubblewrap did not provide a valid child PID: {error}", stderr)
    if daemon_pid <= 1:
        fail(f"bubblewrap provided an invalid child PID: {daemon_pid}")
    return daemon_pid


def resolve_command_pid(supervisor: subprocess.Popen[bytes], sandbox_pid: int) -> int:
    children_path = Path(f"/proc/{sandbox_pid}/task/{sandbox_pid}/children")
    deadline = time.monotonic() + INFO_TIMEOUT_SECONDS
    while time.monotonic() < deadline:
        if supervisor.poll() is not None:
            fail(
                "bubblewrap exited before starting its command",
                drain_available(supervisor.stderr),
            )
        try:
            children = [int(value) for value in children_path.read_text().split()]
        except (FileNotFoundError, ValueError):
            children = []
        if len(children) == 1:
            return children[0]
        if len(children) > 1:
            fail(f"sandbox init has unexpected children: {children}")
        time.sleep(0.005)
    fail("sandbox command PID handoff timed out", drain_available(supervisor.stderr))


def release_start_gate(supervisor: subprocess.Popen[bytes], gate_path: Path) -> None:
    deadline = time.monotonic() + INFO_TIMEOUT_SECONDS
    while time.monotonic() < deadline:
        if supervisor.poll() is not None:
            fail(
                "sandbox command exited before its start gate",
                drain_available(supervisor.stderr),
            )
        try:
            gate_fd = os.open(gate_path, os.O_WRONLY | os.O_NONBLOCK)
        except OSError as error:
            if error.errno == errno.ENXIO:
                time.sleep(0.005)
                continue
            raise
        try:
            os.write(gate_fd, b"start\n")
        finally:
            os.close(gate_fd)
        return
    fail("sandbox start gate timed out", drain_available(supervisor.stderr))


def spawn_daemon(
    daemon: str,
    root: Path,
    stdout: int | object,
    *,
    with_dictionaries: bool = True,
    reuse_fixture: bool = False,
    start_gate: str = "start",
) -> SandboxedDaemon:
    fixture_dir = root / "run" / "fixture"
    if reuse_fixture:
        if not start_gate.isascii() or not start_gate.replace("-", "").isalnum():
            raise AssertionError(f"invalid sandbox start gate: {start_gate!r}")
        binary = fixture_dir / "punto"
        if not binary.is_file() or not os.access(binary, os.X_OK):
            raise AssertionError("reused sandbox daemon fixture is unavailable")
        os.mkfifo(fixture_dir / start_gate, 0o600)
    else:
        if start_gate != "start":
            raise AssertionError("initial sandbox daemon must use the default gate")
        binary = prepare_fixture(root, daemon, with_dictionaries)
    info_read_fd, info_write_fd = os.pipe2(os.O_CLOEXEC)
    os.set_inheritable(info_write_fd, True)
    supervisor: subprocess.Popen[bytes] | None = None
    supervisor_pidfd: int | None = None
    daemon_pidfd: int | None = None
    try:
        supervisor = subprocess.Popen(
            sandbox_command(root, info_write_fd, start_gate),
            stdin=subprocess.PIPE,
            stdout=stdout,
            stderr=subprocess.PIPE,
            close_fds=True,
            pass_fds=(info_write_fd,),
            restore_signals=True,
        )
        supervisor_pidfd = os.pidfd_open(supervisor.pid, 0)
        os.close(info_write_fd)
        info_write_fd = -1

        sandbox_pid = read_sandbox_pid(supervisor, info_read_fd)
        os.close(info_read_fd)
        info_read_fd = -1
        daemon_pid = resolve_command_pid(supervisor, sandbox_pid)
        daemon_pidfd = os.pidfd_open(daemon_pid, 0)
        if not pidfd_is_alive(daemon_pidfd):
            fail("sandbox command exited before its start gate")
        release_start_gate(supervisor, root / "run" / "fixture" / start_gate)
        return SandboxedDaemon(
            supervisor,
            supervisor_pidfd,
            sandbox_pid,
            daemon_pid,
            daemon_pidfd,
            binary,
        )
    except BaseException:
        if supervisor is not None:
            if daemon_pidfd is not None:
                send_pidfd_signal(daemon_pidfd, signal.SIGKILL)
            elif supervisor.stdin is not None:
                supervisor.stdin.close()
                supervisor.stdin = None
                if supervisor_pidfd is None:
                    try:
                        release_start_gate(
                            supervisor, root / "run" / "fixture" / start_gate
                        )
                    except (AssertionError, OSError) as cleanup_error:
                        _ = cleanup_error
            if supervisor_pidfd is not None:
                send_pidfd_signal(supervisor_pidfd, signal.SIGKILL)
            terminal, _, _ = drain_until_terminal(supervisor, 2.0)
            if terminal:
                communicate_after_exit(supervisor)
            else:
                close_popen_pipes(supervisor)
        close_pidfd(daemon_pidfd)
        close_pidfd(supervisor_pidfd)
        raise
    finally:
        if info_read_fd >= 0:
            os.close(info_read_fd)
        if info_write_fd >= 0:
            os.close(info_write_fd)


def socket_fd_count(pid: int) -> int:
    count = 0
    try:
        entries = list(Path(f"/proc/{pid}/fd").iterdir())
    except FileNotFoundError:
        return 0
    for entry in entries:
        try:
            if os.readlink(entry).startswith("socket:["):
                count += 1
        except FileNotFoundError:
            continue
    return count


def wait_for_stable_socket_baseline(process: SandboxedDaemon) -> int:
    deadline = time.monotonic() + 2.0
    consecutive = 0
    while time.monotonic() < deadline:
        if not pidfd_is_alive(process.daemon_pidfd):
            break
        current = socket_fd_count(process.daemon_pid)
        if current == 1:
            consecutive += 1
            if consecutive == 4:
                return current
        else:
            consecutive = 0
        time.sleep(0.02)
    fail(
        "daemon socket descriptor baseline did not stabilize at one listener "
        f"(last count={socket_fd_count(process.daemon_pid)})"
    )


def wait_for_accepted_client(process: SandboxedDaemon, baseline: int) -> None:
    deadline = time.monotonic() + 2.0
    while time.monotonic() < deadline:
        if socket_fd_count(process.daemon_pid) > baseline:
            return
        if not pidfd_is_alive(process.daemon_pidfd):
            break
        time.sleep(0.005)
    fail("IPC client was not accepted before the signal barrier")


class OutputConsumer:
    def __init__(self, stream: BinaryIO) -> None:
        self._fd = -1
        self._fd_lock = threading.Lock()
        self._stop = threading.Event()
        self._condition = threading.Condition()
        self._captured = bytearray()
        self._error: BaseException | None = None
        self._eof = False
        self._fd_closed = False
        self._started = False
        self._thread = threading.Thread(
            target=self._consume,
            name="punto-contract-output-consumer",
            daemon=True,
        )
        self._fd = os.dup(stream.fileno())
        try:
            os.set_blocking(self._fd, False)
        except BaseException:
            close_fd(self._fd)
            self._fd_closed = True
            raise

    def start(self) -> None:
        try:
            self._thread.start()
            self._started = True
        except BaseException:
            self._close_read_fd()
            raise

    def _close_read_fd(self) -> None:
        with self._fd_lock:
            if self._fd_closed:
                return
            close_fd(self._fd)
            self._fd_closed = True
        with self._condition:
            self._condition.notify_all()

    def _consume(self) -> None:
        try:
            while not self._stop.is_set():
                with self._fd_lock:
                    if self._fd_closed:
                        return
                    readable, _, _ = select.select([self._fd], [], [], 0.05)
                    if not readable:
                        continue
                    try:
                        chunk = os.read(self._fd, 65536)
                    except BlockingIOError:
                        continue
                with self._condition:
                    if not chunk:
                        self._eof = True
                        self._condition.notify_all()
                        return
                    self._captured.extend(chunk)
                    self._condition.notify_all()
        except OSError as error:
            with self._condition:
                self._error = error
                self._condition.notify_all()
        finally:
            self._close_read_fd()
            with self._condition:
                self._condition.notify_all()

    def offset(self) -> int:
        with self._condition:
            return len(self._captured)

    def wait_for(self, expected: bytes, offset: int, timeout: float = 2.0) -> None:
        deadline = time.monotonic() + timeout
        with self._condition:
            while True:
                if self._error is not None:
                    raise AssertionError(
                        f"stdout consumer failed: {self._error}"
                    ) from self._error
                if self._captured.find(expected, offset) >= 0:
                    return
                if self._eof:
                    fail("daemon stdout closed before the passthrough barrier")
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    fail(
                        "several complete input_event frames did not cross the "
                        "stdout passthrough barrier"
                    )
                self._condition.wait(timeout=remaining)

    def join(self, timeout: float = 2.0) -> None:
        if not self._started:
            self._close_read_fd()
            return
        self._thread.join(timeout=timeout)
        if self._thread.is_alive():
            self.stop()
            fail("stdout consumer did not terminate after daemon exit")
        if not self._fd_closed:
            fail("stdout consumer terminated without closing its read fd")
        if self._error is not None:
            raise AssertionError(
                f"stdout consumer failed: {self._error}"
            ) from self._error

    def stop(self, timeout: float = 0.5) -> None:
        self._stop.set()
        with self._condition:
            self._condition.notify_all()
        if not self._started:
            self._close_read_fd()
            return
        self._thread.join(timeout=timeout)
        if self._thread.is_alive():
            self._close_read_fd()
            self._thread.join(timeout=0.1)
        if self._thread.is_alive():
            fail("stdout consumer ignored its bounded stop request")
        if not self._fd_closed:
            fail("stdout consumer stop did not close its read fd")


def close_process_pidfds(process: SandboxedDaemon) -> None:
    close_pidfd(process.daemon_pidfd)
    close_pidfd(process.supervisor_pidfd)
    process.daemon_pidfd = None
    process.supervisor_pidfd = None


def cleanup_process(
    process: SandboxedDaemon,
    *,
    output_consumer: OutputConsumer | None = None,
) -> None:
    try:
        if pidfd_is_alive(process.daemon_pidfd):
            send_pidfd_signal(process.daemon_pidfd, signal.SIGKILL)

        terminal, _, _ = drain_until_terminal(
            process.supervisor,
            2.0,
            drain_stdout=output_consumer is None,
        )
        if not terminal and pidfd_is_alive(process.supervisor_pidfd):
            send_pidfd_signal(process.supervisor_pidfd, signal.SIGKILL)
            terminal, _, _ = drain_until_terminal(
                process.supervisor,
                2.0,
                drain_stdout=output_consumer is None,
            )
        if not terminal:
            try:
                if output_consumer is not None:
                    output_consumer.stop()
            finally:
                close_popen_pipes(process.supervisor)
            fail("sandbox supervisor was not reaped after pidfd SIGKILL")
        if output_consumer is not None:
            try:
                output_consumer.join()
            finally:
                communicate_after_exit(process.supervisor)
        else:
            communicate_after_exit(process.supervisor)
    finally:
        close_process_pidfds(process)


def read_ipc_response(client: socket.socket) -> bytes:
    response = bytearray()
    while b"\n" not in response:
        chunk = client.recv(512)
        if not chunk:
            break
        response.extend(chunk)
        if len(response) > IPC_RESPONSE_MAX_BYTES:
            fail(f"IPC response exceeded {IPC_RESPONSE_MAX_BYTES} bytes")
    return bytes(response)


def ipc_request(socket_path: Path, command: bytes) -> bytes:
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(IPC_TIMEOUT_SECONDS)
        client.connect(str(socket_path))
        client.sendall(command)
        return read_ipc_response(client)


def parse_stats(response: bytes) -> dict[str, str]:
    try:
        line = response.decode("ascii", errors="strict")
    except UnicodeDecodeError as error:
        fail(f"STATS response is not ASCII: {error}")
    if not line.endswith("\n") or line.count("\n") != 1 or "\r" in line:
        fail(f"STATS response is not one LF-terminated line: {response!r}")
    tokens = line[:-1].split(" ")
    if not tokens or tokens[0] != "OK" or any(not token for token in tokens):
        fail(f"STATS response framing is invalid: {response!r}")
    fields: dict[str, str] = {}
    for token in tokens[1:]:
        if token.count("=") != 1:
            fail(f"STATS token is invalid: {token!r}")
        key, value = token.split("=", 1)
        if not key or not value or key in fields:
            fail(f"STATS field is invalid or duplicated: {token!r}")
        fields[key] = value
    required_prefix = (
        "x11_health",
        "analysis_health",
        "input_health",
        "x11_last_progress_ms",
        "analysis_last_progress_ms",
        "input_last_progress_ms",
        "analysis_outstanding",
        "input_in_flight",
    )
    actual_prefix = tuple(token.split("=", 1)[0] for token in tokens[1:9])
    if actual_prefix != required_prefix:
        fail(f"STATS fixed field order mismatch: {actual_prefix!r}")
    if "log_dropped" not in fields or not fields["log_dropped"].isdecimal():
        fail(f"STATS log_dropped is missing or invalid: {fields!r}")
    return fields


def wait_until_ready(process: SandboxedDaemon, socket_path: Path) -> bytes:
    stderr_stream = process.supervisor.stderr
    assert stderr_stream is not None
    deadline = time.monotonic() + INIT_TIMEOUT_SECONDS
    captured = bytearray()
    last_ipc_error = "socket not created"

    while time.monotonic() < deadline:
        captured.extend(drain_available(stderr_stream))
        if process.supervisor.poll() is not None:
            captured.extend(stderr_stream.read() or b"")
            fail(
                f"daemon exited during initialization: {process.supervisor.returncode}",
                bytes(captured),
            )

        try:
            response = ipc_request(socket_path, b"GET_STATUS\n")
            if response != b"OK DISABLED\n":
                fail(f"unexpected readiness response: {response!r}")

            endpoint = socket_path.lstat()
            if not stat.S_ISSOCK(endpoint.st_mode):
                fail(f"runtime endpoint is not a Unix socket: {socket_path}")
            if stat.S_IMODE(endpoint.st_mode) != 0o660:
                fail(
                    "runtime endpoint mode must be 0660, got "
                    f"{stat.S_IMODE(endpoint.st_mode):04o}"
                )
            process.socket_fd_baseline = wait_for_stable_socket_baseline(process)

            actual_executable = Path(f"/proc/{process.daemon_pid}/exe")
            if not actual_executable.exists() or not os.path.samefile(
                actual_executable, process.binary
            ):
                fail(
                    "sandbox child PID does not identify the daemon fixture",
                    bytes(captured),
                )
            captured.extend(drain_available(stderr_stream))
            return bytes(captured)
        except OSError as error:
            last_ipc_error = f"{type(error).__name__}: {error}"

        select.select([stderr_stream.fileno()], [], [], 0.05)

    cleanup_process(process)
    fail(
        f"daemon readiness timed out ({last_ipc_error})",
        bytes(captured),
    )


def wait_for_stderr(
    process: SandboxedDaemon,
    captured: bytes,
    expected: bytes,
    timeout: float = 10.0,
) -> bytes:
    stream = process.supervisor.stderr
    assert stream is not None
    collected = bytearray(captured)
    offset = len(collected)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        collected.extend(drain_available(stream))
        if collected.find(expected, offset) >= 0:
            return bytes(collected)
        if process.supervisor.poll() is not None:
            collected.extend(stream.read() or b"")
            fail(
                f"daemon exited before stderr marker {expected!r}",
                bytes(collected),
            )
        select.select([stream.fileno()], [], [], 0.05)
    fail(f"stderr marker timed out: {expected!r}", bytes(collected))


def read_control_plane_state(path: Path) -> dict[str, str]:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except (FileNotFoundError, UnicodeDecodeError):
        return {}
    if len(lines) != 4 or any("=" not in line for line in lines):
        return {}
    return dict(line.split("=", 1) for line in lines)


def wait_for_control_plane_state(
    path: Path,
    expected_path: str,
    minimum_generation: int,
    processes: tuple[SandboxedDaemon, ...],
    timeout: float = 10.0,
) -> dict[str, str]:
    deadline = time.monotonic() + timeout
    last: dict[str, str] = {}
    while time.monotonic() < deadline:
        for process in processes:
            if not pidfd_is_alive(process.daemon_pidfd):
                fail("daemon exited while waiting for control-plane state")
        last = read_control_plane_state(path)
        try:
            generation = int(last.get("config_generation", "-1"))
        except ValueError:
            generation = -1
        if last.get("config_path") == expected_path and generation >= minimum_generation:
            return last
        time.sleep(0.02)
    fail(
        "control-plane state did not reach "
        f"path={expected_path!r}, generation>={minimum_generation}; last={last!r}"
    )


def wait_for_exit(
    process: SandboxedDaemon,
    socket_path: Path,
    captured: bytes,
    expected_code: int,
    description: str,
    *,
    output_consumer: OutputConsumer | None = None,
) -> bytes:
    terminal, _, drained_stderr = drain_until_terminal(
        process.supervisor,
        EXIT_TIMEOUT_SECONDS,
        drain_stdout=output_consumer is None,
    )
    stderr = captured + drained_stderr
    if not terminal:
        cleanup_process(process, output_consumer=output_consumer)
        fail(f"{description} did not complete bounded shutdown", stderr)

    if output_consumer is not None:
        output_consumer.join()
    _, tail = communicate_after_exit(process.supervisor)

    stderr += tail or b""
    returncode = process.supervisor.returncode
    daemon_alive = pidfd_is_alive(process.daemon_pidfd)
    close_process_pidfds(process)
    if returncode is None:
        fail(f"{description} has no exit status", stderr)
    if returncode != expected_code:
        fail(
            f"{description} exit must be {expected_code}, got {returncode}",
            stderr,
        )
    if daemon_alive:
        fail(f"{description} left daemon process {process.daemon_pid} alive", stderr)
    if socket_path.exists() or socket_path.is_symlink():
        fail(f"{description} left owned socket behind: {socket_path}", stderr)
    return stderr


def assert_open_stdin_keeps_daemon_alive(
    process: SandboxedDaemon,
    captured: bytes,
    duration: float = 0.25,
) -> bytes:
    if process.supervisor.stdin is None or process.supervisor.stdin.closed:
        fail("negative control requires an open stdin stream", captured)

    deadline = time.monotonic() + duration
    stderr = bytearray(captured)
    while time.monotonic() < deadline:
        stderr.extend(drain_available(process.supervisor.stderr))
        if process.supervisor.stdout is not None:
            drain_available(process.supervisor.stdout)
        if process.supervisor.poll() is not None or not pidfd_is_alive(
            process.daemon_pidfd
        ):
            fail(
                "daemon exited while the negative-control stdin stayed open",
                bytes(stderr),
            )
        time.sleep(0.01)
    return bytes(stderr)


def close_stdin(process: SandboxedDaemon) -> None:
    stream = process.supervisor.stdin
    if stream is None:
        return
    stream.close()
    process.supervisor.stdin = None


def pack_event(event_type: int, code: int, value: int) -> bytes:
    event = InputEvent(Timeval(0, 0), event_type, code, value)
    return bytes(event)


def passthrough_barrier_frames() -> bytes:
    events = (
        InputEvent(Timeval(101, 1001), 1, 34, 1),
        InputEvent(Timeval(102, 1002), 0, 0, 0),
        InputEvent(Timeval(103, 1003), 1, 34, 0),
        InputEvent(Timeval(104, 1004), 0, 0, 0),
    )
    return b"".join(bytes(event) for event in events)


def write_atomic_bundle(fd: int, payload: bytes) -> None:
    deadline = time.monotonic() + 2.0
    while time.monotonic() < deadline:
        try:
            written = os.write(fd, payload)
        except BlockingIOError:
            time.sleep(0.001)
            continue
        if written != len(payload):
            fail(
                "atomic input-event bundle was partially written "
                f"({written}/{len(payload)} bytes)"
            )
        return
    fail("atomic input-event bundle write timed out")


def write_fragmented(fd: int, payload: bytes) -> None:
    boundaries = (1, 4, 9, len(payload))
    offset = 0
    for boundary in boundaries:
        chunk = payload[offset:boundary]
        if not chunk:
            continue
        written = os.write(fd, chunk)
        if written != len(chunk):
            fail(
                "fragmented input_event write was partial "
                f"({written}/{len(chunk)} bytes)"
            )
        offset = boundary
        time.sleep(0.01)
    if offset != len(payload):
        fail("fragmented input_event helper did not write the complete frame")


def pipe_pending_bytes(fd: int) -> int:
    packed = fcntl.ioctl(fd, termios.FIONREAD, struct.pack("I", 0))
    return int(struct.unpack("I", packed)[0])


def wait_until_pipe_consumed(process: SandboxedDaemon, fd: int) -> None:
    deadline = time.monotonic() + 2.0
    while time.monotonic() < deadline:
        if not pidfd_is_alive(process.daemon_pidfd):
            fail("daemon exited before consuming the partial input_event")
        if pipe_pending_bytes(fd) == 0:
            return
        time.sleep(0.005)
    fail("daemon did not consume the partial input_event before SIGTERM")


def activity_frames() -> bytes:
    frames = bytearray()
    for code in (34, 35, 48, 32, 20, 49, 57):
        frames.extend(pack_event(1, code, 1))
        frames.extend(pack_event(0, 0, 0))
        frames.extend(pack_event(1, code, 0))
        frames.extend(pack_event(0, 0, 0))
    return bytes(frames)


def test_initialization_failure_exit_code(daemon: str) -> None:
    with tempfile.TemporaryDirectory(prefix="punto-init-failure-") as directory:
        root = Path(directory)
        socket_path = root / "run" / IPC_SOCKET_NAME
        process = spawn_daemon(
            daemon, root, subprocess.DEVNULL, with_dictionaries=False
        )
        try:
            if process.supervisor.stdin is None or process.supervisor.stdin.closed:
                fail("initialization failure was not tested with open stdin")
            stderr = wait_for_exit(process, socket_path, b"", 2, "missing dictionaries")
            fatal_lines = [line for line in stderr.splitlines() if b"FATAL:" in line]
            expected_fatal = [
                b"[punto] FATAL: dictionary initialization failed: no-source"
            ]
            if fatal_lines != expected_fatal:
                fail(
                    f"initialization fatal diagnostics must be exact: {fatal_lines!r}",
                    stderr,
                )
            forbidden = (str(root).encode("utf-8"),)
            leaked = [token for token in forbidden if token in stderr]
            if leaked:
                fail(
                    f"initialization failure leaked lifecycle data: {leaked!r}", stderr
                )
            if socket_path.exists() or socket_path.is_symlink():
                fail(
                    "initialization failure created or retained its IPC socket", stderr
                )
        finally:
            cleanup_process(process)


def test_command_line_contract(daemon: str) -> None:
    version = subprocess.run(
        [daemon, "--version"],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=2.0,
        check=False,
    )
    if version.returncode != 0 or "Punto Switcher" not in version.stdout:
        fail(
            "--version is not a successful side-effect-free query",
            version.stderr.encode(),
        )

    invalid = subprocess.run(
        [daemon, "--definitely-invalid"],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=2.0,
        check=False,
    )
    if invalid.returncode != 2:
        fail(f"unknown option returned {invalid.returncode}, expected 2")
    if "Unknown option" not in invalid.stderr or "Использование:" not in invalid.stderr:
        fail("unknown option has no deterministic usage diagnostic")
    if invalid.stdout:
        fail("unknown option unexpectedly wrote to stdout")


def test_sigterm_clean_exit(daemon: str) -> None:
    with tempfile.TemporaryDirectory(prefix="punto-sigterm-") as directory:
        root = Path(directory)
        socket_path = root / "run" / IPC_SOCKET_NAME
        process = spawn_daemon(daemon, root, subprocess.DEVNULL)
        try:
            captured = wait_until_ready(process, socket_path)
            captured = assert_open_stdin_keeps_daemon_alive(process, captured)
            if not send_pidfd_signal(process.daemon_pidfd, signal.SIGTERM):
                fail("daemon exited before SIGTERM negative-control release", captured)
            wait_for_exit(process, socket_path, captured, 0, "SIGTERM")
        finally:
            cleanup_process(process)


def test_service_home_is_not_startup_config_authority(daemon: str) -> None:
    with tempfile.TemporaryDirectory(prefix="punto-startup-authority-") as directory:
        root = Path(directory)
        socket_path = root / "run" / IPC_SOCKET_NAME
        process = spawn_daemon(daemon, root, subprocess.DEVNULL)
        try:
            captured = wait_until_ready(process, socket_path)
            response = ipc_request(socket_path, b"GET_STATUS\n")
            if response != b"OK DISABLED\n":
                fail(
                    f"safe-mode effective status is not disabled: {response!r}",
                    captured,
                )
            fields = parse_stats(ipc_request(socket_path, b"STATS\n"))
            if (
                fields.get("text_mutation") != "disabled"
                or fields.get("enabled") != "0"
            ):
                fail(f"safe-mode STATS capability is inconsistent: {fields!r}")
            if fields.get("configured_enabled") != "1":
                fail(
                    "service HOME config overrode the system analysis intent: "
                    f"{fields!r}",
                    captured,
                )
            if not send_pidfd_signal(process.daemon_pidfd, signal.SIGTERM):
                fail("daemon exited before startup-authority cleanup", captured)
            wait_for_exit(
                process,
                socket_path,
                captured,
                0,
                "startup config authority",
            )
        finally:
            cleanup_process(process)


def test_control_plane_promotion_reconciles_authoritative_snapshot(
    daemon: str,
) -> None:
    with tempfile.TemporaryDirectory(prefix="punto-control-failover-") as directory:
        root = Path(directory)
        primary_socket = root / "run" / IPC_SOCKET_NAME
        secondary_socket = root / "run" / "punto-2.sock"
        state_path = root / "run" / "punto-control.state"
        processes: list[SandboxedDaemon] = []
        primary_reaped = False
        try:
            primary = spawn_daemon(daemon, root, subprocess.DEVNULL)
            processes.append(primary)
            primary_stderr = wait_until_ready(primary, primary_socket)
            initial_state = wait_for_control_plane_state(
                state_path,
                "/etc/punto/config.yaml",
                1,
                (primary,),
            )
            initial_generation = int(initial_state["config_generation"])

            stale_secondary = spawn_daemon(
                daemon,
                root,
                subprocess.DEVNULL,
                reuse_fixture=True,
                start_gate="secondary-stale",
            )
            processes.append(stale_secondary)
            stale_stderr = wait_until_ready(stale_secondary, secondary_socket)
            if b'Configuration reloaded: "/etc/punto/config.yaml"' not in stale_stderr:
                fail("stale secondary did not apply the initial state", stale_stderr)
            if not send_pidfd_signal(stale_secondary.daemon_pidfd, signal.SIGSTOP):
                fail("stale secondary exited before its promotion barrier")

            synced_peer = spawn_daemon(
                daemon,
                root,
                subprocess.DEVNULL,
                reuse_fixture=True,
                start_gate="secondary-synced",
            )
            processes.append(synced_peer)
            peer_stderr = wait_for_stderr(
                synced_peer,
                b"",
                b"secondary diagnostic IPC server failed to start",
            )

            alternate = (
                root / "etc-punto" / "config.yaml"
            ).read_text(encoding="utf-8").replace(
                "enabled: true", "enabled: false", 1
            )
            write_fixture(root / "etc-punto" / "alternate.yaml", alternate)
            response = ipc_request(
                primary_socket, b"RELOAD /etc/punto/alternate.yaml\n"
            )
            if response != b"OK Scheduled\n":
                fail(f"primary failed to schedule alternate config: {response!r}")

            committed_state = wait_for_control_plane_state(
                state_path,
                "/etc/punto/alternate.yaml",
                initial_generation + 1,
                (primary, stale_secondary, synced_peer),
            )
            committed_generation = int(committed_state["config_generation"])
            peer_stderr = wait_for_stderr(
                synced_peer,
                peer_stderr,
                b'Configuration reloaded: "/etc/punto/alternate.yaml"',
            )
            if not send_pidfd_signal(synced_peer.daemon_pidfd, signal.SIGSTOP):
                fail("synced peer exited before the failover barrier")

            if not send_pidfd_signal(primary.daemon_pidfd, signal.SIGKILL):
                fail("primary exited before the failover crash point", primary_stderr)
            terminal, _, primary_tail = drain_until_terminal(
                primary.supervisor, 3.0
            )
            if not terminal:
                fail("crashed primary supervisor was not reaped", primary_tail)
            communicate_after_exit(primary.supervisor)
            close_process_pidfds(primary)
            primary_reaped = True

            if not send_pidfd_signal(stale_secondary.daemon_pidfd, signal.SIGCONT):
                fail("stale secondary exited before promotion")
            promoted_state = wait_for_control_plane_state(
                state_path,
                "/etc/punto/alternate.yaml",
                committed_generation + 1,
                (stale_secondary, synced_peer),
            )
            promoted_generation = int(promoted_state["config_generation"])
            if promoted_generation <= committed_generation:
                fail(
                    "promoted generation did not advance strictly: "
                    f"{promoted_generation} <= {committed_generation}"
                )

            deadline = time.monotonic() + 5.0
            last_error = "promoted primary socket was unavailable"
            while time.monotonic() < deadline:
                try:
                    fields = parse_stats(ipc_request(primary_socket, b"STATS\n"))
                    if (
                        fields.get("control_plane") == "primary"
                        and fields.get("configured_enabled") == "0"
                    ):
                        break
                    last_error = f"unexpected promoted state: {fields!r}"
                except OSError as error:
                    last_error = f"{type(error).__name__}: {error}"
                time.sleep(0.02)
            else:
                fail(last_error, stale_stderr)

            peer_offset = len(peer_stderr)
            if not send_pidfd_signal(synced_peer.daemon_pidfd, signal.SIGCONT):
                fail("synced peer exited before generation observation")
            peer_stderr = wait_for_stderr(
                synced_peer,
                peer_stderr,
                b'Configuration reloaded: "/etc/punto/alternate.yaml"',
            )
            if peer_stderr.find(
                b'Configuration reloaded: "/etc/punto/alternate.yaml"',
                peer_offset,
            ) < 0:
                fail("synced peer did not observe the promoted generation", peer_stderr)
        finally:
            for process in reversed(processes):
                if process is primary and primary_reaped:
                    continue
                if pidfd_is_alive(process.daemon_pidfd):
                    send_pidfd_signal(process.daemon_pidfd, signal.SIGCONT)
                cleanup_process(process)


def test_control_plane_promotion_fails_closed_without_valid_config(
    daemon: str,
) -> None:
    with tempfile.TemporaryDirectory(prefix="punto-control-fail-closed-") as directory:
        root = Path(directory)
        primary_socket = root / "run" / IPC_SOCKET_NAME
        secondary_socket = root / "run" / "punto-2.sock"
        state_path = root / "run" / "punto-control.state"
        processes: list[SandboxedDaemon] = []
        primary_reaped = False
        try:
            primary = spawn_daemon(daemon, root, subprocess.DEVNULL)
            processes.append(primary)
            primary_stderr = wait_until_ready(primary, primary_socket)
            initial_state = wait_for_control_plane_state(
                state_path, "/etc/punto/config.yaml", 1, (primary,)
            )

            stale_secondary = spawn_daemon(
                daemon,
                root,
                subprocess.DEVNULL,
                reuse_fixture=True,
                start_gate="secondary-fail-closed",
            )
            processes.append(stale_secondary)
            stale_stderr = wait_until_ready(stale_secondary, secondary_socket)
            if b'Configuration reloaded: "/etc/punto/config.yaml"' not in stale_stderr:
                fail("fail-closed secondary did not apply initial state", stale_stderr)
            if not send_pidfd_signal(stale_secondary.daemon_pidfd, signal.SIGSTOP):
                fail("fail-closed secondary exited before its barrier")

            alternate = (
                root / "etc-punto" / "config.yaml"
            ).read_text(encoding="utf-8").replace(
                "enabled: true", "enabled: false", 1
            )
            alternate_path = root / "etc-punto" / "alternate.yaml"
            write_fixture(alternate_path, alternate)
            response = ipc_request(
                primary_socket, b"RELOAD /etc/punto/alternate.yaml\n"
            )
            if response != b"OK Scheduled\n":
                fail(f"primary failed to commit fail-closed fixture: {response!r}")
            committed_state = wait_for_control_plane_state(
                state_path,
                "/etc/punto/alternate.yaml",
                int(initial_state["config_generation"]) + 1,
                (primary, stale_secondary),
            )
            committed_generation = committed_state["config_generation"]
            alternate_path.unlink()
            (root / "etc-punto" / "config.yaml").unlink()

            if not send_pidfd_signal(primary.daemon_pidfd, signal.SIGKILL):
                fail("primary exited before fail-closed crash point", primary_stderr)
            terminal, _, primary_tail = drain_until_terminal(
                primary.supervisor, 3.0
            )
            if not terminal:
                fail("fail-closed primary supervisor was not reaped", primary_tail)
            communicate_after_exit(primary.supervisor)
            close_process_pidfds(primary)
            primary_reaped = True

            if not send_pidfd_signal(stale_secondary.daemon_pidfd, signal.SIGCONT):
                fail("fail-closed secondary exited before promotion attempt")
            stale_stderr = wait_for_stderr(
                stale_secondary, stale_stderr, b"Config reload failed"
            )
            stale_stderr = wait_for_stderr(
                stale_secondary, stale_stderr, b"Config reload failed"
            )
            observed = read_control_plane_state(state_path)
            if observed != committed_state:
                fail(
                    "failed reconciliation changed the committed state: "
                    f"{observed!r} != {committed_state!r}",
                    stale_stderr,
                )
            if observed.get("config_generation") != committed_generation:
                fail("failed reconciliation bumped the generation", stale_stderr)
            try:
                response = ipc_request(primary_socket, b"STATS\n")
            except OSError:
                response = b""
            if response:
                fail(
                    f"failed reconciliation exposed primary IPC: {response!r}",
                    stale_stderr,
                )
        finally:
            for process in reversed(processes):
                if process is primary and primary_reaped:
                    continue
                if pidfd_is_alive(process.daemon_pidfd):
                    send_pidfd_signal(process.daemon_pidfd, signal.SIGCONT)
                cleanup_process(process)


def test_stdin_eof_pollhup_clean_exit(daemon: str) -> None:
    with tempfile.TemporaryDirectory(prefix="punto-stdin-eof-") as directory:
        root = Path(directory)
        socket_path = root / "run" / IPC_SOCKET_NAME
        process = spawn_daemon(daemon, root, subprocess.DEVNULL)
        try:
            captured = wait_until_ready(process, socket_path)
            close_stdin(process)
            wait_for_exit(process, socket_path, captured, 0, "stdin EOF/POLLHUP")
        finally:
            cleanup_process(process)


def test_input_health_tracks_only_accepted_frame_stalls(daemon: str) -> None:
    with tempfile.TemporaryDirectory(prefix="punto-input-health-") as directory:
        root = Path(directory)
        socket_path = root / "run" / IPC_SOCKET_NAME
        process = spawn_daemon(daemon, root, subprocess.PIPE)
        output_consumer: OutputConsumer | None = None
        try:
            output = process.supervisor.stdout
            stream = process.supervisor.stdin
            assert output is not None and stream is not None
            output_consumer = OutputConsumer(output)
            output_consumer.start()
            captured = wait_until_ready(process, socket_path)

            initial = parse_stats(ipc_request(socket_path, b"STATS\n"))
            if initial["input_health"] != "ready" or initial["input_in_flight"] != "0":
                fail(f"idle input must start ready with no frame: {initial!r}")

            event = pack_event(4, 4, 123456)
            offset = output_consumer.offset()
            write_atomic_bundle(stream.fileno(), event)
            output_consumer.wait_for(event, offset)

            time.sleep(2.05)
            stalled = parse_stats(ipc_request(socket_path, b"STATS\n"))
            if (
                stalled["input_health"] != "degraded"
                or stalled["input_in_flight"] != "1"
            ):
                fail(f"accepted frame must degrade at the stall boundary: {stalled!r}")

            syn = pack_event(0, 0, 0)
            offset = output_consumer.offset()
            write_atomic_bundle(stream.fileno(), syn)
            output_consumer.wait_for(syn, offset)
            recovered = parse_stats(ipc_request(socket_path, b"STATS\n"))
            if (
                recovered["input_health"] != "ready"
                or recovered["input_in_flight"] != "0"
            ):
                fail(f"SYN_REPORT commit must recover input health: {recovered!r}")
            if int(recovered["input_last_progress_ms"]) <= int(
                initial["input_last_progress_ms"]
            ):
                fail("input progress timestamp did not advance at frame commit")

            if not send_pidfd_signal(process.daemon_pidfd, signal.SIGTERM):
                fail("daemon exited before input-health cleanup", captured)
            wait_for_exit(
                process,
                socket_path,
                captured,
                0,
                "input health cleanup",
                output_consumer=output_consumer,
            )
            output_consumer = None
        finally:
            cleanup_process(process, output_consumer=output_consumer)


def test_fragmented_frame_and_partial_frame_sigterm(daemon: str) -> None:
    with tempfile.TemporaryDirectory(prefix="punto-partial-frame-") as directory:
        root = Path(directory)
        socket_path = root / "run" / IPC_SOCKET_NAME
        process = spawn_daemon(daemon, root, subprocess.PIPE)
        output_consumer: OutputConsumer | None = None
        try:
            output = process.supervisor.stdout
            assert output is not None
            output_consumer = OutputConsumer(output)
            output_consumer.start()
            captured = wait_until_ready(process, socket_path)
            stream = process.supervisor.stdin
            assert stream is not None
            input_fd = stream.fileno()

            complete = bytes(InputEvent(Timeval(201, 2001), 4, 4, 123456))
            offset = output_consumer.offset()
            write_fragmented(input_fd, complete)
            output_consumer.wait_for(complete, offset)

            incomplete = bytes(InputEvent(Timeval(202, 2002), 4, 4, 654321))
            partial = incomplete[: len(incomplete) // 2]
            written = os.write(input_fd, partial)
            if written != len(partial):
                fail("partial input_event setup write was itself partial")
            wait_until_pipe_consumed(process, input_fd)
            if process.supervisor.stdin is None or process.supervisor.stdin.closed:
                fail("partial-frame SIGTERM contract requires open stdin")
            if not send_pidfd_signal(process.daemon_pidfd, signal.SIGTERM):
                fail("daemon exited before partial-frame SIGTERM", captured)

            wait_for_exit(
                process,
                socket_path,
                captured,
                0,
                "partial input_event SIGTERM",
                output_consumer=output_consumer,
            )
        finally:
            cleanup_process(process, output_consumer=output_consumer)
            if output_consumer is not None:
                output_consumer.join()


def wait_until_stopped(process: SandboxedDaemon) -> None:
    deadline = time.monotonic() + 2.0
    status_path = Path(f"/proc/{process.daemon_pid}/status")
    while time.monotonic() < deadline:
        if not pidfd_is_alive(process.daemon_pidfd):
            break
        try:
            for line in status_path.read_text(encoding="ascii").splitlines():
                if line.startswith("State:") and "T" in line.split()[1]:
                    return
        except FileNotFoundError:
            break
        time.sleep(0.01)
    fail("daemon did not enter SIGSTOP barrier")


def test_coalesced_sigterm_burst_with_active_runtime(daemon: str) -> None:
    with tempfile.TemporaryDirectory(prefix="punto-signal-race-") as directory:
        root = Path(directory)
        socket_path = root / "run" / IPC_SOCKET_NAME
        process = spawn_daemon(daemon, root, subprocess.PIPE)
        output_consumer: OutputConsumer | None = None
        slow_client: socket.socket | None = None
        feeder_stop = threading.Event()
        feeder_started = threading.Event()
        feeder_errors: list[BaseException] = []
        feeder: threading.Thread | None = None
        try:
            output = process.supervisor.stdout
            assert output is not None
            output_consumer = OutputConsumer(output)
            output_consumer.start()
            captured = wait_until_ready(process, socket_path)
            stream = process.supervisor.stdin
            assert stream is not None
            input_fd = stream.fileno()
            os.set_blocking(input_fd, False)
            frames = activity_frames()

            def feed_input() -> None:
                while not feeder_stop.is_set():
                    try:
                        if os.write(input_fd, frames) > 0:
                            feeder_started.set()
                            time.sleep(0.001)
                    except BlockingIOError:
                        time.sleep(0.001)
                    except (BrokenPipeError, OSError) as error:
                        if not feeder_stop.is_set():
                            feeder_errors.append(error)
                        return

            feeder = threading.Thread(
                target=feed_input,
                name="punto-contract-input-feeder",
                daemon=False,
            )
            feeder.start()
            if not feeder_started.wait(timeout=2.0):
                fail("active-input barrier was not reached", captured)

            baseline_socket_fds = process.socket_fd_baseline
            if baseline_socket_fds is None:
                fail("readiness did not establish a socket descriptor baseline")

            marker = passthrough_barrier_frames()
            marker_offset = output_consumer.offset()
            write_atomic_bundle(input_fd, marker)
            output_consumer.wait_for(marker, marker_offset)

            client_window_started = time.monotonic()
            slow_client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            slow_client.settimeout(IPC_TIMEOUT_SECONDS)
            slow_client.connect(str(socket_path))
            wait_for_accepted_client(process, baseline_socket_fds)
            if not send_pidfd_signal(process.daemon_pidfd, signal.SIGSTOP):
                fail("daemon exited before the SIGSTOP barrier", captured)
            wait_until_stopped(process)
            frozen_elapsed = time.monotonic() - client_window_started
            if frozen_elapsed >= IPC_FREEZE_BUDGET_SECONDS:
                fail(
                    "accepted IPC client was not frozen before the framing "
                    f"safety budget ({frozen_elapsed:.3f}s >= "
                    f"{IPC_FREEZE_BUDGET_SECONDS:.3f}s)"
                )
            if slow_client.fileno() < 0:
                fail("incomplete IPC client closed while daemon was frozen")
            if socket_fd_count(process.daemon_pid) <= baseline_socket_fds:
                fail("accepted incomplete IPC fd vanished while daemon was frozen")
            # Complete the frame while every daemon thread is frozen.  On
            # resume the command can be admitted concurrently with shutdown;
            # shutdown must close admission and complete/cancel the mailbox
            # before waiting for the IPC poller.
            slow_client.sendall(b"STATS\n")
            for _ in range(16):
                if not send_pidfd_signal(process.daemon_pidfd, signal.SIGTERM):
                    fail("daemon exited during the coalesced SIGTERM burst", captured)
            burst_elapsed = time.monotonic() - client_window_started
            if burst_elapsed >= IPC_FRAME_EXPIRY_SECONDS:
                fail(
                    "SIGTERM burst exceeded the incomplete-frame expiry "
                    f"({burst_elapsed:.3f}s >= {IPC_FRAME_EXPIRY_SECONDS:.3f}s)"
                )
            if not send_pidfd_signal(process.daemon_pidfd, signal.SIGCONT):
                fail("daemon exited before SIGCONT", captured)
            slow_client.close()
            slow_client = None

            feeder_stop.set()
            feeder.join(timeout=2.0)
            if feeder.is_alive():
                fail("active-input feeder did not terminate")
            if feeder_errors:
                fail(f"active-input feeder failed: {feeder_errors[0]}", captured)

            wait_for_exit(
                process,
                socket_path,
                captured,
                0,
                "coalesced SIGTERM burst with active IPC/input",
                output_consumer=output_consumer,
            )
        finally:
            feeder_stop.set()
            if slow_client is not None:
                slow_client.close()
            feeder_alive = False
            if feeder is not None:
                feeder.join(timeout=2.0)
                feeder_alive = feeder.is_alive()
            cleanup_process(process, output_consumer=output_consumer)
            if output_consumer is not None:
                output_consumer.join()
            if feeder_alive:
                fail("active-input feeder survived mandatory cleanup")
            if feeder_errors:
                fail(f"active-input feeder failed: {feeder_errors[0]}")


def test_broken_stdout_is_orderly_runtime_failure(daemon: str) -> None:
    with tempfile.TemporaryDirectory(prefix="punto-epipe-") as directory:
        root = Path(directory)
        socket_path = root / "run" / IPC_SOCKET_NAME
        read_fd, write_fd = os.pipe()
        os.close(read_fd)
        try:
            process = spawn_daemon(daemon, root, write_fd)
        finally:
            os.close(write_fd)

        try:
            captured = wait_until_ready(process, socket_path)
            stream = process.supervisor.stdin
            assert stream is not None
            try:
                event = pack_event(1, 30, 1)
                written = os.write(stream.fileno(), event)
                if written != len(event):
                    fail("broken-stdout trigger input_event write was partial")
            except BrokenPipeError as error:
                fail(
                    f"daemon failed before exercising broken stdout: {error}",
                    captured,
                )
            if stream.closed:
                fail("broken-stdout contract requires stdin to remain open", captured)

            stderr = wait_for_exit(process, socket_path, captured, 3, "broken stdout")
            if b"KeyInjector" in stderr or b"Fatal output I/O error" in stderr:
                fail("broken stdout used the removed mutation output path", stderr)
            if b"Event loop terminated" not in stderr:
                fail(
                    "direct passthrough failure bypassed orderly EventLoop termination",
                    stderr,
                )
        finally:
            cleanup_process(process)


def preflight_output_consumer_cleanup() -> None:
    read_fd, write_fd = os.pipe2(os.O_CLOEXEC)
    stream = os.fdopen(read_fd, "rb", buffering=0)
    consumer: OutputConsumer | None = None
    try:
        consumer = OutputConsumer(stream)
        started = time.monotonic()
        consumer.start()
        consumer.stop(timeout=0.5)
        elapsed = time.monotonic() - started
        if elapsed > 0.75:
            fail(f"stdout consumer bounded stop took {elapsed:.3f}s")
    finally:
        try:
            if consumer is not None:
                consumer.stop(timeout=0.5)
        finally:
            stream.close()
            close_fd(write_fd)


def preflight() -> None:
    if not sys.platform.startswith("linux"):
        raise ContractSkip("Linux namespaces are required")
    if shutil.which("bwrap") is None:
        raise ContractSkip("bubblewrap is required")
    if not hasattr(os, "pidfd_open") or not hasattr(signal, "pidfd_send_signal"):
        raise ContractSkip("Python pidfd signal APIs are required")
    try:
        self_pidfd = os.pidfd_open(os.getpid(), 0)
    except OSError as error:
        raise ContractSkip(f"pidfd preflight failed: {error}") from error
    close_pidfd(self_pidfd)
    preflight_output_consumer_cleanup()

    try:
        with tempfile.TemporaryDirectory(prefix="punto-bwrap-preflight-") as directory:
            root = Path(directory)
            prepare_sandbox_layout(root)
            var_contract = (
                'test -L /var/run; test "$(readlink /var/run)" = /run; '
                'test -z "$(find /var -mindepth 1 -maxdepth 1 '
                '! -name run -print -quit)"'
            )
            smoke = subprocess.run(
                sandbox_prefix(root)
                + [
                    "--info-fd",
                    "2",
                    "--",
                    "/bin/sh",
                    "-ceu",
                    var_contract,
                ],
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
                timeout=5.0,
                check=False,
            )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise ContractSkip(f"bubblewrap namespace preflight failed: {error}") from error
    if smoke.returncode != 0:
        raise ContractSkip(
            f"bubblewrap namespace preflight failed with exit {smoke.returncode}"
        )
    if ctypes.sizeof(InputEvent) not in (16, 24):
        raise ContractSkip(
            f"unsupported native input_event size: {ctypes.sizeof(InputEvent)}"
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("daemon")
    args = parser.parse_args()
    daemon = os.path.realpath(args.daemon)
    if not os.path.isfile(daemon) or not os.access(daemon, os.X_OK):
        fail(f"daemon is not an executable regular file: {daemon}")

    preflight()
    cases = (
        ("command line", lambda: test_command_line_contract(daemon)),
        (
            "initialization failure",
            lambda: test_initialization_failure_exit_code(daemon),
        ),
        ("SIGTERM", lambda: test_sigterm_clean_exit(daemon)),
        (
            "startup config authority",
            lambda: test_service_home_is_not_startup_config_authority(daemon),
        ),
        (
            "control-plane authoritative failover",
            lambda: test_control_plane_promotion_reconciles_authoritative_snapshot(
                daemon
            ),
        ),
        (
            "control-plane fail-closed promotion",
            lambda: test_control_plane_promotion_fails_closed_without_valid_config(
                daemon
            ),
        ),
        ("stdin EOF/POLLHUP", lambda: test_stdin_eof_pollhup_clean_exit(daemon)),
        (
            "input health",
            lambda: test_input_health_tracks_only_accepted_frame_stalls(daemon),
        ),
        (
            "fragmented/partial input_event",
            lambda: test_fragmented_frame_and_partial_frame_sigterm(daemon),
        ),
        (
            "coalesced SIGTERM burst",
            lambda: test_coalesced_sigterm_burst_with_active_runtime(daemon),
        ),
        (
            "broken stdout",
            lambda: test_broken_stdout_is_orderly_runtime_failure(daemon),
        ),
    )
    failures: list[str] = []
    for name, case in cases:
        try:
            case()
        except AssertionError as error:
            failures.append(f"[{name}] {error}")
    if failures:
        raise AssertionError("\n\n".join(failures))
    print("test_daemon_process_contract: OK")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ContractSkip as error:
        if os.environ.get("PUNTO_REQUIRE_DAEMON_PROCESS_CONTRACT") == "1":
            print(
                f"test_daemon_process_contract: FAIL: required runtime missing: {error}",
                file=sys.stderr,
            )
            raise SystemExit(1)
        print(f"test_daemon_process_contract: SKIP: {error}", file=sys.stderr)
        raise SystemExit(SKIP_EXIT_CODE)
    except AssertionError as error:
        print(f"test_daemon_process_contract: FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
