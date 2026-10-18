#!/usr/bin/env python3
"""Fail-closed launcher for the C2C revision-20 isolated target diagnostic."""

from __future__ import annotations

import argparse
import errno
import hashlib
import json
import os
import re
import secrets
import select
import shutil
import signal
import subprocess
import sys
import time
from collections.abc import Callable, Iterable, Mapping, Sequence
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

GIB = 1024**3
TASK_ID = "c2c_bfdc"
ITERATION = 7
PLAN_REVISION = 20
REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_EVIDENCE_ROOT = (
    REPO_ROOT / ".cache/codex-thread-workflows/punto-i7-recovery-r20/proof"
)
R20_EVIDENCE_BASE = REPO_ROOT / ".cache/codex-thread-workflows/punto-i7-recovery-r20"
SCOPE_PREFIX = "punto-c2c-bfdc-i7-r20-"
_PRODUCTION_SURVIVOR_LIVE_STATES = frozenset({"D", "I", "K", "P", "R", "S", "T", "W", "t"})
_PRODUCTION_SURVIVOR_TERMINAL_STATES = frozenset({"X", "Z", "x"})
_TRANSPORT_KERNEL_FALLBACK_STATUSES = frozenset({"EOF_WITHOUT_RECORD", "PARTIAL_EOF"})
_OFFENDING_PREFIX_BYTES = 160
SCOPE_WATCHDOG_SECONDS = 60
CLEANUP_DEADLINE_SECONDS = 5
MAX_JSON_LINE_BYTES = 1_048_576
MAX_STDERR_BYTES = 65_536
PRODUCTION_SMOKE_PAYLOAD = b"punto-r13" * 4096
PRODUCTION_SMOKE_BYTES = len(PRODUCTION_SMOKE_PAYLOAD)
PRODUCTION_SMOKE_SHA256 = hashlib.sha256(PRODUCTION_SMOKE_PAYLOAD).hexdigest()
SCRATCH_METADATA_ALLOWANCE_BYTES = 64 * 1024
MEMORY_WORKER_COUNT = 3
MEMORY_WORKER_BYTES = 112 * 1024**2
ENFORCEMENT_PHASES = (
    "sandbox-ready",
    "task-proof",
    "scratch-proof",
    "scratch-restored",
    "memory-prepared",
    "memory-proof",
)
RESOURCE_PROOF_MOUNT_ALLOWLIST: dict[str, Mapping[str, Any]] = {
    "/": {"writable": True, "filesystem": "tmpfs"},
    "/proc": {"writable": True, "filesystem": "proc"},
    "/usr": {"writable": False},
    "/control": {"writable": False},
}


class IsolationError(RuntimeError):
    """A mandatory safety or identity boundary is unavailable."""


class SafetyAbort(IsolationError):
    """Cleanup or protected-host identity verification failed."""


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    path.write_text(
        json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    path.chmod(0o600)


def run_capture(command: Sequence[str], timeout: float = 10) -> dict[str, Any]:
    started = time.monotonic()
    try:
        completed = subprocess.run(
            list(command),
            stdin=subprocess.DEVNULL,
            capture_output=True,
            text=True,
            timeout=timeout,
            check=False,
            env={"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "LC_ALL": "C.UTF-8"},
            close_fds=True,
        )
        return {
            "command": list(command),
            "exit_code": completed.returncode,
            "stdout": completed.stdout,
            "stderr": completed.stderr,
            "elapsed_ms": round((time.monotonic() - started) * 1000),
        }
    except (OSError, subprocess.TimeoutExpired) as error:
        return {
            "command": list(command),
            "exit_code": None,
            "error": f"{type(error).__name__}: {error}",
            "elapsed_ms": round((time.monotonic() - started) * 1000),
        }


@dataclass(frozen=True)
class ArtifactSpec:
    artifact_id: str
    package: Path
    package_sha256: str
    executable_hashes: Mapping[str, str]
    payload_manifest_sha256: str | None = None

    def verify_and_extract(self, destination: Path) -> dict[str, Any]:
        if not self.package.is_file() or self.package.is_symlink():
            raise IsolationError(f"missing regular package: {self.package}")
        actual_package_hash = sha256_file(self.package)
        if actual_package_hash != self.package_sha256:
            raise IsolationError(
                f"package hash mismatch for {self.artifact_id}: {actual_package_hash}"
            )
        if destination.exists():
            raise IsolationError(f"extraction destination already exists: {destination}")
        destination.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        completed = subprocess.run(
            ["dpkg-deb", "--extract", str(self.package), str(destination)],
            stdin=subprocess.DEVNULL,
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
            env={"PATH": "/usr/bin:/bin", "LC_ALL": "C.UTF-8"},
            close_fds=True,
        )
        if completed.returncode != 0:
            raise IsolationError(
                f"package extraction failed for {self.artifact_id}: "
                f"{completed.stderr.strip()}"
            )
        verified: dict[str, str] = {}
        manifest_lines: list[str] = []
        for path in sorted(destination.rglob("*")):
            if path.is_symlink():
                target = os.readlink(path)
                if os.path.isabs(target) or ".." in Path(target).parts:
                    raise IsolationError(f"unsafe extracted symlink: {path}")
                continue
            if path.is_file():
                relative = path.relative_to(destination)
                manifest_lines.append(f"{sha256_file(path)}  ./{relative}\n")
        payload_manifest_hash = hashlib.sha256(
            "".join(manifest_lines).encode("utf-8")
        ).hexdigest()
        if (
            self.payload_manifest_sha256 is not None
            and payload_manifest_hash != self.payload_manifest_sha256
        ):
            raise IsolationError(f"payload manifest mismatch for {self.artifact_id}")
        for relative, expected in self.executable_hashes.items():
            path = destination / relative.lstrip("/")
            if not path.is_file() or path.is_symlink():
                raise IsolationError(f"missing extracted executable: {relative}")
            actual = sha256_file(path)
            if actual != expected:
                raise IsolationError(
                    f"executable hash mismatch for {self.artifact_id}:{relative}"
                )
            verified[relative] = actual
        return {
            "artifact_id": self.artifact_id,
            "package": str(self.package),
            "package_sha256": actual_package_hash,
            "payload_files": len(manifest_lines),
            "payload_manifest_sha256": payload_manifest_hash,
            "executables": verified,
            "extracted_root": str(destination),
        }


@dataclass(frozen=True)
class IsolationPolicy:
    forbidden_environment: frozenset[str] = frozenset(
        {"DISPLAY", "WAYLAND_DISPLAY", "XAUTHORITY", "DBUS_SESSION_BUS_ADDRESS"}
    )
    forbidden_mounts: tuple[str, ...] = (
        "/run/systemd",
        "/run/user",
        "/tmp/.X11-unix",
        "/dev/input",
        "/dev/uinput",
        "/dev/dri",
        "/dev/tty",
        "/dev/snd",
    )

    def validate_child_environment(self, environment: Mapping[str, str]) -> None:
        for name in sorted(self.forbidden_environment):
            if name in environment:
                raise IsolationError(f"host {name} is forbidden in child environment")

    def validate_mount_sources(self, sources: Iterable[Path]) -> None:
        for source in sources:
            try:
                normalized = str(source.resolve(strict=True))
            except OSError as error:
                raise IsolationError(f"cannot resolve host mount source: {source}: {error}") from error
            if normalized == "/":
                raise IsolationError("full root bind is forbidden")
            for forbidden in self.forbidden_mounts:
                matched = normalized == forbidden or normalized.startswith(forbidden + "/")
                if forbidden == "/dev/tty":
                    matched = normalized.startswith(forbidden)
                if matched:
                    raise IsolationError(f"forbidden host mount source: {normalized}")


def validate_evidence_root(path: Path, base: Path = R20_EVIDENCE_BASE) -> Path:
    """Return a new revision-20 evidence path without following an alias outside it."""
    if ".." in Path(path).parts:
        raise IsolationError("evidence root contains lexical ..")
    absolute_base = base.absolute()
    absolute_path = path.absolute()
    if ".." in absolute_path.parts:
        raise IsolationError("evidence root contains lexical ..")
    try:
        relative = absolute_path.relative_to(absolute_base)
    except ValueError as error:
        raise IsolationError("evidence root is outside revision-20 directory") from error
    if relative == Path(".") or not relative.parts:
        raise IsolationError("evidence root must be a subdirectory of the revision-20 directory")
    if ".." in relative.parts:
        raise IsolationError("evidence root contains lexical ..")
    current = absolute_base
    if current.is_symlink():
        raise IsolationError("revision-20 evidence base is a symlink")
    for part in relative.parts:
        if part == "..":
            raise IsolationError("evidence root contains lexical ..")
        current = current / part
        if current.is_symlink():
            raise IsolationError(f"evidence root contains symlink: {current}")
    if absolute_base.resolve(strict=False) != absolute_base:
        raise IsolationError("revision-20 evidence base resolves outside itself")
    resolved_path = absolute_path.resolve(strict=False)
    try:
        resolved_path.relative_to(absolute_base)
    except ValueError as error:
        raise IsolationError("evidence root canonical path escapes revision-20") from error
    if resolved_path != absolute_path:
        raise IsolationError("evidence root canonical path uses an alias")
    return absolute_path


def _unescape_mount_field(value: str) -> str:
    return (
        value.replace("\\040", " ")
        .replace("\\011", "\t")
        .replace("\\012", "\n")
        .replace("\\134", "\\")
    )


def parse_mountinfo(text: str) -> list[dict[str, Any]]:
    """Parse /proc/self/mountinfo using per-mount options before ' - ' only."""
    mounts: list[dict[str, Any]] = []
    for line in text.splitlines():
        if " - " not in line:
            raise IsolationError(f"invalid mountinfo line: {line[:160]!r}")
        left, right = line.split(" - ", 1)
        left_fields = left.split()
        right_fields = right.split()
        if len(left_fields) < 6 or len(right_fields) < 3:
            raise IsolationError(f"invalid mountinfo fields: {line[:160]!r}")
        mount_options = set(left_fields[5].split(","))
        filesystem = right_fields[0]
        writable = "rw" in mount_options and "ro" not in mount_options
        mounts.append(
            {
                "target": _unescape_mount_field(left_fields[4]),
                "source": filesystem,
                "filesystem": filesystem,
                "mount_source": _unescape_mount_field(right_fields[1]),
                "writable": writable,
                "mount_options": sorted(mount_options),
            }
        )
    return mounts


def verify_writable_mounts(mounts: Sequence[Mapping[str, Any]]) -> None:
    seen: list[str] = []
    for mount in mounts:
        target = os.path.normpath(str(mount.get("target", "")))
        if target in seen:
            raise IsolationError(f"duplicate mount target: {target}")
        seen.append(target)
        allowed = RESOURCE_PROOF_MOUNT_ALLOWLIST.get(target)
        writable = bool(mount.get("writable"))
        if allowed is None:
            if writable:
                raise IsolationError(f"unexpected writable mount: {target}")
            raise IsolationError(f"extra mount target: {target}")
        if writable and not allowed["writable"]:
            raise IsolationError(f"unexpected writable mount: {target}")
        if not writable and allowed["writable"]:
            raise IsolationError(f"required mount is not writable: {target}")
        required_filesystem = allowed.get("filesystem")
        if required_filesystem is not None:
            filesystem = str(mount.get("filesystem") or mount.get("source") or "")
            if filesystem != required_filesystem:
                raise IsolationError(
                    f"writable mount {target} is not {required_filesystem}: {filesystem}"
                )
    missing = [
        target for target in RESOURCE_PROOF_MOUNT_ALLOWLIST if target not in seen
    ]
    if missing:
        raise IsolationError(f"missing required mount target: {missing[0]}")


def _path_in_scratch(path: str) -> bool:
    normalized = os.path.normpath(path)
    if not os.path.isabs(normalized):
        return False
    return normalized == "/scratch" or normalized.startswith("/scratch/")


def verify_sandbox_paths(sandbox: Mapping[str, Any]) -> None:
    paths = sandbox.get("paths") if isinstance(sandbox.get("paths"), Mapping) else {}
    devices = sandbox.get("devices") if isinstance(sandbox.get("devices"), Mapping) else {}
    required = ("home", "tmpdir", "xdg_runtime_dir", "cwd", "scratch")
    if any(name not in paths or name not in devices for name in required):
        raise IsolationError("sandbox path or device inventory is incomplete")
    try:
        scratch_device = int(devices["scratch"])
    except (TypeError, ValueError) as error:
        raise IsolationError("sandbox scratch device is invalid") from error
    for name in required:
        resolved = str(paths[name])
        if not _path_in_scratch(resolved):
            raise IsolationError(f"{name} does not resolve inside /scratch")
        try:
            device = int(devices[name])
        except (TypeError, ValueError) as error:
            raise IsolationError(f"{name} device is invalid") from error
        if device != scratch_device:
            raise IsolationError(f"{name} is not on the scratch filesystem")


@dataclass(frozen=True)
class ResourceRequest:
    memory_bytes: int
    tasks_max: int
    scratch_bytes: int
    cpu_quota: int = 200000
    cpu_period: int = 100000
    cpu_count: int = 2


@dataclass(frozen=True)
class ScopeObservation:
    control_group: str
    member_pids: tuple[int, ...]
    cpu_quota: int | None
    cpu_period: int | None
    affinity: tuple[int, ...]
    memory_max: int | None
    memory_swap_max: int | None
    tasks_max: int | None
    scratch_bytes: int | None
    scratch_inode_count: int | None
    writable_mounts: tuple[Mapping[str, Any], ...]


def verify_kernel_observation(
    request: ResourceRequest,
    observation: ScopeObservation,
    starter_pid: int,
    unit: str | None = None,
) -> None:
    if not observation.control_group.endswith(".scope"):
        raise IsolationError("scope ControlGroup is not a dedicated transient scope")
    if unit is not None and Path(observation.control_group).name != unit:
        raise IsolationError("scope ControlGroup does not match the owned unit")
    if observation.member_pids != (starter_pid,):
        foreign = sorted(set(observation.member_pids) - {starter_pid})
        detail = foreign or list(observation.member_pids)
        raise SafetyAbort(f"foreign PID in scope before launch: {detail}")
    if observation.cpu_quota is None or observation.cpu_period is None:
        raise IsolationError("CPU quota is not set")
    if observation.cpu_quota > request.cpu_quota or observation.cpu_period != request.cpu_period:
        raise IsolationError("CPU quota readback exceeds request")
    if not observation.affinity or len(observation.affinity) > request.cpu_count:
        raise IsolationError("CPU affinity is not restricted to requested CPUs")
    if observation.memory_max != request.memory_bytes:
        raise IsolationError("memory limit readback mismatch")
    if observation.memory_swap_max != 0:
        raise IsolationError("swap limit readback mismatch")
    if observation.tasks_max != request.tasks_max:
        raise IsolationError("task limit readback mismatch")


def verify_scope_observation(
    request: ResourceRequest, observation: ScopeObservation, starter_pid: int, unit: str | None = None
) -> None:
    verify_kernel_observation(request, observation, starter_pid, unit=unit)
    if observation.scratch_bytes is not None:
        if observation.scratch_bytes != request.scratch_bytes:
            raise IsolationError("scratch limit readback mismatch")
        if not observation.scratch_inode_count or observation.scratch_inode_count < 1:
            raise IsolationError("scratch inode limit is not finite")
        verify_writable_mounts(observation.writable_mounts)


def verify_sandbox_observation(
    request: ResourceRequest,
    sandbox: Mapping[str, Any],
    mounts: Sequence[Mapping[str, Any]] | None = None,
) -> None:
    if sandbox.get("kind") != "sandbox-ready":
        raise IsolationError("sandbox did not report readiness")
    if sandbox.get("socket_fds"):
        raise IsolationError("inner sandbox inherited a socket descriptor")
    if mounts is None:
        mounts = parse_mountinfo(str(sandbox.get("mountinfo", "")))
    verify_writable_mounts(mounts)
    try:
        scratch_bytes = int(sandbox["scratch_bytes"])
        scratch_inodes = int(sandbox["scratch_inode_count"])
        available_bytes = int(sandbox["scratch_available_bytes"])
    except (KeyError, TypeError, ValueError) as error:
        raise IsolationError("sandbox scratch inventory is invalid") from error
    if scratch_bytes != request.scratch_bytes:
        raise IsolationError("scratch limit readback mismatch")
    if scratch_inodes < 1:
        raise IsolationError("scratch inode limit is not finite")
    if available_bytes < 1 or available_bytes > scratch_bytes:
        raise IsolationError("sandbox available scratch is invalid")
    if scratch_bytes - available_bytes > SCRATCH_METADATA_ALLOWANCE_BYTES:
        raise IsolationError("sandbox available scratch is below the requested budget")
    verify_sandbox_paths(sandbox)


def write_launch_marker(
    control_dir: Path,
    request: ResourceRequest,
    observation: ScopeObservation,
    starter_pid: int,
    unit: str,
) -> None:
    verify_kernel_observation(request, observation, starter_pid, unit=unit)
    (control_dir / "launch-go").write_text("go\n", encoding="ascii")


def verify_production_smoke(workload: Mapping[str, Any]) -> None:
    try:
        actual_bytes = int(workload["small_write_bytes"])
    except (KeyError, TypeError, ValueError) as error:
        raise IsolationError("production smoke byte length is missing") from error
    digest = workload.get("small_write_sha256")
    if not digest:
        raise IsolationError("production smoke digest is missing")
    if actual_bytes != PRODUCTION_SMOKE_BYTES:
        raise IsolationError("production smoke byte length mismatch")
    if digest != PRODUCTION_SMOKE_SHA256:
        raise IsolationError("production smoke digest mismatch")


def _required_counter(counters: Mapping[str, Any], key: str) -> int:
    if key not in counters:
        raise IsolationError(f"enforcement counter {key} is missing")
    value = counters[key]
    if type(value) is not int:
        raise IsolationError(f"enforcement counter {key} is not an integer")
    if value < 0:
        raise IsolationError(f"enforcement counter {key} is negative")
    return value


def _counter_increase(before: Mapping[str, Any], after: Mapping[str, Any], key: str) -> int:
    start = _required_counter(before, key)
    end = _required_counter(after, key)
    if end < start:
        raise IsolationError(f"enforcement counter {key} decreased")
    return end - start


def _accept_protocol_record(
    record: Mapping[str, Any], attempt_id: str, phase: str, sequence: int
) -> dict[str, Any]:
    expected_kind = "sandbox-ready" if sequence == 0 else "phase-result"
    if record.get("kind") != expected_kind:
        raise IsolationError(
            f"staged proof record kind is invalid: expected {expected_kind}"
        )
    if record.get("attempt_id") != attempt_id:
        raise IsolationError("staged proof record belongs to the wrong attempt")
    if record.get("phase") != phase:
        raise IsolationError(
            f"staged proof phase is out of order: expected {phase}, got {record.get('phase')}"
        )
    if type(record.get("sequence")) is not int or record["sequence"] != sequence:
        raise IsolationError(
            f"staged proof sequence is invalid: expected {sequence}, got {record.get('sequence')}"
        )
    return dict(record)


def _write_protocol_ack(
    control_dir: Path, attempt_id: str, phase: str, sequence: int
) -> Path:
    acknowledgement = {
        "kind": "phase-ack",
        "attempt_id": attempt_id,
        "phase": phase,
        "sequence": sequence,
    }
    path = control_dir / f"ack-{sequence}.json"
    if path.exists():
        raise IsolationError(f"duplicate staged proof acknowledgement: {sequence}")
    temporary = control_dir / f".ack-{sequence}-{attempt_id}.tmp"
    if temporary.exists():
        raise IsolationError(f"stale staged proof acknowledgement: {sequence}")
    write_json(temporary, acknowledgement)
    os.replace(temporary, path)
    return path


def _persist_received_protocol_record(
    proof: dict[str, Any],
    proof_path: Path,
    record: Mapping[str, Any],
    phase: str,
    sequence: int,
) -> None:
    protocol = proof.setdefault(
        "protocol",
        {
            "attempt_id": proof.get("attempt_id"),
            "received": [],
            "accepted": [],
            "rejected": [],
        },
    )
    protocol.setdefault("received", []).append(
        {"phase": phase, "sequence": sequence, "record": dict(record)}
    )
    write_json(proof_path, proof)


def _persist_rejected_protocol_record(
    proof: dict[str, Any],
    proof_path: Path,
    record: Mapping[str, Any],
    phase: str,
    sequence: int,
    error: BaseException,
) -> None:
    proof["protocol"].setdefault("rejected", []).append(
        {
            "phase": phase,
            "sequence": sequence,
            "record": dict(record),
            "error": str(error),
        }
    )
    write_json(proof_path, proof)


def verify_task_proof(
    task: Mapping[str, Any], before: Mapping[str, Any], after: Mapping[str, Any]
) -> None:
    delta = _counter_increase(before, after, "max")
    spawned = task.get("tasks_spawned")
    attempts = task.get("tasks_spawn_attempts")
    if (
        type(spawned) is not int
        or type(attempts) is not int
        or not 1 <= attempts <= 40
        or not 0 <= spawned < attempts
    ):
        raise IsolationError("task proof exceeded 40 spawn attempts")
    children = task.get("task_children")
    if not isinstance(children, list) or len(children) != spawned:
        raise IsolationError("task proof child outcomes are incomplete")
    if any(not isinstance(item, Mapping) or type(item.get("exit_code")) is not int for item in children):
        raise IsolationError("task proof contains an unreaped child")
    if delta < 1 or task.get("tasks_spawn_errno") != errno.EAGAIN:
        raise IsolationError("task enforcement was not observed")


def verify_scratch_proof(
    request: ResourceRequest, scratch: Mapping[str, Any], available_bytes: int
) -> None:
    files = scratch.get("scratch_files")
    if not isinstance(files, list) or len(files) != 3:
        raise IsolationError("scratch file size evidence is missing")
    try:
        written = int(scratch["scratch_successful_write_bytes"])
        reported_file_bytes = int(scratch["scratch_file_bytes"])
        sizes = [int(item["size"]) for item in files]
        intended = [int(item.get("intended_bytes") or 0) for item in files]
        available = int(available_bytes)
    except (KeyError, TypeError, ValueError) as error:
        raise IsolationError("scratch size evidence is invalid") from error
    file_total = sum(sizes)
    if reported_file_bytes != file_total or written != file_total:
        raise IsolationError("scratch size evidence is inconsistent")
    expected = [6 * 1024**2, 6 * 1024**2, 8 * 1024**2]
    if intended != expected or sizes[:2] != expected[:2] or not 0 <= sizes[2] < expected[2]:
        raise IsolationError("scratch file progression is inconsistent")
    if scratch.get("scratch_errno") != errno.ENOSPC:
        raise IsolationError("shared scratch enforcement was not observed")
    if available < 1:
        raise IsolationError("measured scratch availability is invalid")
    ceiling = min(request.scratch_bytes, available)
    if written > ceiling or file_total > ceiling:
        raise IsolationError("scratch writes exceed the shared ceiling")
    if written < ceiling - SCRATCH_METADATA_ALLOWANCE_BYTES:
        raise IsolationError("scratch ENOSPC was early")


def verify_scratch_restored(restored: Mapping[str, Any], available_before: int) -> None:
    value = restored.get("scratch_available_bytes")
    if type(value) is not int or value < available_before - SCRATCH_METADATA_ALLOWANCE_BYTES:
        raise IsolationError("scratch availability was not restored before memory proof")


def _transport_eligibility_for_status(status: str) -> str:
    if status == "COMPLETE":
        return "REPORTER"
    if status in _TRANSPORT_KERNEL_FALLBACK_STATUSES:
        return "KERNEL_FALLBACK"
    return "REJECT"


def _transport_status_for_parse_error(error: IsolationError) -> str:
    message = str(error)
    if "not UTF-8" in message:
        return "INVALID_UTF8"
    if "not an object" in message:
        return "INVALID_OBJECT"
    return "INVALID_JSON"


class _BoundedOffendingBytes:
    def __init__(self) -> None:
        self.byte_count = 0
        self._digest = hashlib.sha256()
        self._prefix = bytearray()

    def extend(self, data: bytes) -> None:
        if not data:
            return
        self._digest.update(data)
        room = _OFFENDING_PREFIX_BYTES - len(self._prefix)
        if room > 0:
            self._prefix.extend(data[:room])
        self.byte_count += len(data)

    def evidence(self) -> dict[str, Any] | None:
        if self.byte_count == 0:
            return None
        return {
            "byte_count": self.byte_count,
            "sha256": self._digest.hexdigest(),
            "prefix": bytes(self._prefix).decode("utf-8", "replace"),
        }


def verify_memory_proof(
    memory_before: Mapping[str, Any],
    observations: Sequence[Mapping[str, Any]],
    workers: Sequence[Mapping[str, Any]],
    transport: Mapping[str, Any] | None,
    kernel_evidence: Mapping[str, Any],
    *,
    attempt_id: str,
    control_group: str,
    release_started_at: float,
    observation_ended_at: float,
) -> None:
    if not isinstance(transport, Mapping):
        raise IsolationError("memory reporter transport is invalid")
    status = transport.get("status")
    eligibility = transport.get("eligibility")
    if not isinstance(status, str) or eligibility != _transport_eligibility_for_status(status):
        raise IsolationError("memory reporter transport eligibility is invalid")
    if eligibility == "REJECT":
        raise IsolationError("memory reporter transport is contradictory")

    previous_counter = _required_counter(memory_before, "oom_kill")
    previous_timestamp = release_started_at
    readable = 0
    increased = False
    for item in observations:
        if not isinstance(item, Mapping):
            raise IsolationError("memory proof observation is invalid")
        if item.get("attempt_id") != attempt_id:
            raise IsolationError("memory proof observation belongs to the wrong attempt")
        if item.get("control_group") != control_group:
            raise IsolationError("memory proof observation belongs to the wrong cgroup")
        observed_at = item.get("observed_at")
        if type(observed_at) not in (int, float):
            raise IsolationError("memory proof observation timestamp is invalid")
        if not release_started_at <= observed_at <= observation_ended_at:
            raise IsolationError("memory proof observation is outside the release window")
        if observed_at < previous_timestamp:
            raise IsolationError("memory proof observation timestamps are out of order")
        previous_timestamp = observed_at
        if item.get("available") is False:
            continue
        if item.get("available") is not True:
            raise IsolationError("memory proof observation availability is invalid")
        current = _required_counter(item.get("value", {}), "oom_kill")
        if current < previous_counter:
            raise IsolationError("enforcement counter oom_kill decreased")
        increased = increased or current > previous_counter
        previous_counter = current
        readable += 1
    if readable == 0:
        raise IsolationError("memory proof has no readable outer counter observation")
    if not increased:
        raise IsolationError("memory enforcement counter did not increase")
    if len(workers) != MEMORY_WORKER_COUNT:
        raise IsolationError("memory proof did not preserve exactly three workers")
    by_id = {item.get("worker_id"): item for item in workers}
    if len(by_id) != MEMORY_WORKER_COUNT or None in by_id:
        raise IsolationError("memory worker identities are incomplete")

    reporter = transport.get("record")
    if eligibility == "REPORTER":
        if not isinstance(reporter, Mapping):
            raise IsolationError("memory reporter is contradictory")
        reported = reporter.get("memory_children")
        if not isinstance(reported, list) or len(reported) != MEMORY_WORKER_COUNT:
            raise IsolationError("memory reporter is contradictory")
        reported_ids: set[str] = set()
        killed = False
        for item in reported:
            prepared = by_id.get(item.get("worker_id")) if isinstance(item, Mapping) else None
            if (
                prepared is None
                or item.get("worker_id") in reported_ids
                or item.get("namespace_pid") != prepared.get("namespace_pid")
                or type(item.get("exit_code")) is not int
            ):
                raise IsolationError("memory reporter is contradictory")
            reported_ids.add(item["worker_id"])
            killed = killed or item["exit_code"] == -signal.SIGKILL
        if reported_ids != set(by_id) or not killed:
            raise IsolationError("memory reporter is contradictory")
        return

    if reporter is not None:
        raise IsolationError("memory reporter transport is contradictory")
    if kernel_evidence.get("status") != "AVAILABLE":
        raise IsolationError("memory enforcement lacks a corroborated prepared-worker outcome")
    victims = kernel_evidence.get("victims")
    if not isinstance(victims, list):
        raise IsolationError("kernel victim evidence is invalid")
    host_pids = {item.get("host_pid") for item in workers}
    corroborated = any(
        isinstance(item, Mapping)
        and item.get("attempt_id") == attempt_id
        and item.get("control_group") == control_group
        and item.get("host_pid") in host_pids
        and type(item.get("observed_at")) in (int, float)
        and release_started_at <= item["observed_at"] <= observation_ended_at
        for item in victims
    )
    if not corroborated:
        raise IsolationError("memory enforcement lacks a corroborated prepared-worker outcome")


def finalize_scope_errors(
    proof: dict[str, Any],
    primary_error: BaseException | None,
    cleanup_error: BaseException | None,
) -> None:
    if primary_error is not None:
        proof["primary_error"] = str(primary_error)
    if cleanup_error is not None:
        cleanup = proof.setdefault("cleanup", {})
        if isinstance(cleanup, dict):
            cleanup["error"] = str(cleanup_error)
            cleanup["complete"] = False
        raise SafetyAbort(str(cleanup_error)) from cleanup_error
    if primary_error is not None:
        raise primary_error


def snapshot_is_valid(snapshot: Mapping[str, Any]) -> bool:
    if snapshot.get("status") != "valid":
        return False
    if not isinstance(snapshot.get("processes"), list):
        return False
    sessions = snapshot.get("sessions")
    if not isinstance(sessions, Mapping):
        return False
    return sessions.get("exit_code") == 0 and sessions.get("stdout") is not None


def compare_protected_identity(
    before: Mapping[str, Any], after: Mapping[str, Any]
) -> dict[str, Any]:
    valid = snapshot_is_valid(before) and snapshot_is_valid(after)
    processes_equal = valid and before.get("processes") == after.get("processes")
    before_sessions = before.get("sessions") if isinstance(before.get("sessions"), Mapping) else {}
    after_sessions = after.get("sessions") if isinstance(after.get("sessions"), Mapping) else {}
    sessions_equal = valid and before_sessions.get("stdout") == after_sessions.get("stdout")
    return {
        "status": "valid" if valid else "unknown",
        "protected_processes_unchanged": bool(processes_equal),
        "protected_sessions_unchanged": bool(sessions_equal),
        "protected_before": before.get("processes"),
        "protected_after": after.get("processes"),
        "protected_sessions_before": before.get("sessions"),
        "protected_sessions_after": after.get("sessions"),
    }


def assert_ready_for_next_scope(
    first_proof: Mapping[str, Any],
    before: Mapping[str, Any],
    intermediate: Mapping[str, Any],
) -> None:
    cleanup = first_proof.get("cleanup") if isinstance(first_proof.get("cleanup"), Mapping) else {}
    if cleanup.get("unknown"):
        raise SafetyAbort("first scope cleanup is unknown")
    if cleanup.get("complete") is not True:
        raise SafetyAbort("first scope cleanup is incomplete")
    comparison = compare_protected_identity(before, intermediate)
    if comparison["status"] != "valid":
        raise SafetyAbort("host postflight is invalid or unknown")
    if not comparison["protected_processes_unchanged"] or not comparison["protected_sessions_unchanged"]:
        raise SafetyAbort("protected host identity changed after first scope")


def wait_for_owned_starter_exit(
    process: subprocess.Popen[str],
    starter: Mapping[str, Any],
    timeout: float,
) -> dict[str, Any]:
    """Reap the owned starter Popen without signaling it to manufacture an exit."""
    try:
        starter_pid = int(starter["pid"])
        starter_identity = str(starter["start_identity"])
    except (KeyError, TypeError, ValueError) as error:
        raise IsolationError("starter identity record is invalid") from error
    if process.pid != starter_pid:
        raise IsolationError("owned Popen PID does not match recorded starter")
    try:
        current_identity = process_start_identity(starter_pid)
    except (FileNotFoundError, ProcessLookupError):
        current_identity = None
    except IsolationError as error:
        raise IsolationError("owned Popen identity does not match recorded starter") from error
    if current_identity is not None and current_identity != starter_identity:
        raise IsolationError("owned Popen identity does not match recorded starter")
    deadline = time.monotonic() + timeout
    exit_code = process.poll()
    if exit_code is None:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise IsolationError("starter still alive before cleanup")
        try:
            exit_code = process.wait(timeout=remaining)
        except subprocess.TimeoutExpired as error:
            raise IsolationError("starter still alive before cleanup") from error
    if type(exit_code) is not int:
        raise IsolationError("starter exit status is missing")
    if exit_code != 0:
        raise IsolationError(f"starter exited abnormally: {exit_code}")
    return {
        "pid": starter_pid,
        "owned_process_pid": process.pid,
        "start_identity": starter_identity,
        "ownership_verified": True,
        "exit_code": exit_code,
        "natural_exit": True,
        "reaped": True,
    }


def parse_production_survivor_stat(raw: str, expected_pid: int) -> dict[str, Any]:
    """Parse PID, state and start identity from one /proc/<pid>/stat snapshot."""
    open_paren = raw.find("(")
    close_paren = raw.rfind(")")
    if open_paren < 1 or close_paren <= open_paren:
        raise IsolationError("escaped child state observation is malformed")
    pid_text = raw[:open_paren].strip()
    fields = raw[close_paren + 1 :].split()
    if not pid_text or len(fields) < 20 or not fields[0]:
        raise IsolationError("escaped child state observation is malformed")
    try:
        observed_pid = int(pid_text)
    except ValueError as error:
        raise IsolationError("escaped child state observation is malformed") from error
    if observed_pid != expected_pid:
        raise IsolationError("escaped child identity changed before cleanup")
    return {
        "pid": observed_pid,
        "state": fields[0],
        "start_identity": fields[19],
    }


def observe_production_survivor_stat(pid: int) -> dict[str, Any]:
    """Read /proc/<pid>/stat once for the production survivor."""
    try:
        raw = Path(f"/proc/{pid}/stat").read_text(encoding="ascii")
    except FileNotFoundError as error:
        raise IsolationError("escaped child identity changed before cleanup") from error
    except (OSError, UnicodeError) as error:
        raise IsolationError("escaped child state is unreadable") from error
    return parse_production_survivor_stat(raw, pid)


def production_survivor_cleanup_target(
    child: Mapping[str, Any],
    members: Sequence[int],
    observation: Mapping[str, Any],
) -> dict[str, Any]:
    """Establish exact owned-scope cleanup identity before live acceptance."""
    try:
        child_pid = int(child["pid"])
        recorded_identity = str(child["start_identity"])
        observed_pid = int(observation["pid"])
        observed_identity = str(observation["start_identity"])
    except (KeyError, TypeError, ValueError) as error:
        raise IsolationError("escaped child identity record is invalid") from error
    if observed_pid != child_pid or observed_identity != recorded_identity:
        raise IsolationError("escaped child identity changed before cleanup")
    if child_pid not in set(members):
        raise IsolationError("new-process-group child escaped the owned scope")
    return {
        "pid": child_pid,
        "start_identity": recorded_identity,
    }


def accept_production_survivor(
    child: Mapping[str, Any],
    members: Sequence[int],
    observation: Mapping[str, Any] | None = None,
) -> dict[str, Any]:
    """Accept a live child PID/start identity that is in the fresh owned scope."""
    try:
        child_pid = int(child["pid"])
        recorded_identity = str(child["start_identity"])
        child_pgid = int(child["pgid"])
        child_sid = int(child["sid"])
        recorded_starter_pgid = int(child["starter_pgid"])
        recorded_starter_sid = int(child["starter_sid"])
    except (KeyError, TypeError, ValueError) as error:
        raise IsolationError("escaped child identity record is invalid") from error
    if observation is None:
        observation = observe_production_survivor_stat(child_pid)
    try:
        state = str(observation["state"])
    except (KeyError, TypeError, ValueError) as error:
        raise IsolationError("escaped child state observation is malformed") from error
    production_survivor_cleanup_target(child, members, observation)
    if not state:
        raise IsolationError("escaped child state observation is malformed")
    if state in _PRODUCTION_SURVIVOR_TERMINAL_STATES:
        raise IsolationError("escaped child is in exited state")
    if state not in _PRODUCTION_SURVIVOR_LIVE_STATES:
        raise IsolationError(f"escaped child is in unknown state: {state}")
    return {
        "pid": child_pid,
        "start_identity": recorded_identity,
        "pgid": child_pgid,
        "sid": child_sid,
        "starter_pgid": recorded_starter_pgid,
        "starter_sid": recorded_starter_sid,
        "starter_exited": True,
    }


def verify_production_relationship(
    starter: Mapping[str, Any],
    survivor: Mapping[str, Any],
) -> None:
    """Require distinct, unchanged process group and session identities."""
    try:
        starter_pgid = int(starter["pgid"])
        starter_sid = int(starter["sid"])
        child_pid = int(survivor["pid"])
        child_pgid = int(survivor["pgid"])
        child_sid = int(survivor["sid"])
        recorded_starter_pgid = int(survivor["starter_pgid"])
        recorded_starter_sid = int(survivor["starter_sid"])
    except (KeyError, TypeError, ValueError) as error:
        raise IsolationError("escaped child identity record is invalid") from error
    if recorded_starter_pgid != starter_pgid or recorded_starter_sid != starter_sid:
        raise IsolationError("escaped child starter identity is inconsistent")
    if child_pgid == starter_pgid:
        raise IsolationError("surviving child did not receive a distinct process group")
    if child_sid == starter_sid:
        raise IsolationError("surviving child did not receive a distinct session")
    try:
        live_pgid = os.getpgid(child_pid)
        live_sid = os.getsid(child_pid)
    except ProcessLookupError as error:
        raise IsolationError("surviving child session identity changed before cleanup") from error
    if live_pgid != child_pgid or live_sid != child_sid:
        raise IsolationError("surviving child session identity changed before cleanup")


def _read_limit(path: Path) -> int | None:
    value = path.read_text(encoding="ascii").strip()
    return None if value == "max" else int(value)


def _manager_environment() -> dict[str, str]:
    runtime = Path("/run/user") / str(os.getuid())
    bus = runtime / "bus"
    if not runtime.is_dir() or not bus.exists():
        raise IsolationError("user manager unavailable: runtime bus is absent")
    return {
        "PATH": "/usr/bin:/bin",
        "LC_ALL": "C.UTF-8",
        "XDG_RUNTIME_DIR": str(runtime),
        "DBUS_SESSION_BUS_ADDRESS": f"unix:path={bus}",
    }


class UserScopeManager:
    def __init__(
        self,
        run: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
        popen: Callable[..., subprocess.Popen[str]] = subprocess.Popen,
    ) -> None:
        self.run = run
        self.popen = popen
        self.environment = _manager_environment()

    @staticmethod
    def _validate_unit(unit: str) -> None:
        if not re.fullmatch(r"punto-c2c-bfdc-i7-r20-[a-z0-9]+\.scope", unit):
            raise IsolationError(f"refusing non-owned scope name: {unit}")

    def _capture(self, command: Sequence[str], timeout: float = 10) -> subprocess.CompletedProcess[str]:
        return self.run(
            list(command), stdin=subprocess.DEVNULL, capture_output=True, text=True,
            timeout=timeout, check=False, env=self.environment, close_fds=True,
        )

    def available(self) -> None:
        result = self._capture(["systemctl", "--user", "show", "-p", "Version"])
        if result.returncode != 0:
            raise IsolationError(
                "user manager unavailable: " + (result.stderr.strip() or "read-only probe failed")
            )

    def start(
        self, unit: str, request: ResourceRequest, command: Sequence[str]
    ) -> subprocess.Popen[str]:
        self._validate_unit(unit)
        self.available()
        quota_percent = request.cpu_quota * 100 / request.cpu_period
        invocation = [
            "systemd-run", "--user", "--scope", "--quiet", f"--unit={unit}",
            "--property", f"CPUQuota={quota_percent:g}%",
            "--property", f"MemoryMax={request.memory_bytes}",
            "--property", "MemorySwapMax=0",
            "--property", f"TasksMax={request.tasks_max}",
            "--", *command,
        ]
        try:
            return self.popen(
                invocation, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, text=True, env=self.environment, close_fds=True,
            )
        except OSError as error:
            raise IsolationError(f"transient scope start failed: {error}") from error

    def properties(self, unit: str, timeout: float = 10) -> dict[str, str]:
        self._validate_unit(unit)
        result = self._capture([
            "systemctl", "--user", "show", unit,
            "-p", "ControlGroup", "-p", "ActiveState", "-p", "SubState",
            "-p", "Result", "-p", "OOMPolicy",
            "-p", "MemoryMax", "-p", "MemorySwapMax", "-p", "TasksMax",
            "-p", "CPUQuotaPerSecUSec", "-p", "CPUQuotaPeriodUSec",
        ], timeout=timeout)
        if result.returncode != 0:
            raise IsolationError(f"cannot read transient scope: {result.stderr.strip()}")
        return dict(line.split("=", 1) for line in result.stdout.splitlines() if "=" in line)

    def scope_members(self, control_group: str) -> tuple[int, ...]:
        state = self.scope_state(control_group)
        if state["unknown"]:
            raise IsolationError(f"cannot read recursive scope membership: {control_group}")
        return tuple(state["members"])

    def scope_state(self, control_group: str) -> dict[str, Any]:
        if not control_group.startswith("/user.slice/") or not control_group.endswith(".scope"):
            raise IsolationError(f"unsafe ControlGroup: {control_group}")
        root = Path("/sys/fs/cgroup") / control_group.lstrip("/")
        if not root.exists():
            return {"members": (), "populated": False, "unknown": False}
        try:
            members: set[int] = set()
            for path in (root / "cgroup.procs", *root.rglob("cgroup.procs")):
                members.update(int(value) for value in path.read_text(encoding="ascii").split())
            events = dict(
                line.split(maxsplit=1)
                for line in (root / "cgroup.events").read_text(encoding="ascii").splitlines()
            )
            return {
                "members": tuple(sorted(members)),
                "populated": events.get("populated") == "1",
                "unknown": False,
            }
        except FileNotFoundError:
            if not root.exists():
                return {"members": (), "populated": False, "unknown": False}
            return {"members": (), "populated": None, "unknown": True}
        except (OSError, ValueError):
            return {"members": (), "populated": None, "unknown": True}

    def kill_scope(self, unit: str, timeout: float = CLEANUP_DEADLINE_SECONDS) -> None:
        self._validate_unit(unit)
        deadline = time.monotonic() + timeout

        def remaining() -> float:
            value = deadline - time.monotonic()
            if value <= 0:
                raise IsolationError("scope cleanup deadline expired")
            return value

        for signal_name in ("SIGTERM", "SIGKILL"):
            result = self._capture([
                "systemctl", "--user", "kill", "--kill-whom=all",
                f"--signal={signal_name}", unit,
            ], timeout=min(1, remaining()))
            if result.returncode != 0 and "not loaded" not in result.stderr.lower():
                raise IsolationError(f"scope cleanup failed: {result.stderr.strip()}")
            time.sleep(min(0.1, remaining()))
            try:
                properties = self.properties(unit, timeout=min(1, remaining()))
            except IsolationError:
                continue
            control_group = properties.get("ControlGroup", "")
            if not control_group:
                break
            state = self.scope_state(control_group)
            if not state["unknown"] and not state["members"] and not state["populated"]:
                break
        stopped = self._capture(
            ["systemctl", "--user", "stop", unit], timeout=remaining()
        )
        if stopped.returncode != 0 and "not loaded" not in stopped.stderr.lower():
            raise IsolationError(f"scope stop failed: {stopped.stderr.strip()}")


@dataclass
class TransientScope:
    manager: UserScopeManager
    unit: str
    process: subprocess.Popen[str]
    control_group: str
    starter_pid: int
    starter_identity: str
    cleaned: bool = False

    survivor: Mapping[str, Any] | None = None
    cleanup_result: dict[str, Any] | None = None

    def cleanup(self) -> dict[str, Any]:
        if self.cleaned:
            if self.cleanup_result is None:
                raise IsolationError("scope cleanup result is unavailable")
            return self.cleanup_result
        deadline = time.monotonic() + CLEANUP_DEADLINE_SECONDS

        def remaining() -> float:
            value = deadline - time.monotonic()
            if value <= 0:
                raise IsolationError("scope cleanup deadline expired")
            return value

        state = self.manager.scope_state(self.control_group)
        if state["unknown"] or state["members"] or state["populated"]:
            self.manager.kill_scope(self.unit, timeout=remaining())
        while time.monotonic() < deadline:
            state = self.manager.scope_state(self.control_group)
            if not state["unknown"] and not state["members"] and state["populated"] is False:
                break
            time.sleep(0.05)
        state = self.manager.scope_state(self.control_group)
        self.cleaned = True
        if self.process.poll() is None:
            try:
                self.process.wait(timeout=min(1, remaining()))
            except subprocess.TimeoutExpired:
                self.process.terminate()
                self.process.wait(timeout=remaining())
        for stream in (self.process.stdout, self.process.stderr):
            if stream is not None and not stream.closed:
                stream.close()
        survivor_gone = None
        if self.survivor:
            try:
                survivor_gone = (
                    process_start_identity(int(self.survivor["pid"]))
                    != str(self.survivor["start_identity"])
                )
            except (FileNotFoundError, ProcessLookupError):
                survivor_gone = True
        result = {**state, "survivor_gone": survivor_gone}
        self.cleanup_result = result
        if state["unknown"]:
            raise IsolationError("scope cleanup membership is unknown")
        if state["members"] or state["populated"] is not False or survivor_gone is False:
            raise IsolationError(f"scope cleanup incomplete: {result}")
        return result


def _wait_for_file(path: Path, process: subprocess.Popen[str], timeout: float) -> dict[str, Any]:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.is_file():
            return json.loads(path.read_text(encoding="utf-8"))
        if process.poll() is not None:
            raise IsolationError(
                f"scope starter exited before gate with status {process.returncode}"
            )
        time.sleep(0.02)
    raise IsolationError(f"scope starter gate timed out: {path.name}")


def _parse_json_object_line(line_bytes: bytes) -> dict[str, Any]:
    try:
        line = line_bytes.decode("utf-8")
    except UnicodeDecodeError as error:
        raise IsolationError("scope workload evidence is not UTF-8") from error
    try:
        value = json.loads(line)
    except json.JSONDecodeError as error:
        raise IsolationError(f"invalid scope workload evidence: {line[:160]!r}") from error
    if not isinstance(value, dict):
        raise IsolationError("scope workload evidence is not an object")
    value["_raw_json_line"] = line
    return value


def _read_json_line(process: subprocess.Popen[str], timeout: float) -> dict[str, Any]:
    if process.stdout is None:
        raise IsolationError("scope output pipe is unavailable")
    deadline = time.monotonic() + timeout
    data = bytearray()
    descriptor = process.stdout.fileno()
    while b"\n" not in data:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise IsolationError("scope workload output timed out")
        readable, _, _ = select.select([descriptor], [], [], remaining)
        if not readable:
            raise IsolationError("scope workload output timed out")
        chunk = os.read(descriptor, min(65536, MAX_JSON_LINE_BYTES + 1 - len(data)))
        if not chunk:
            if data:
                raise IsolationError("scope workload output ended with a partial line")
            raise IsolationError("scope workload ended without evidence")
        data.extend(chunk)
        if len(data) > MAX_JSON_LINE_BYTES:
            raise IsolationError("scope workload evidence exceeds size limit")
    line_bytes, remainder = bytes(data).split(b"\n", 1)
    if remainder:
        raise IsolationError("scope workload emitted multiple buffered records")
    return _parse_json_object_line(line_bytes)


SANDBOX_WORKLOAD = r'''
import errno
import hashlib
import json
import os
import subprocess
import sys
import time
from pathlib import Path

mode = sys.argv[1]
scratch_limit = int(sys.argv[2])
attempt_id = sys.argv[3]
protocol_deadline = time.monotonic() + 55

def emit(phase, sequence, **values):
    print(json.dumps({
        "kind": "phase-result", "attempt_id": attempt_id,
        "phase": phase, "sequence": sequence, **values,
    }, sort_keys=True), flush=True)

def await_ack(phase, sequence):
    path = Path("/control") / f"ack-{sequence}.json"
    while time.monotonic() < protocol_deadline:
        if path.is_file():
            value = json.loads(path.read_text())
            expected = {
                "kind": "phase-ack", "attempt_id": attempt_id,
                "phase": phase, "sequence": sequence,
            }
            if value != expected:
                raise SystemExit("invalid phase acknowledgement")
            return
        time.sleep(0.02)
    raise SystemExit("phase acknowledgement timeout")
stat = os.statvfs("/scratch")
mountinfo = Path("/proc/self/mountinfo").read_text()
for forbidden in ("DISPLAY", "WAYLAND_DISPLAY", "XAUTHORITY", "DBUS_SESSION_BUS_ADDRESS"):
    if forbidden in os.environ:
        raise SystemExit("forbidden environment: " + forbidden)
socket_fds = []
for entry in Path("/proc/self/fd").iterdir():
    try:
        if os.readlink(entry).startswith("socket:"):
            socket_fds.append(entry.name)
    except OSError:
        pass
ready = {
    "kind": "sandbox-ready",
    "attempt_id": attempt_id,
    "phase": "sandbox-ready",
    "sequence": 0,
    "scratch_bytes": stat.f_blocks * stat.f_frsize,
    "scratch_available_bytes": stat.f_bavail * stat.f_frsize,
    "scratch_inode_count": stat.f_files,
    "mountinfo": mountinfo,
    "socket_fds": socket_fds,
    "paths": {
        "home": str(Path(os.environ["HOME"]).resolve()),
        "tmpdir": str(Path(os.environ["TMPDIR"]).resolve()),
        "xdg_runtime_dir": str(Path(os.environ["XDG_RUNTIME_DIR"]).resolve()),
        "cwd": str(Path.cwd().resolve()),
        "scratch": str(Path("/scratch").resolve()),
    },
    "devices": {
        "home": os.stat(os.environ["HOME"]).st_dev,
        "tmpdir": os.stat(os.environ["TMPDIR"]).st_dev,
        "xdg_runtime_dir": os.stat(os.environ["XDG_RUNTIME_DIR"]).st_dev,
        "cwd": os.stat(Path.cwd()).st_dev,
        "scratch": os.stat("/scratch").st_dev,
    },
}
print(json.dumps(ready, sort_keys=True), flush=True)
if mode == "enforcement":
    await_ack("sandbox-ready", 0)
    sleepers = []
    spawn_error = None
    spawn_attempts = 0
    for _ in range(40):
        spawn_attempts += 1
        try:
            sleepers.append(subprocess.Popen(["/usr/bin/sleep", "2"]))
        except OSError as error:
            spawn_error = error.errno
            break
    for child in sleepers:
        child.terminate()
    task_children = []
    for child in sleepers:
        task_children.append({"namespace_pid": child.pid, "exit_code": child.wait(timeout=2)})
    emit(
        "task-proof", 1, tasks_spawned=len(sleepers),
        tasks_spawn_attempts=spawn_attempts, tasks_spawn_errno=spawn_error,
        task_children=task_children,
    )
    await_ack("task-proof", 1)

    scratch_error = None
    successful_write_bytes = 0
    scratch_files = []
    try:
        for index, mib in enumerate((6, 6, 8)):
            path = Path("/scratch") / f"budget-{index}"
            intended = mib * 1024 * 1024
            try:
                with path.open("wb", buffering=0) as stream:
                    remaining = intended
                    while remaining:
                        count = stream.write(b"x" * min(1024 * 1024, remaining))
                        if not count:
                            raise OSError(errno.EIO, "scratch write made no progress")
                        successful_write_bytes += count
                        remaining -= count
            finally:
                if path.exists():
                    scratch_files.append({
                        "path": path.name,
                        "size": path.stat().st_size,
                        "intended_bytes": intended,
                    })
    except OSError as error:
        scratch_error = error.errno
    emit(
        "scratch-proof", 2,
        scratch_successful_write_bytes=successful_write_bytes,
        scratch_file_bytes=sum(item["size"] for item in scratch_files),
        scratch_errno=scratch_error, scratch_files=scratch_files,
    )
    await_ack("scratch-proof", 2)
    for item in scratch_files:
        (Path("/scratch") / item["path"]).unlink(missing_ok=True)
    restored = os.statvfs("/scratch")
    emit(
        "scratch-restored", 3,
        scratch_available_bytes=restored.f_bavail * restored.f_frsize,
    )
    await_ack("scratch-restored", 3)

    memory_code = (
        "import sys,time; from pathlib import Path; "
        "attempt_id,worker_id=sys.argv[1:3]; deadline=time.monotonic()+50; "
        "gate=Path('/control/ack-4.json'); "
        "exec(\"while not gate.is_file():\\n"
        " if time.monotonic() >= deadline: raise SystemExit(72)\\n"
        " time.sleep(0.02)\"); "
        "size=112*1024*1024; allocation=bytearray(size); "
        "[allocation.__setitem__(offset,1) for offset in range(0,size,4096)]; "
        "time.sleep(4)"
    )
    memory = []
    declarations = []
    for index in range(3):
        worker_id = f"memory-{index}"
        child = subprocess.Popen([
            sys.executable, "-c", memory_code, attempt_id, worker_id,
        ])
        memory.append((worker_id, child))
        declarations.append({
            "worker_id": worker_id, "namespace_pid": child.pid,
            "requested_bytes": 112 * 1024 * 1024,
        })
    emit("memory-prepared", 4, memory_workers=declarations)
    await_ack("memory-prepared", 4)
    memory_children = []
    for worker_id, child in memory:
        try:
            exit_code = child.wait(timeout=max(0.1, protocol_deadline - time.monotonic()))
        except subprocess.TimeoutExpired:
            child.terminate()
            exit_code = child.wait(timeout=2)
        memory_children.append({
            "worker_id": worker_id, "namespace_pid": child.pid,
            "exit_code": exit_code,
        })
    emit("memory-proof", 5, memory_children=memory_children)
    await_ack("memory-proof", 5)
else:
    deadline = time.monotonic() + 10
    while not Path("/control/workload-go").exists():
        if time.monotonic() >= deadline:
            raise SystemExit("workload gate timeout")
        time.sleep(0.02)
    result = {"kind": "workload-result", "mode": mode}
    payload = b"punto-r13" * 4096
    path = Path("/scratch/smoke.bin")
    path.write_bytes(payload)
    result["small_write_bytes"] = len(payload)
    result["small_write_sha256"] = hashlib.sha256(path.read_bytes()).hexdigest()
    print(json.dumps(result, sort_keys=True), flush=True)
'''


def _bwrap_command(
    control_dir: Path, mode: str, scratch_bytes: int, attempt_id: str = "legacy"
) -> list[str]:
    policy = IsolationPolicy()
    policy.validate_mount_sources([Path("/usr"), control_dir])
    return [
        "bwrap", "--die-with-parent", "--new-session", "--unshare-all", "--clearenv",
        "--size", str(scratch_bytes), "--tmpfs", "/",
        "--ro-bind", "/usr", "/usr", "--symlink", "usr/bin", "/bin",
        "--symlink", "usr/lib", "/lib", "--symlink", "usr/lib64", "/lib64",
        "--proc", "/proc", "--dir", "/dev",
        "--dir", "/scratch", "--dir", "/scratch/home", "--dir", "/scratch/tmp",
        "--dir", "/scratch/run",
        "--dir", "/scratch/shm", "--symlink", "/scratch/tmp", "/tmp",
        "--symlink", "/scratch/run", "/run", "--symlink", "/scratch/home", "/home/punto",
        "--ro-bind", str(control_dir), "/control", "--setenv", "PATH", "/usr/bin:/bin",
        "--setenv", "HOME", "/scratch/home", "--setenv", "TMPDIR", "/scratch/tmp",
        "--setenv", "XDG_RUNTIME_DIR", "/scratch/run", "--chdir", "/scratch",
        "/usr/bin/python3", "-c", SANDBOX_WORKLOAD, mode, str(scratch_bytes), attempt_id,
    ]


def scope_worker(
    control_dir: Path, mode: str, scratch_bytes: int, cpus: Sequence[int], attempt_id: str
) -> int:
    os.sched_setaffinity(0, set(cpus))
    starter = {
        "pid": os.getpid(),
        "start_identity": process_start_identity(os.getpid()),
        "affinity": sorted(os.sched_getaffinity(0)),
        "pgid": os.getpgid(0),
        "sid": os.getsid(0),
    }
    write_json(control_dir / "starter-ready.json", starter)
    deadline = time.monotonic() + 10
    while not (control_dir / "launch-go").exists():
        if time.monotonic() >= deadline:
            return 70
        time.sleep(0.02)
    completed = subprocess.run(
        _bwrap_command(control_dir, mode, scratch_bytes, attempt_id),
        stdin=subprocess.DEVNULL, check=False, env={"PATH": "/usr/bin:/bin", "LC_ALL": "C.UTF-8"},
        close_fds=True,
    )
    write_json(control_dir / "sandbox-exit.json", {"exit_code": completed.returncode})
    deadline = time.monotonic() + 10
    while not (control_dir / "finish-go").exists():
        if time.monotonic() >= deadline:
            return 71
        time.sleep(0.02)
    if mode == "production":
        child = subprocess.Popen(
            ["/usr/bin/sleep", "30"], stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            close_fds=True, start_new_session=True,
        )
        write_json(
            control_dir / "escaped-child.json",
            {
                "pid": child.pid,
                "start_identity": process_start_identity(child.pid),
                "pgid": os.getpgid(child.pid),
                "sid": os.getsid(child.pid),
                "starter_pgid": starter["pgid"],
                "starter_sid": starter["sid"],
            },
        )
    return completed.returncode


def _cgroup_path(control_group: str) -> Path:
    path = Path("/sys/fs/cgroup") / control_group.lstrip("/")
    current = next(
        (line.split(":", 2)[2] for line in Path("/proc/self/cgroup").read_text().splitlines()
         if line.startswith("0::")), ""
    )
    if control_group == current or current.startswith(control_group.rstrip("/") + "/"):
        raise IsolationError("transient scope overlaps the current editor cgroup")
    if not control_group.startswith("/user.slice/") or not control_group.endswith(".scope"):
        raise IsolationError(f"unsafe transient scope cgroup: {control_group}")
    return path


def _cpu_max(path: Path) -> tuple[int | None, int | None]:
    quota, period = (path / "cpu.max").read_text(encoding="ascii").split()
    return (None if quota == "max" else int(quota), int(period))


def _event_counters(path: Path, name: str) -> dict[str, int]:
    result: dict[str, int] = {}
    for line in (path / name).read_text(encoding="ascii").splitlines():
        key, value = line.split()
        result[key] = int(value)
    return result


def _process_namespace_pids(pid: int) -> tuple[int, ...]:
    for line in (Path("/proc") / str(pid) / "status").read_text(encoding="ascii").splitlines():
        if line.startswith("NSpid:"):
            values = tuple(int(value) for value in line.split()[1:])
            if values:
                return values
    raise IsolationError(f"process {pid} has no namespace PID identity")


def _process_cmdline(pid: int) -> tuple[str, ...]:
    raw = (Path("/proc") / str(pid) / "cmdline").read_bytes()
    return tuple(part.decode("utf-8", errors="strict") for part in raw.split(b"\0") if part)


def _map_memory_workers(
    manager: UserScopeManager,
    control_group: str,
    starter_pid: int,
    prepared: Mapping[str, Any],
    attempt_id: str,
) -> list[dict[str, Any]]:
    declarations = prepared.get("memory_workers")
    if not isinstance(declarations, list) or len(declarations) != MEMORY_WORKER_COUNT:
        raise IsolationError("memory preparation did not declare exactly three workers")
    members = manager.scope_members(control_group)
    candidates: dict[int, tuple[int, tuple[int, ...], tuple[str, ...]]] = {}
    for host_pid in members:
        if not _is_descendant(host_pid, starter_pid):
            raise SafetyAbort(f"foreign PID entered scope: {host_pid}")
        try:
            namespace_pids = _process_namespace_pids(host_pid)
            cmdline = _process_cmdline(host_pid)
        except (FileNotFoundError, ProcessLookupError, UnicodeDecodeError):
            continue
        candidates[namespace_pids[-1]] = (host_pid, namespace_pids, cmdline)

    mapped: list[dict[str, Any]] = []
    used_hosts: set[int] = set()
    used_workers: set[str] = set()
    for declaration in declarations:
        if not isinstance(declaration, Mapping):
            raise IsolationError("memory worker declaration is invalid")
        worker_id = declaration.get("worker_id")
        namespace_pid = declaration.get("namespace_pid")
        requested_bytes = declaration.get("requested_bytes")
        if (
            not isinstance(worker_id, str)
            or worker_id in used_workers
            or type(namespace_pid) is not int
            or requested_bytes != MEMORY_WORKER_BYTES
        ):
            raise IsolationError("memory worker declaration is incomplete")
        candidate = candidates.get(namespace_pid)
        if candidate is None:
            raise IsolationError("memory worker namespace PID has no owned host mapping")
        host_pid, namespace_pids, cmdline = candidate
        if host_pid in used_hosts or attempt_id not in cmdline or worker_id not in cmdline:
            raise IsolationError("memory worker host identity does not match its declaration")
        mapped.append({
            "worker_id": worker_id,
            "requested_bytes": requested_bytes,
            "namespace_pid": namespace_pid,
            "namespace_pid_chain": list(namespace_pids),
            "host_pid": host_pid,
            "host_start_identity": process_start_identity(host_pid),
            "cmdline": list(cmdline),
        })
        used_workers.add(worker_id)
        used_hosts.add(host_pid)
    return mapped


def _memory_event_observation(
    cgroup: Path, attempt_id: str, control_group: str
) -> dict[str, Any]:
    observed_at = time.time()
    try:
        return {
            "available": True,
            "observed_at": observed_at,
            "attempt_id": attempt_id,
            "control_group": control_group,
            "value": _event_counters(cgroup, "memory.events"),
        }
    except (OSError, ValueError) as error:
        return {
            "available": False,
            "observed_at": observed_at,
            "attempt_id": attempt_id,
            "control_group": control_group,
            "error": f"{type(error).__name__}: {error}",
        }


def _observe_memory_phase(
    process: subprocess.Popen[str],
    cgroup: Path,
    timeout: float,
    persist: Callable[[list[dict[str, Any]]], None],
    *,
    attempt_id: str,
    control_group: str,
) -> tuple[dict[str, Any], list[dict[str, Any]], float]:
    deadline = time.monotonic() + timeout
    observations: list[dict[str, Any]] = []
    record: dict[str, Any] | None = None
    status: str | None = None
    error: str | None = None
    pending = bytearray()
    offending = _BoundedOffendingBytes()
    stdout = process.stdout
    reading = stdout is not None
    fd: int | None = None
    if stdout is None:
        status = "PIPE_UNAVAILABLE"
        error = "scope output pipe is unavailable"
        reading = False
    else:
        fd = stdout.fileno()
        os.set_blocking(fd, False)

    def persist_observation() -> None:
        observations.append(_memory_event_observation(cgroup, attempt_id, control_group))
        persist(observations)

    def mark_duplicate(extra: bytes) -> None:
        nonlocal status, error, reading
        if extra:
            offending.extend(extra)
        if status in (None, "COMPLETE"):
            status = "DUPLICATE"
            error = "scope workload emitted multiple buffered records"
        reading = False

    def consume_available() -> None:
        nonlocal record, status, error, reading
        if not reading or fd is None:
            return
        while reading:
            try:
                chunk = os.read(fd, 65536)
            except BlockingIOError:
                return
            except OSError as exc:
                status = "ERROR"
                error = f"scope workload output failed: {type(exc).__name__}: {exc}"
                reading = False
                return
            if not chunk:
                reading = False
                if record is None and status is None:
                    if pending:
                        status = "PARTIAL_EOF"
                        error = "scope workload output ended with a partial line"
                        offending.extend(bytes(pending))
                        pending.clear()
                    else:
                        status = "EOF_WITHOUT_RECORD"
                        error = "scope workload ended without evidence"
                return
            if record is not None:
                mark_duplicate(chunk)
                continue
            pending.extend(chunk)
            if b"\n" in pending:
                line_bytes, remainder = bytes(pending).split(b"\n", 1)
                pending.clear()
                if len(line_bytes) > MAX_JSON_LINE_BYTES:
                    status = "OVERFLOW"
                    error = "scope workload evidence exceeds size limit"
                    offending.extend(line_bytes)
                    if remainder:
                        offending.extend(remainder)
                    reading = False
                    return
                try:
                    record = _parse_json_object_line(line_bytes)
                except IsolationError as exc:
                    status = _transport_status_for_parse_error(exc)
                    error = str(exc)
                    offending.extend(line_bytes)
                    if remainder:
                        offending.extend(remainder)
                    reading = False
                    return
                status = "COMPLETE"
                if remainder:
                    mark_duplicate(remainder)
                continue
            if len(pending) > MAX_JSON_LINE_BYTES:
                status = "OVERFLOW"
                error = "scope workload evidence exceeds size limit"
                offending.extend(bytes(pending))
                pending.clear()
                reading = False
                return

    while time.monotonic() < deadline:
        persist_observation()
        consume_available()
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        wait = min(0.05, remaining)
        if reading and fd is not None:
            select.select([fd], [], [], wait)
        else:
            time.sleep(wait)
    if status is None:
        status = "TIMEOUT"
        error = "memory proof reporter timed out"
        if pending:
            offending.extend(bytes(pending))
            pending.clear()
    persist_observation()
    return (
        _reporter_transport_evidence(
            status, record=record, error=error, offending=offending.evidence()
        ),
        observations,
        time.time(),
    )


def _reporter_transport_evidence(
    status: str,
    *,
    record: Mapping[str, Any] | None = None,
    error: str | None = None,
    offending: Mapping[str, Any] | None = None,
) -> dict[str, Any]:
    evidence: dict[str, Any] = {
        "status": status,
        "eligibility": _transport_eligibility_for_status(status),
    }
    if record is not None:
        evidence["record"] = dict(record)
    if error is not None:
        evidence["error"] = error
    if offending is not None:
        evidence["offending"] = dict(offending)
    return evidence


def _kernel_victim_records(
    control_group: str,
    workers: Sequence[Mapping[str, Any]],
    attempt_id: str,
    started_at: float,
    ended_at: float,
) -> dict[str, Any]:
    capture = run_capture(
        [
            "journalctl", "-k", "--no-pager", "--output=short-unix",
            f"--since=@{started_at:.6f}", f"--until=@{ended_at:.6f}",
        ],
        timeout=2,
    )
    stdout = str(capture.get("stdout", ""))
    query = {
        key: capture[key]
        for key in ("command", "exit_code", "error", "elapsed_ms")
        if key in capture
    }
    query["stdout_bytes"] = len(stdout.encode("utf-8", errors="replace"))
    if capture.get("stderr"):
        query["stderr"] = str(capture["stderr"])[:512]
    if capture.get("exit_code") != 0:
        return {"status": "UNAVAILABLE", "query": query, "victims": []}
    worker_pids = {
        int(item["host_pid"])
        for item in workers
        if type(item.get("host_pid")) is int
    }
    records: list[dict[str, Any]] = []
    for line in stdout.splitlines():
        marker = "oom-kill:"
        if marker not in line:
            continue
        timestamp_match = re.match(r"\s*(\d+(?:\.\d+)?)", line)
        if timestamp_match is None:
            continue
        fields: dict[str, str] = {}
        duplicate_field = False
        for part in line.split(marker, 1)[1].split(","):
            key, separator, value = part.strip().partition("=")
            if separator and key:
                if key in fields:
                    duplicate_field = True
                    break
                fields[key] = value.strip()
        if (
            duplicate_field
            or fields.get("constraint") != "CONSTRAINT_MEMCG"
            or fields.get("oom_memcg") != control_group
            or fields.get("task_memcg") != control_group
            or not fields.get("pid", "").isdigit()
        ):
            continue
        host_pid = int(fields["pid"])
        observed_at = float(timestamp_match.group(1))
        if host_pid in worker_pids and started_at <= observed_at <= ended_at:
            records.append({
                "attempt_id": attempt_id,
                "control_group": control_group,
                "host_pid": host_pid,
                "observed_at": observed_at,
                "record": line,
            })
    return {"status": "AVAILABLE", "query": query, "victims": records}


def _capture_terminal_diagnostics(
    manager: UserScopeManager, unit: str, cgroup: Path
) -> dict[str, Any]:
    """Capture bounded post-failure observations without deciding success."""
    diagnostics: dict[str, Any] = {"status": "CAPTURED"}
    for key, name in (
        ("memory_events", "memory.events"),
        ("pids_events", "pids.events"),
    ):
        try:
            diagnostics[key] = {"available": True, "value": _event_counters(cgroup, name)}
        except Exception as error:  # noqa: BLE001 - per-source capture cannot fail the helper.
            diagnostics[key] = {
                "available": False,
                "error": f"{type(error).__name__}: {error}",
            }
    try:
        diagnostics["manager_properties"] = {
            "available": True,
            "value": manager.properties(unit, timeout=1),
        }
    except Exception as error:  # noqa: BLE001 - per-source capture cannot fail the helper.
        diagnostics["manager_properties"] = {
            "available": False,
            "error": f"{type(error).__name__}: {error}",
        }
    return diagnostics


def _is_descendant(pid: int, root_pid: int) -> bool:
    seen: set[int] = set()
    while pid > 1 and pid not in seen:
        if pid == root_pid:
            return True
        seen.add(pid)
        try:
            status = Path(f"/proc/{pid}/status").read_text(encoding="utf-8")
        except FileNotFoundError:
            return False
        match = re.search(r"^PPid:\s+(\d+)$", status, re.MULTILINE)
        if match is None:
            return False
        pid = int(match.group(1))
    return False


@dataclass(frozen=True)
class ResourceBoundary:
    controllers: set[str]
    cpu_count: int
    memory_bytes: int
    scratch_bytes: int
    cpu_limit_enforced: bool = False
    scratch_limit_enforced: bool = False
    child_cgroup_enforceable: bool = False
    source: str = "unknown"
    cgroup_process_count: int = 0
    subtree_control: tuple[str, ...] = ()

    @classmethod
    def detect(cls) -> ResourceBoundary:
        relative = None
        for line in Path("/proc/self/cgroup").read_text(encoding="utf-8").splitlines():
            fields = line.split(":", 2)
            if len(fields) == 3 and fields[0] == "0":
                relative = fields[2]
                break
        if relative is None:
            return cls(set(), 2, 4 * GIB, 2 * GIB, source="no-cgroup-v2")
        cgroup = Path("/sys/fs/cgroup") / relative.lstrip("/")
        try:
            controllers = set(
                (cgroup / "cgroup.controllers").read_text(encoding="ascii").split()
            )
        except OSError:
            controllers = set()
        try:
            subtree_control = tuple(
                (cgroup / "cgroup.subtree_control").read_text(encoding="ascii").split()
            )
            process_count = len(
                (cgroup / "cgroup.procs").read_text(encoding="ascii").splitlines()
            )
        except OSError:
            subtree_control = ()
            process_count = 0
        allowed_cpus = os.sched_getaffinity(0) if hasattr(os, "sched_getaffinity") else set()
        return cls(
            controllers=controllers,
            cpu_count=min(2, len(allowed_cpus)) if allowed_cpus else 0,
            memory_bytes=4 * GIB,
            scratch_bytes=2 * GIB,
            cpu_limit_enforced=bool(allowed_cpus),
            scratch_limit_enforced=False,
            child_cgroup_enforceable=(
                {"memory", "pids"}.issubset(subtree_control)
                and os.access(cgroup, os.W_OK)
            ),
            source=str(cgroup),
            cgroup_process_count=process_count,
            subtree_control=subtree_control,
        )

    def require_enforceable(self) -> None:
        failures: list[str] = []
        missing = {"memory", "pids"} - self.controllers
        if missing:
            failures.append("missing delegated " + ", ".join(sorted(missing)) + " controller")
        if not self.cpu_limit_enforced:
            failures.append("two-CPU affinity limit is not enforceable")
        if self.cpu_count < 1 or self.cpu_count > 2:
            failures.append("CPU boundary exceeds two CPUs")
        if self.memory_bytes < 1 or self.memory_bytes > 4 * GIB:
            failures.append("memory boundary exceeds 4 GiB")
        if self.scratch_bytes < 1 or self.scratch_bytes > 2 * GIB:
            failures.append("scratch boundary exceeds 2 GiB")
        if not self.scratch_limit_enforced:
            failures.append("hard scratch limit is not enforceable")
        if not self.child_cgroup_enforceable:
            failures.append("dedicated child cgroup is not enforceable")
        if failures:
            raise IsolationError("; ".join(failures))


def process_start_identity(pid: int) -> str:
    stat = Path(f"/proc/{pid}/stat").read_text(encoding="ascii")
    close = stat.rfind(")")
    fields = stat[close + 2 :].split() if close >= 0 else []
    if len(fields) < 20:
        raise IsolationError(f"cannot read process start identity: {pid}")
    return fields[19]


@dataclass
class OwnedProcess:
    process: subprocess.Popen[str]
    start_identity: str
    process_group: int
    cleaned: bool = False

    @classmethod
    def start(cls, command: Sequence[str], env: Mapping[str, str]) -> OwnedProcess:
        process = subprocess.Popen(
            list(command),
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=dict(env),
            close_fds=True,
            start_new_session=True,
        )
        return cls(process, process_start_identity(process.pid), os.getpgid(process.pid))

    def _identity_matches(self) -> bool:
        try:
            return process_start_identity(self.process.pid) == self.start_identity
        except (FileNotFoundError, ProcessLookupError):
            return False

    def cleanup(self) -> None:
        if self.cleaned:
            return
        self.cleaned = True
        if self.process.poll() is not None:
            self._close_streams()
            return
        if not self._identity_matches():
            self._close_streams()
            return
        try:
            os.killpg(self.process_group, signal.SIGTERM)
            self.process.wait(timeout=1)
        except subprocess.TimeoutExpired:
            if self._identity_matches():
                os.killpg(self.process_group, signal.SIGKILL)
                self.process.wait(timeout=1)
        except ProcessLookupError:
            return
        finally:
            self._close_streams()

    def _close_streams(self) -> None:
        for stream in (self.process.stdout, self.process.stderr):
            if stream is not None and not stream.closed:
                stream.close()

    def wait(self, timeout: float) -> int | str:
        try:
            result = self.process.wait(timeout=timeout)
            self._close_streams()
            return result
        except subprocess.TimeoutExpired:
            self.cleanup()
            return "TIMEOUT"


@dataclass
class Preflight:
    resource_boundary: ResourceBoundary = field(default_factory=ResourceBoundary.detect)
    policy: IsolationPolicy = field(default_factory=IsolationPolicy)

    def run(self) -> dict[str, Any]:
        try:
            self.resource_boundary.require_enforceable()
        except IsolationError as error:
            return {
                "status": "BLOCKED_ENVIRONMENT",
                "target_graphics": "NOT_STARTED",
                "exact_product_route": "NOT_STARTED",
                "blocker_detail": str(error),
            }
        return {
            "status": "READY_FOR_ISOLATED_BOOTSTRAP",
            "target_graphics": "NOT_STARTED",
            "exact_product_route": "NOT_STARTED",
        }


def diagnostic_classification() -> dict[str, str]:
    return {
        "run_class": "NESTED_SYNTHETIC_DIAGNOSTIC",
        "physical_input_acceptance": "NOT_ESTABLISHED",
    }


def host_identity_snapshot() -> dict[str, Any]:
    protected_names = {
        "gnome-shell",
        "ibus-daemon",
        "udevmon",
        "punto-daemon",
        "punto-tray",
        "punto-session-bridge",
    }
    processes: list[dict[str, Any]] = []
    census_error: str | None = None
    try:
        entries = list(Path("/proc").iterdir())
    except OSError as error:
        entries = []
        census_error = f"{type(error).__name__}: {error}"
    for entry in entries:
        if not entry.name.isdigit():
            continue
        try:
            name = (entry / "comm").read_text(encoding="utf-8").strip()
            canonical_name = name
            if name.startswith("punto-") and name not in protected_names:
                command = (entry / "cmdline").read_bytes().split(b"\0", 1)[0]
                if command:
                    canonical_name = Path(os.fsdecode(command)).name
            if canonical_name in protected_names:
                processes.append(
                    {
                        "pid": int(entry.name),
                        "name": canonical_name,
                        "start_identity": process_start_identity(int(entry.name)),
                    }
                )
        except (FileNotFoundError, ProcessLookupError):
            continue
        except (PermissionError, OSError) as error:
            census_error = f"{type(error).__name__}: {error}"
            break
    sessions = run_capture(["loginctl", "list-sessions", "--no-legend"])
    valid = census_error is None and sessions.get("exit_code") == 0
    return {
        "status": "valid" if valid else "unknown",
        "processes": sorted(processes, key=lambda item: item["pid"]),
        "sessions": sessions,
        "census_error": census_error,
    }


def inventory() -> dict[str, Any]:
    commands = {
        "bubblewrap": ["bwrap", "--version"],
        "unshare": ["unshare", "--version"],
        "xvfb_package": ["dpkg-query", "-W", "-f=${Version}", "xvfb"],
        "gnome_shell": ["gnome-shell", "--version"],
        "dbus": ["dbus-daemon", "--version"],
        "gtk3": ["pkg-config", "--modversion", "gtk+-3.0"],
        "gtk4": ["pkg-config", "--modversion", "gtk4"],
        "namespace_probe": [
            "bwrap", "--unshare-user", "--unshare-pid", "--unshare-ipc",
            "--unshare-net", "--ro-bind", "/usr", "/usr", "--ro-bind",
            "/lib", "/lib", "--ro-bind", "/lib64", "/lib64", "--proc",
            "/proc", "--dev", "/dev", "--tmpfs", "/tmp", "--dir", "/run",
            "/usr/bin/true",
        ],
        "bubblewrap_help": ["bwrap", "--help"],
        "xvfb_help": ["Xvfb", "-help"],
        "gnome_shell_help": ["gnome-shell", "--help"],
        "dbus_run_session_help": ["dbus-run-session", "--help"],
    }
    browser = shutil.which("chromium") or shutil.which("chromium-browser") or shutil.which("google-chrome")
    if browser:
        direct_browser = Path("/opt/google/chrome/chrome")
        commands["browser"] = [
            str(direct_browser if direct_browser.is_file() else Path(browser).resolve()),
            "--version",
        ]
    results = {name: run_capture(command) for name, command in commands.items()}
    results["executables"] = {
        name: str(Path(executable).resolve()) if executable else None
        for name, executable in {
            "bwrap": shutil.which("bwrap"),
            "unshare": shutil.which("unshare"),
            "xvfb": shutil.which("Xvfb"),
            "gnome_shell": shutil.which("gnome-shell"),
            "dbus_run_session": shutil.which("dbus-run-session"),
            "gtk_target_python": shutil.which("python3"),
            "browser": browser,
        }.items()
    }
    required = (
        "bubblewrap", "unshare", "xvfb_package", "gnome_shell", "dbus",
        "gtk3", "browser", "namespace_probe",
    )
    results["required_available"] = all(results.get(name, {}).get("exit_code") == 0 for name in required)
    return results


def approved_artifacts() -> list[ArtifactSpec]:
    return [
        ArtifactSpec(
            "baseline_2_8_5",
            REPO_ROOT / "punto-switcher_2.8.5_amd64.deb",
            "80c977e4b195f73bdbf89c9e2ba8b1c4162a6c318ea74142fedd6010d222ebd4",
            {"usr/local/bin/punto-daemon": "edc37fc25bd16d79e8a1252d1e8a9ef012099b271f8c8b6e7f17176d173cd6a6"},
        ),
        ArtifactSpec(
            "candidate_2_8_14",
            REPO_ROOT / "punto-switcher_2.8.14_amd64.deb",
            "9b4007855477383824c25416fa62ca0a2d5c5071fa45c2c508702d3e4d4cd3e2",
            {
                "usr/bin/punto-daemon": "73dcb9d9b97b38dc17312e25d6afe7982c90e5738f26bb2a7938f477db84624a",
                "usr/libexec/punto-session-bridge": "05c9b756f697915cb0c4a8289caed2bb5fc981c9746997bba8d715c6c9feaa81",
                "usr/bin/punto-tray": "e02b7ae616eefe1e9f1d78e92830c352e62c788a655f44fd812091e8292b75de",
            },
            "778923adb37f06870c6d12e53c193065276d4f26efb5cc6f2f246957d07c01b2",
        ),
    ]


def verify_source_manifest() -> dict[str, Any]:
    manifest = REPO_ROOT / ".cache/codex-thread-workflows/punto-i7-plan-r5/source-manifest.sha256"
    expected_manifest_hash = "81939bb1e32f54176619d4925b8de38f55501f0b953bf3bbd2f50bf98c6e6d73"
    actual_manifest_hash = sha256_file(manifest)
    if actual_manifest_hash != expected_manifest_hash:
        raise IsolationError("candidate source manifest identity mismatch")
    checked = 0
    for line in manifest.read_text(encoding="utf-8").splitlines():
        digest, relative = line.split("  ", 1)
        relative_path = Path(relative)
        if relative_path.is_absolute() or ".." in relative_path.parts:
            raise IsolationError(f"unsafe candidate source path: {relative}")
        path = REPO_ROOT / relative
        if not path.is_file() or path.is_symlink():
            raise IsolationError(f"candidate source is not a regular file: {relative}")
        if sha256_file(path) != digest:
            raise IsolationError(f"candidate source byte mismatch: {relative}")
        checked += 1
    return {"manifest_sha256": actual_manifest_hash, "files_verified": checked}


def workspace_snapshot() -> dict[str, Any]:
    immutable = (
        "protocol/c2c-bfdc-i7-recovery-contract.json",
        "protocol/c2c-bfdc-i7-recovery-r10-plan.md",
        "protocol/c2c-bfdc-i7-recovery-r10-graph.json",
        "tests/recovery/probe.py",
        "tests/recovery/test_probe.py",
        ".cache/codex-thread-workflows/punto-i7-plan-r5/source-manifest.sha256",
        "punto-switcher_2.8.5_amd64.deb",
        "punto-switcher_2.8.14_amd64.deb",
    )
    fingerprints = {}
    for relative in immutable:
        path = REPO_ROOT / relative
        if not path.is_file() or path.is_symlink():
            raise IsolationError(f"missing immutable input: {relative}")
        fingerprints[relative] = sha256_file(path)
    return {
        "head": run_capture(["git", "rev-parse", "HEAD"]),
        "status": run_capture(["git", "status", "--short"]),
        "immutable_sha256": fingerprints,
    }


def _resource_inventory() -> dict[str, Any]:
    commands = {
        "systemd_run_version": ["systemd-run", "--version"],
        "systemd_run_help": ["systemd-run", "--user", "--scope", "--help"],
        "bubblewrap_version": ["bwrap", "--version"],
        "bubblewrap_help": ["bwrap", "--help"],
    }
    results = {name: run_capture(command) for name, command in commands.items()}
    help_text = results["bubblewrap_help"].get("stdout", "")
    results["required_available"] = (
        all(value.get("exit_code") == 0 for value in results.values() if isinstance(value, dict))
        and "--size BYTES" in help_text
        and "--tmpfs DEST" in help_text
        and "--remount-ro DEST" in help_text
    )
    return results


def _r11_identity() -> dict[str, str]:
    base = REPO_ROOT / ".cache/codex-thread-workflows/punto-i7-recovery-r11/final"
    expected = {
        "packet.json": "e8da88a2506f7ffc76bbdede22ecba49dad6df759b45e85149c5ebb6c80c5faf",
        "scoped.patch": "9a97994706228663debf3c5d14d947cc3e78fa7131e17ec0a74c72daca81c08b",
        "manifest.sha256": "29889c8933e922e9af51dd41ab5da4f25f8d418a2cb9767a834a771491cfdd92",
    }
    actual: dict[str, str] = {}
    for name, digest in expected.items():
        path = base / name
        if not path.is_file() or path.is_symlink():
            raise IsolationError(f"revision-11 evidence input missing: {name}")
        actual[name] = sha256_file(path)
        if actual[name] != digest:
            raise IsolationError(f"revision-11 evidence identity mismatch: {name}")
    return actual


def _scope_properties_with_retry(
    manager: UserScopeManager, unit: str, timeout: float
) -> dict[str, str]:
    deadline = time.monotonic() + timeout
    last_error: IsolationError | None = None
    while time.monotonic() < deadline:
        try:
            properties = manager.properties(
                unit, timeout=max(0.05, min(1, deadline - time.monotonic()))
            )
            if properties.get("ControlGroup"):
                return properties
        except IsolationError as error:
            last_error = error
        time.sleep(0.05)
    raise last_error or IsolationError("transient scope did not expose ControlGroup")


def _run_staged_enforcement(
    manager: UserScopeManager,
    process: subprocess.Popen[str],
    control_dir: Path,
    cgroup: Path,
    control_group: str,
    starter_pid: int,
    request: ResourceRequest,
    sandbox_ready: Mapping[str, Any],
    pids_before: Mapping[str, Any],
    attempt_id: str,
    proof: dict[str, Any],
    proof_path: Path,
    remaining: Callable[[float], float],
) -> dict[str, Any]:
    proof["protocol"] = {
        "attempt_id": attempt_id,
        "received": [],
        "accepted": [],
        "rejected": [],
    }
    _persist_received_protocol_record(
        proof, proof_path, sandbox_ready, ENFORCEMENT_PHASES[0], 0
    )
    proof["task_events_before"] = dict(pids_before)
    proof["phase"] = "TASK_BASELINE_PERSISTED"
    write_json(proof_path, proof)
    try:
        ready = _accept_protocol_record(
            sandbox_ready, attempt_id, ENFORCEMENT_PHASES[0], 0
        )
        _required_counter(pids_before, "max")
    except IsolationError as error:
        _persist_rejected_protocol_record(
            proof, proof_path, sandbox_ready, ENFORCEMENT_PHASES[0], 0, error
        )
        raise
    proof["protocol"]["accepted"].append(ready)
    write_json(proof_path, proof)
    _write_protocol_ack(control_dir, attempt_id, ENFORCEMENT_PHASES[0], 0)

    raw_task = _read_json_line(process, remaining(15))
    _persist_received_protocol_record(
        proof, proof_path, raw_task, ENFORCEMENT_PHASES[1], 1
    )
    try:
        pids_after = _event_counters(cgroup, "pids.events")
        proof["task_events_after"] = pids_after
        write_json(proof_path, proof)
        task = _accept_protocol_record(
            raw_task, attempt_id, ENFORCEMENT_PHASES[1], 1
        )
        verify_task_proof(task, pids_before, pids_after)
    except (IsolationError, OSError, ValueError) as error:
        rejected = error if isinstance(error, IsolationError) else IsolationError(str(error))
        if "task_events_after" not in proof:
            proof["task_events_after"] = {
                "available": False,
                "error": f"{type(error).__name__}: {error}",
            }
        _persist_rejected_protocol_record(
            proof, proof_path, raw_task, ENFORCEMENT_PHASES[1], 1, rejected
        )
        if rejected is error:
            raise
        raise rejected from error
    proof["protocol"]["accepted"].append(task)
    proof["phase"] = "TASK_PROOF_PERSISTED"
    write_json(proof_path, proof)
    _write_protocol_ack(control_dir, attempt_id, ENFORCEMENT_PHASES[1], 1)

    raw_scratch = _read_json_line(process, remaining(15))
    _persist_received_protocol_record(
        proof, proof_path, raw_scratch, ENFORCEMENT_PHASES[2], 2
    )
    try:
        scratch = _accept_protocol_record(
            raw_scratch, attempt_id, ENFORCEMENT_PHASES[2], 2
        )
        verify_scratch_proof(
            request, scratch, int(sandbox_ready["scratch_available_bytes"])
        )
    except (IsolationError, KeyError, TypeError, ValueError) as error:
        rejected = (
            error if isinstance(error, IsolationError) else IsolationError(str(error))
        )
        _persist_rejected_protocol_record(
            proof, proof_path, raw_scratch, ENFORCEMENT_PHASES[2], 2, rejected
        )
        if rejected is error:
            raise
        raise rejected from error
    proof["protocol"]["accepted"].append(scratch)
    proof["phase"] = "SCRATCH_PROOF_PERSISTED"
    write_json(proof_path, proof)
    _write_protocol_ack(control_dir, attempt_id, ENFORCEMENT_PHASES[2], 2)

    raw_restored = _read_json_line(process, remaining(10))
    _persist_received_protocol_record(
        proof, proof_path, raw_restored, ENFORCEMENT_PHASES[3], 3
    )
    try:
        restored = _accept_protocol_record(
            raw_restored, attempt_id, ENFORCEMENT_PHASES[3], 3
        )
        verify_scratch_restored(
            restored, int(sandbox_ready["scratch_available_bytes"])
        )
    except (IsolationError, KeyError, TypeError, ValueError) as error:
        rejected = (
            error if isinstance(error, IsolationError) else IsolationError(str(error))
        )
        _persist_rejected_protocol_record(
            proof, proof_path, raw_restored, ENFORCEMENT_PHASES[3], 3, rejected
        )
        if rejected is error:
            raise
        raise rejected from error
    proof["protocol"]["accepted"].append(restored)
    proof["phase"] = "SCRATCH_RESTORATION_PERSISTED"
    write_json(proof_path, proof)
    _write_protocol_ack(control_dir, attempt_id, ENFORCEMENT_PHASES[3], 3)

    raw_prepared = _read_json_line(process, remaining(10))
    _persist_received_protocol_record(
        proof, proof_path, raw_prepared, ENFORCEMENT_PHASES[4], 4
    )
    try:
        prepared = _accept_protocol_record(
            raw_prepared, attempt_id, ENFORCEMENT_PHASES[4], 4
        )
        workers = _map_memory_workers(
            manager, control_group, starter_pid, prepared, attempt_id
        )
        proof["memory_workers"] = workers
        write_json(proof_path, proof)
        memory_before = _event_counters(cgroup, "memory.events")
        proof["memory_events_before"] = memory_before
        write_json(proof_path, proof)
        _required_counter(memory_before, "oom_kill")
    except (IsolationError, OSError, ValueError) as error:
        rejected = error if isinstance(error, IsolationError) else IsolationError(str(error))
        if "memory_events_before" not in proof:
            proof["memory_events_before"] = {
                "available": False,
                "error": f"{type(error).__name__}: {error}",
            }
        _persist_rejected_protocol_record(
            proof, proof_path, raw_prepared, ENFORCEMENT_PHASES[4], 4, rejected
        )
        if rejected is error:
            raise
        raise rejected from error
    proof["protocol"]["accepted"].append(prepared)
    proof["phase"] = "MEMORY_PREPARATION_PERSISTED"
    write_json(proof_path, proof)
    release_started_at = time.time()
    _write_protocol_ack(control_dir, attempt_id, ENFORCEMENT_PHASES[4], 4)
    proof["memory_release_started_at"] = release_started_at
    proof["phase"] = "MEMORY_RELEASED"
    write_json(proof_path, proof)

    def persist_observations(observations: list[dict[str, Any]]) -> None:
        proof["memory_event_observations"] = list(observations)
        write_json(proof_path, proof)

    transport, observations, observation_ended_at = _observe_memory_phase(
        process,
        cgroup,
        remaining(20),
        persist_observations,
        attempt_id=attempt_id,
        control_group=control_group,
    )
    proof["memory_reporter_transport"] = transport
    if transport.get("error"):
        proof["memory_reporter_error"] = transport["error"]
    write_json(proof_path, proof)
    reporter: dict[str, Any] | None = None
    raw_reporter = transport.get("record")
    if transport.get("eligibility") == "REPORTER" and isinstance(raw_reporter, Mapping):
        _persist_received_protocol_record(
            proof, proof_path, raw_reporter, ENFORCEMENT_PHASES[5], 5
        )
        try:
            reporter = _accept_protocol_record(
                raw_reporter, attempt_id, ENFORCEMENT_PHASES[5], 5
            )
        except IsolationError as error:
            _persist_rejected_protocol_record(
                proof, proof_path, raw_reporter, ENFORCEMENT_PHASES[5], 5, error
            )
            raise
        proof["protocol"]["accepted"].append(reporter)
    kernel_evidence = _kernel_victim_records(
        control_group,
        workers,
        attempt_id,
        release_started_at,
        observation_ended_at,
    )
    proof["kernel_victim_evidence"] = kernel_evidence
    proof["memory_observation_ended_at"] = observation_ended_at
    write_json(proof_path, proof)
    try:
        verify_memory_proof(
            memory_before,
            observations,
            workers,
            transport,
            kernel_evidence,
            attempt_id=attempt_id,
            control_group=control_group,
            release_started_at=release_started_at,
            observation_ended_at=observation_ended_at,
        )
    except IsolationError as error:
        if reporter is not None:
            proof["protocol"]["accepted"].pop()
            _persist_rejected_protocol_record(
                proof, proof_path, reporter, ENFORCEMENT_PHASES[5], 5, error
            )
        elif transport.get("eligibility") == "REJECT":
            rejected_record = (
                dict(raw_reporter)
                if isinstance(raw_reporter, Mapping)
                else {"transport": dict(transport)}
            )
            _persist_rejected_protocol_record(
                proof, proof_path, rejected_record, ENFORCEMENT_PHASES[5], 5, error
            )
        raise
    proof["phase"] = "MEMORY_PROOF_PERSISTED"
    write_json(proof_path, proof)
    if reporter is not None:
        _write_protocol_ack(control_dir, attempt_id, ENFORCEMENT_PHASES[5], 5)
    return {
        "kind": "staged-workload-result",
        "mode": "enforcement",
        "task": task,
        "scratch": scratch,
        "scratch_restored": restored,
        "memory_prepared": prepared,
        "memory_reporter": reporter,
    }


def _run_resource_scope(
    manager: UserScopeManager,
    evidence_root: Path,
    label: str,
    request: ResourceRequest,
    mode: str,
    *,
    attempt: dict[str, Any] | None = None,
) -> dict[str, Any]:
    scope_deadline = time.monotonic() + SCOPE_WATCHDOG_SECONDS

    def remaining(maximum: float) -> float:
        value = scope_deadline - time.monotonic()
        if value <= 0:
            raise IsolationError("60-second scope watchdog expired")
        return min(maximum, value)

    control_dir = evidence_root / label / "control"
    control_dir.mkdir(mode=0o700, parents=True)
    token = secrets.token_hex(6)
    unit = f"{SCOPE_PREFIX}{token}.scope"
    proof = attempt if attempt is not None else {}
    proof.update({
        "label": label,
        "unit": unit,
        "request": request.__dict__,
        "mode": mode,
        "phase": "STARTING",
        "status": "IN_PROGRESS",
    })
    write_json(evidence_root / label / "proof.json", proof)
    available = sorted(os.sched_getaffinity(0))
    if not available:
        raise IsolationError("no CPUs are available for scope affinity")
    cpus = available[: request.cpu_count]
    command = [
        sys.executable, str(Path(__file__).resolve()), "_scope-worker",
        "--control-dir", str(control_dir), "--mode", mode,
        "--scratch-bytes", str(request.scratch_bytes),
        "--cpus", ",".join(str(cpu) for cpu in cpus),
        "--attempt-id", token,
    ]
    process: subprocess.Popen[str] | None = None
    scope: TransientScope | None = None
    primary_error: Exception | None = None
    diagnostic_error: Exception | None = None
    cleanup_error: Exception | None = None
    cgroup: Path | None = None
    try:
        process = manager.start(unit, request, command)
        proof["phase"] = "MANAGER_STARTED"
        write_json(evidence_root / label / "proof.json", proof)
        starter = _wait_for_file(control_dir / "starter-ready.json", process, remaining(5))
        proof["starter_raw"] = starter
        starter_pid = int(starter["pid"])
        if process_start_identity(starter_pid) != starter["start_identity"]:
            raise IsolationError("scope starter identity changed before verification")
        properties = _scope_properties_with_retry(manager, unit, remaining(5))
        proof["manager_properties_raw"] = properties
        control_group = properties["ControlGroup"]
        cgroup = _cgroup_path(control_group)
        scope = TransientScope(
            manager, unit, process, control_group, starter_pid, starter["start_identity"]
        )
        members_before = manager.scope_members(control_group)
        proof["members_before_launch_raw"] = list(members_before)
        if members_before != (starter_pid,):
            foreign = sorted(set(members_before) - {starter_pid})
            detail = foreign or list(members_before)
            raise SafetyAbort(f"foreign PID in scope before launch: {detail}")
        cpu_quota, cpu_period = _cpu_max(cgroup)
        kernel = {
            "cpu_max": [cpu_quota, cpu_period],
            "memory_max": _read_limit(cgroup / "memory.max"),
            "memory_swap_max": _read_limit(cgroup / "memory.swap.max"),
            "pids_max": _read_limit(cgroup / "pids.max"),
        }
        proof["kernel_readback_raw"] = kernel
        if any(properties.get(name, "infinity") == "infinity" for name in (
            "CPUQuotaPerSecUSec", "MemoryMax", "MemorySwapMax", "TasksMax"
        )):
            raise IsolationError("manager property readback contains an unset limit")
        kernel_observation = ScopeObservation(
            control_group=control_group,
            member_pids=members_before,
            cpu_quota=cpu_quota,
            cpu_period=cpu_period,
            affinity=tuple(int(cpu) for cpu in starter["affinity"]),
            memory_max=kernel["memory_max"],
            memory_swap_max=kernel["memory_swap_max"],
            tasks_max=kernel["pids_max"],
            scratch_bytes=None,
            scratch_inode_count=None,
            writable_mounts=(),
        )
        proof.update({
            "control_group": control_group,
            "starter": starter,
            "members_before_launch": list(members_before),
            "manager_properties": properties,
            "kernel_readback": kernel,
        })
        write_launch_marker(control_dir, request, kernel_observation, starter_pid, unit)
        sandbox_ready = _read_json_line(process, remaining(10))
        proof["sandbox_readback_raw"] = sandbox_ready
        mounts = parse_mountinfo(str(sandbox_ready.get("mountinfo", "")))
        proof["sandbox_mounts_parsed"] = mounts
        verify_sandbox_observation(request, sandbox_ready, mounts)
        participants = manager.scope_members(control_group)
        proof["participants_after_sandbox_start_raw"] = list(participants)
        foreign = [pid for pid in participants if not _is_descendant(pid, starter_pid)]
        if foreign:
            raise SafetyAbort(f"foreign PID entered scope: {foreign}")
        pids_before = _event_counters(cgroup, "pids.events")
        proof_path = evidence_root / label / "proof.json"
        if mode == "enforcement":
            proof["pids_events_before_raw"] = pids_before
            proof["phase"] = "TASK_BASELINE_CAPTURED"
            write_json(proof_path, proof)
            workload = _run_staged_enforcement(
                manager,
                process,
                control_dir,
                cgroup,
                control_group,
                starter_pid,
                request,
                sandbox_ready,
                pids_before,
                token,
                proof,
                proof_path,
                remaining,
            )
        else:
            memory_before = _event_counters(cgroup, "memory.events")
            proof.update({
                "memory_events_before_raw": memory_before,
                "pids_events_before_raw": pids_before,
                "phase": "BASELINES_PERSISTED",
            })
            write_json(proof_path, proof)
            (control_dir / "workload-go").write_text("go\n", encoding="ascii")
            proof["phase"] = "WORKLOAD_RELEASED"
            write_json(proof_path, proof)
            workload = _read_json_line(process, remaining(45))
        proof["workload_raw"] = workload
        try:
            sandbox_exit = _wait_for_file(
                control_dir / "sandbox-exit.json", process, remaining(5)
            )
        except IsolationError as error:
            if mode != "enforcement":
                raise
            sandbox_exit = {
                "available": False,
                "error": str(error),
                "accepted_only_after_staged_memory_verdict": True,
            }
        proof["sandbox_exit_raw"] = sandbox_exit
        if sandbox_exit.get("exit_code") != 0 and mode == "production":
            raise IsolationError(f"bounded sandbox failed: {sandbox_exit}")
        if mode == "production":
            verify_production_smoke(workload)
            memory_after = _event_counters(cgroup, "memory.events")
            pids_after = _event_counters(cgroup, "pids.events")
            proof.update({
                "participants_after_sandbox_start": list(participants),
                "workload": workload,
                "memory_events_after_raw": memory_after,
                "pids_events_after_raw": pids_after,
                "memory_events_before": memory_before,
                "memory_events_after": memory_after,
                "pids_events_before": pids_before,
                "pids_events_after": pids_after,
                "phase": "SMOKE_COMPLETED",
            })
            write_json(evidence_root / label / "proof.json", proof)
            (control_dir / "finish-go").write_text("go\n", encoding="ascii")
            escaped = _wait_for_file(
                control_dir / "escaped-child.json", process, remaining(5)
            )
            proof["escaped_child_raw"] = escaped
            write_json(evidence_root / label / "proof.json", proof)
            if process is None:
                raise IsolationError("owned starter Popen is missing")
            if scope is None:
                raise IsolationError("owned transient scope is missing")
            proof["starter_exit"] = wait_for_owned_starter_exit(
                process, starter, remaining(5)
            )
            proof["phase"] = "STARTER_EXIT_REAPED"
            write_json(evidence_root / label / "proof.json", proof)
            members_after_exit = manager.scope_members(control_group)
            proof["members_after_starter_exit"] = list(members_after_exit)
            proof["phase"] = "POST_EXIT_MEMBERSHIP"
            write_json(evidence_root / label / "proof.json", proof)
            try:
                escaped_pid = int(escaped["pid"])
            except (KeyError, TypeError, ValueError) as error:
                raise IsolationError("escaped child identity record is invalid") from error
            observation = observe_production_survivor_stat(escaped_pid)
            proof["survivor_state"] = {
                "pid": observation["pid"],
                "state": observation["state"],
                "start_identity": observation["start_identity"],
            }
            proof["phase"] = "SURVIVOR_STATE_OBSERVED"
            write_json(evidence_root / label / "proof.json", proof)
            cleanup_target = production_survivor_cleanup_target(
                escaped, members_after_exit, observation
            )
            scope.survivor = cleanup_target
            proof["leader_exit_cleanup_case"] = cleanup_target
            proof["phase"] = "SURVIVOR_CLEANUP_TARGET"
            write_json(evidence_root / label / "proof.json", proof)
            survivor = accept_production_survivor(
                escaped, members_after_exit, observation=observation
            )
            proof["phase"] = "SURVIVOR_IDENTITY_ACCEPTED"
            write_json(evidence_root / label / "proof.json", proof)
            verify_production_relationship(starter, survivor)
            proof["phase"] = "RELATIONSHIP_VERIFIED"
            write_json(evidence_root / label / "proof.json", proof)
        else:
            proof.update({
                "participants_after_sandbox_start": list(participants),
                "workload": workload,
            })
            (control_dir / "finish-go").write_text("go\n", encoding="ascii")
            process.wait(timeout=remaining(3))
        proof.update({"phase": "WORKLOAD_VERIFIED", "status": "WORKLOAD_VERIFIED"})
        write_json(evidence_root / label / "proof.json", proof)
    except Exception as error:  # noqa: BLE001 - cleanup must run for every primary failure.
        primary_error = (
            error
            if isinstance(error, IsolationError)
            else IsolationError(f"scope proof failed: {type(error).__name__}: {error}")
        )
        proof.update({
            "status": "FAILED",
            "failure_phase": proof.get("phase", "UNKNOWN"),
            "primary_error": str(primary_error),
            "phase": "TERMINAL_DIAGNOSTICS",
        })
        try:
            if cgroup is None:
                proof["terminal_diagnostics"] = {
                    "status": "UNAVAILABLE",
                    "reason": "ControlGroup ownership was not established",
                }
            else:
                proof["terminal_diagnostics"] = _capture_terminal_diagnostics(
                    manager, unit, cgroup
                )
        except Exception as error:  # noqa: BLE001 - diagnostics cannot block cleanup.
            diagnostic_error = error
            proof["diagnostic_error"] = f"{type(error).__name__}: {error}"
        write_json(evidence_root / label / "proof.json", proof)
    finally:
        if scope is not None:
            try:
                cleanup_state = scope.cleanup()
            except Exception as error:  # noqa: BLE001 - uncertain cleanup is a safety abort.
                cleanup_error = error
                cleanup_state = manager.scope_state(scope.control_group)
        else:
            try:
                manager.kill_scope(unit)
            except Exception as error:  # noqa: BLE001 - uncertain cleanup is a safety abort.
                cleanup_error = error
            try:
                if process is not None and process.poll() is None:
                    process.terminate()
                    process.wait(timeout=CLEANUP_DEADLINE_SECONDS)
            except Exception as error:  # noqa: BLE001 - uncertain cleanup is a safety abort.
                cleanup_error = cleanup_error or error
            cleanup_error = cleanup_error or IsolationError(
                "scope cleanup evidence unavailable before ControlGroup ownership was established"
            )
            cleanup_state = {
                "members": (),
                "populated": None,
                "unknown": True,
                "survivor_gone": None,
            }
        cleanup_unknown = bool(cleanup_state.get("unknown"))
        if cleanup_error is None and cleanup_unknown:
            cleanup_error = IsolationError("scope cleanup membership is unknown")
        proof["cleanup"] = {
            "complete": cleanup_error is None,
            "error": str(cleanup_error) if cleanup_error else None,
            "members_after": list(cleanup_state.get("members", ())),
            "populated_after": cleanup_state.get("populated"),
            "unknown": cleanup_unknown,
            "survivor_gone": cleanup_state.get("survivor_gone"),
        }
        if primary_error is not None:
            proof["primary_error"] = str(primary_error)
        if diagnostic_error is not None:
            proof["diagnostic_error"] = f"{type(diagnostic_error).__name__}: {diagnostic_error}"
        if cleanup_error is not None or cleanup_unknown:
            proof["status"] = "ABORTED_SAFETY"
            proof["phase"] = "CLEANUP_FAILED"
        else:
            proof["phase"] = "CLEANUP_COMPLETE"
            if primary_error is None:
                proof["status"] = "VERIFIED"
        write_json(evidence_root / label / "proof.json", proof)
    finalize_scope_errors(proof, primary_error, cleanup_error)
    return proof


def _run_scope_sequence(
    manager: UserScopeManager,
    evidence_root: Path,
    before: Mapping[str, Any],
    *,
    runner: Callable[..., dict[str, Any]] = _run_resource_scope,
    snapshot: Callable[[], dict[str, Any]] = host_identity_snapshot,
    results: list[dict[str, Any]] | None = None,
) -> list[dict[str, Any]]:
    completed = results if results is not None else []
    first_request = ResourceRequest(256 * 1024**2, 32, 16 * 1024**2)
    first: dict[str, Any] = {
        "label": "scope-small",
        "request": first_request.__dict__,
        "mode": "enforcement",
        "phase": "REGISTERED",
        "status": "IN_PROGRESS",
    }
    completed.append(first)
    first = runner(
        manager,
        evidence_root,
        "scope-small",
        first_request,
        "enforcement",
        attempt=first,
    )
    intermediate = snapshot()
    write_json(evidence_root / "host-between-scopes.json", intermediate)
    assert_ready_for_next_scope(first, before, intermediate)
    second_request = ResourceRequest(4 * GIB, 256, 2 * GIB)
    second: dict[str, Any] = {
        "label": "scope-production",
        "request": second_request.__dict__,
        "mode": "production",
        "phase": "REGISTERED",
        "status": "IN_PROGRESS",
    }
    completed.append(second)
    second = runner(
        manager,
        evidence_root,
        "scope-production",
        second_request,
        "production",
        attempt=second,
    )
    return completed


def execute_resource_proof(evidence_root: Path) -> dict[str, Any]:
    evidence_root = validate_evidence_root(evidence_root)
    if evidence_root.exists():
        raise IsolationError(f"evidence root already exists: {evidence_root}")
    evidence_root.mkdir(mode=0o700, parents=True)
    before = host_identity_snapshot()
    write_json(evidence_root / "host-before.json", before)
    write_json(evidence_root / "workspace.json", workspace_snapshot())
    write_json(evidence_root / "r11-identity.json", _r11_identity())
    tools = _resource_inventory()
    write_json(evidence_root / "inventory.json", tools)
    result: dict[str, Any] = {
        "task_id": TASK_ID,
        "iteration": ITERATION,
        "plan_revision": PLAN_REVISION,
        "resource_boundary": "NOT_VERIFIED",
        "target_graphics": "NOT_STARTED",
        "exact_product_route": "NOT_STARTED",
    }
    scopes: list[dict[str, Any]] = []
    safety_abort = False
    try:
        if not snapshot_is_valid(before):
            raise SafetyAbort("initial protected-host observation is invalid or unknown")
        if not tools["required_available"]:
            raise IsolationError("required systemd/bubblewrap capabilities are unavailable")
        manager = UserScopeManager()
        _run_scope_sequence(manager, evidence_root, before, results=scopes)
        result.update({"status": "VERIFIED", "resource_boundary": "VERIFIED"})
    except SafetyAbort as error:
        result.update({
            "status": "ABORTED_SAFETY",
            "resource_boundary": "NOT_VERIFIED",
            "blocker_detail": str(error),
        })
        safety_abort = True
    except IsolationError as error:
        result.update({"status": "BLOCKED_ENVIRONMENT", "blocker_detail": str(error)})
    finally:
        after = host_identity_snapshot()
        postflight = compare_protected_identity(before, after)
        if (
            postflight["status"] != "valid"
            or not postflight["protected_processes_unchanged"]
            or not postflight["protected_sessions_unchanged"]
        ):
            safety_abort = True
            result.update({
                "status": "ABORTED_SAFETY",
                "resource_boundary": "NOT_VERIFIED",
                "blocker_detail": result.get("blocker_detail")
                or "protected-host postflight is invalid, unknown, or changed",
            })
        result["scopes"] = scopes
        result["cleanup_postflight"] = "ABORTED_SAFETY" if safety_abort else "PASS"
        write_json(evidence_root / "postflight.json", postflight)
        write_json(evidence_root / "result.json", result)
    return result


def execute_preflight(evidence_root: Path) -> dict[str, Any]:
    if evidence_root.exists():
        raise IsolationError(f"evidence root already exists: {evidence_root}")
    evidence_root.mkdir(mode=0o700, parents=True)
    write_json(evidence_root / "workspace.json", workspace_snapshot())
    before = host_identity_snapshot()
    write_json(evidence_root / "host-before.json", before)
    tools = inventory()
    write_json(evidence_root / "inventory.json", tools)

    artifacts: list[dict[str, Any]] = []
    try:
        source = verify_source_manifest()
        for spec in approved_artifacts():
            artifacts.append(
                spec.verify_and_extract(evidence_root / "payloads" / spec.artifact_id)
            )
        artifact_result: dict[str, Any] = {"source": source, "artifacts": artifacts}
    except IsolationError as error:
        artifact_result = {"error": str(error), "artifacts": artifacts}
    write_json(evidence_root / "artifacts.json", artifact_result)

    boundary = ResourceBoundary.detect()
    result = Preflight(boundary).run()
    if artifact_result.get("error"):
        result = {
            "status": "BLOCKED_IDENTITY",
            "target_graphics": "NOT_STARTED",
            "exact_product_route": "NOT_STARTED",
            "blocker_detail": artifact_result["error"],
        }
    elif not tools["required_available"]:
        result = {
            "status": "BLOCKED_ENVIRONMENT",
            "target_graphics": "NOT_STARTED",
            "exact_product_route": "NOT_STARTED",
            "blocker_detail": "one or more required installed runtimes are unavailable",
        }
    result.update(
        {
            "task_id": TASK_ID,
            "iteration": ITERATION,
            "plan_revision": PLAN_REVISION,
            "resource_boundary": {
                "controllers": sorted(boundary.controllers),
                "cpu_count": boundary.cpu_count,
                "memory_bytes": boundary.memory_bytes,
                "scratch_bytes": boundary.scratch_bytes,
                "cpu_limit_enforced": boundary.cpu_limit_enforced,
                "scratch_limit_enforced": boundary.scratch_limit_enforced,
                "child_cgroup_enforceable": boundary.child_cgroup_enforceable,
                "source": boundary.source,
                "cgroup_process_count": boundary.cgroup_process_count,
                "subtree_control": list(boundary.subtree_control),
            },
            **diagnostic_classification(),
        }
    )
    after = host_identity_snapshot()
    postflight = compare_protected_identity(before, after)
    write_json(evidence_root / "postflight.json", postflight)
    result["cleanup_postflight"] = (
        "PASS"
        if postflight["status"] == "valid"
        and postflight["protected_processes_unchanged"]
        and postflight["protected_sessions_unchanged"]
        else "ABORTED_SAFETY"
    )
    write_json(evidence_root / "result.json", result)
    return result


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("action", choices=("preflight", "resource-proof", "_scope-worker"))
    parser.add_argument("--evidence-root", type=Path, default=DEFAULT_EVIDENCE_ROOT)
    parser.add_argument("--control-dir", type=Path)
    parser.add_argument("--mode", choices=("enforcement", "production"))
    parser.add_argument("--scratch-bytes", type=int)
    parser.add_argument("--cpus")
    parser.add_argument("--attempt-id")
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    arguments = parse_args(argv or sys.argv[1:])
    try:
        if arguments.action == "_scope-worker":
            if None in (
                arguments.control_dir,
                arguments.mode,
                arguments.scratch_bytes,
                arguments.cpus,
                arguments.attempt_id,
            ):
                raise IsolationError("scope worker arguments are incomplete")
            cpus = [int(value) for value in arguments.cpus.split(",")]
            return scope_worker(
                arguments.control_dir,
                arguments.mode,
                arguments.scratch_bytes,
                cpus,
                arguments.attempt_id,
            )
        if arguments.action == "resource-proof":
            result = execute_resource_proof(arguments.evidence_root)
        else:
            result = execute_preflight(arguments.evidence_root.resolve())
    except IsolationError as error:
        print(json.dumps({"status": "BLOCKED_ENVIRONMENT", "error": str(error)}))
        return 2
    print(json.dumps(result, ensure_ascii=False, sort_keys=True))
    return 0 if result["status"] in {"READY_FOR_ISOLATED_BOOTSTRAP", "VERIFIED"} else 2


if __name__ == "__main__":
    raise SystemExit(main())
