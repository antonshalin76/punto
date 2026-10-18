import hashlib
import os
import signal
import subprocess
import sys
import tempfile
import unittest
from contextlib import ExitStack
from pathlib import Path
from unittest import mock

RECOVERY_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(RECOVERY_DIR))

import isolated_target
from isolated_target import (
    MEMORY_WORKER_BYTES,
    PRODUCTION_SMOKE_BYTES,
    PRODUCTION_SMOKE_SHA256,
    ArtifactSpec,
    IsolationError,
    IsolationPolicy,
    OwnedProcess,
    Preflight,
    ResourceBoundary,
    ResourceRequest,
    SafetyAbort,
    ScopeObservation,
    TransientScope,
    UserScopeManager,
    _accept_protocol_record,
    _bwrap_command,
    _capture_terminal_diagnostics,
    _kernel_victim_records,
    _map_memory_workers,
    _observe_memory_phase,
    _read_json_line,
    _reporter_transport_evidence,
    _run_resource_scope,
    _run_scope_sequence,
    _run_staged_enforcement,
    accept_production_survivor,
    compare_protected_identity,
    diagnostic_classification,
    finalize_scope_errors,
    observe_production_survivor_stat,
    parse_mountinfo,
    parse_production_survivor_stat,
    production_survivor_cleanup_target,
    validate_evidence_root,
    verify_memory_proof,
    verify_production_relationship,
    verify_production_smoke,
    verify_scope_observation,
    verify_scratch_proof,
    verify_task_proof,
    verify_writable_mounts,
    wait_for_owned_starter_exit,
    write_launch_marker,
)

RECORDED_R13_MOUNTS = (
    {"target": "/", "filesystem": "tmpfs", "writable": True},
    {"target": "/usr", "filesystem": "ext4", "writable": False},
    {"target": "/proc", "filesystem": "proc", "writable": True},
    {"target": "/control", "filesystem": "ext4", "writable": False},
)


class ArtifactSpecTests(unittest.TestCase):
    def test_hash_mismatch_stops_before_extraction(self):
        with tempfile.TemporaryDirectory() as tmp:
            package = Path(tmp) / "candidate.deb"
            package.write_bytes(b"not-the-approved-package")
            spec = ArtifactSpec("candidate", package, "0" * 64, {})
            with (
                mock.patch("subprocess.run") as run,
                self.assertRaisesRegex(IsolationError, "package hash mismatch"),
            ):
                spec.verify_and_extract(Path(tmp) / "root")
            run.assert_not_called()

    def test_verified_package_hash_is_reported(self):
        with tempfile.TemporaryDirectory() as tmp:
            package = Path(tmp) / "candidate.deb"
            package.write_bytes(b"approved")
            expected = hashlib.sha256(b"approved").hexdigest()
            spec = ArtifactSpec("candidate", package, expected, {})
            with mock.patch("subprocess.run") as run:
                run.return_value = subprocess.CompletedProcess([], 0, "", "")
                result = spec.verify_and_extract(Path(tmp) / "root")
            self.assertEqual(result["package_sha256"], expected)
            run.assert_called_once()

    def test_payload_manifest_mismatch_blocks_artifact(self):
        with tempfile.TemporaryDirectory() as tmp:
            package = Path(tmp) / "candidate.deb"
            package.write_bytes(b"approved")
            expected = hashlib.sha256(b"approved").hexdigest()
            spec = ArtifactSpec("candidate", package, expected, {}, "f" * 64)
            with mock.patch("subprocess.run") as run:
                run.return_value = subprocess.CompletedProcess([], 0, "", "")
                with self.assertRaisesRegex(IsolationError, "payload manifest mismatch"):
                    spec.verify_and_extract(Path(tmp) / "root")


class IsolationPolicyTests(unittest.TestCase):
    def setUp(self):
        self.policy = IsolationPolicy()

    def test_rejects_host_display_and_bus_in_child_environment(self):
        with self.assertRaisesRegex(IsolationError, "DISPLAY"):
            self.policy.validate_child_environment({"DISPLAY": ":0"})
        with self.assertRaisesRegex(IsolationError, "DBUS_SESSION_BUS_ADDRESS"):
            self.policy.validate_child_environment(
                {"DBUS_SESSION_BUS_ADDRESS": "unix:path=/run/user/1000/bus"}
            )

    def test_rejects_host_runtime_and_device_mounts(self):
        for source in (
            "/run/systemd",
            "/run/user/1000/bus",
            "/tmp/.X11-unix/X0",
            "/dev/input",
            "/dev/uinput",
            "/dev/dri",
            "/dev/tty2",
            "/dev/snd",
        ):
            with self.subTest(source=source), self.assertRaises(IsolationError):
                self.policy.validate_mount_sources([Path(source)])

    def test_rejects_full_root_bind(self):
        with self.assertRaisesRegex(IsolationError, "root bind"):
            self.policy.validate_mount_sources([Path("/")])

    def test_rejects_symlink_alias_to_forbidden_mount(self):
        with tempfile.TemporaryDirectory() as tmp:
            alias = Path(tmp) / "bus"
            alias.symlink_to("/run/user")
            with self.assertRaisesRegex(IsolationError, "forbidden host mount"):
                self.policy.validate_mount_sources([alias])

    def test_rejects_additional_writable_mount(self):
        with self.assertRaisesRegex(IsolationError, "unexpected writable mount"):
            verify_writable_mounts(
                [*RECORDED_R13_MOUNTS, {"target": "/sys", "filesystem": "sysfs", "writable": True}]
            )

    def test_accepts_recorded_resource_proof_mount_table(self):
        verify_writable_mounts(RECORDED_R13_MOUNTS)

    def test_rejects_formerly_exempt_and_duplicate_mounts(self):
        for mounts, message in (
            (
                [*RECORDED_R13_MOUNTS, {"target": "/sys", "filesystem": "sysfs", "writable": True}],
                "unexpected writable mount",
            ),
            ([*RECORDED_R13_MOUNTS, RECORDED_R13_MOUNTS[0]], "duplicate mount"),
        ):
            with self.subTest(message=message), self.assertRaisesRegex(IsolationError, message):
                verify_writable_mounts(mounts)

    def test_evidence_root_is_confined_and_rejects_symlink_escape(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp) / "r13"
            base.mkdir()
            outside = Path(tmp) / "outside"
            outside.mkdir()
            (base / "alias").symlink_to(outside, target_is_directory=True)
            with self.assertRaisesRegex(IsolationError, "symlink"):
                validate_evidence_root(base / "alias" / "proof", base)
            with self.assertRaisesRegex(IsolationError, "outside revision-20"):
                validate_evidence_root(outside / "proof", base)

    def test_evidence_root_rejects_traversal_and_base_itself(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp) / "r13"
            base.mkdir()
            with self.assertRaisesRegex(IsolationError, "lexical"):
                validate_evidence_root(base / "proof" / ".." / "escape", base)
            with self.assertRaisesRegex(IsolationError, "subdirectory"):
                validate_evidence_root(base, base)

    def test_read_only_bind_does_not_inherit_superblock_rw(self):
        mounts = parse_mountinfo(
            "36 25 0:32 / / ro,relatime - ext4 /dev/root rw,errors=remount-ro\n"
        )
        self.assertFalse(mounts[0]["writable"])

    def test_bwrap_uses_one_bounded_tmpfs_root(self):
        with tempfile.TemporaryDirectory() as tmp:
            command = _bwrap_command(Path(tmp), "production", 4096)
        self.assertEqual(command.count("--tmpfs"), 1)
        index = command.index("--tmpfs")
        self.assertEqual(command[index + 1], "/")
        self.assertEqual(command[command.index("--size") + 1], "4096")


class ResourceBoundaryTests(unittest.TestCase):
    def test_missing_cpu_limiter_blocks_gui_spawn(self):
        boundary = ResourceBoundary(
            controllers={"memory", "pids"},
            cpu_count=2,
            memory_bytes=4 * 1024**3,
            scratch_bytes=2 * 1024**3,
        )
        with self.assertRaisesRegex(IsolationError, "CPU affinity"):
            boundary.require_enforceable()

    def test_complete_boundary_is_accepted(self):
        boundary = ResourceBoundary(
            controllers={"memory", "pids"},
            cpu_count=2,
            memory_bytes=4 * 1024**3,
            scratch_bytes=2 * 1024**3,
            cpu_limit_enforced=True,
            scratch_limit_enforced=True,
            child_cgroup_enforceable=True,
        )
        boundary.require_enforceable()


class ScopeObservationTests(unittest.TestCase):
    def setUp(self):
        self.request = ResourceRequest(
            memory_bytes=256 * 1024**2,
            tasks_max=32,
            scratch_bytes=16 * 1024**2,
        )
        self.observation = ScopeObservation(
            control_group="/user.slice/test.scope",
            member_pids=(1234,),
            cpu_quota=200000,
            cpu_period=100000,
            affinity=(0, 1),
            memory_max=self.request.memory_bytes,
            memory_swap_max=0,
            tasks_max=self.request.tasks_max,
            scratch_bytes=self.request.scratch_bytes,
            scratch_inode_count=4096,
            writable_mounts=RECORDED_R13_MOUNTS,
        )

    def test_controller_available_but_limit_unset_is_rejected(self):
        observation = self.observation.__class__(
            **{**self.observation.__dict__, "cpu_quota": None}
        )
        with self.assertRaisesRegex(IsolationError, "CPU quota"):
            verify_scope_observation(self.request, observation, 1234)

    def test_foreign_pid_in_scope_is_rejected(self):
        observation = self.observation.__class__(
            **{**self.observation.__dict__, "member_pids": (1234, 9999)}
        )
        with self.assertRaisesRegex(IsolationError, "foreign PID"):
            verify_scope_observation(self.request, observation, 1234)

    def test_memory_pid_and_scratch_readback_mismatch_is_rejected(self):
        for field, value, message in (
            ("memory_max", self.request.memory_bytes + 1, "memory"),
            ("tasks_max", self.request.tasks_max + 1, "task"),
            ("scratch_bytes", self.request.scratch_bytes + 4096, "scratch"),
        ):
            with self.subTest(field=field):
                observation = self.observation.__class__(
                    **{**self.observation.__dict__, field: value}
                )
                with self.assertRaisesRegex(IsolationError, message):
                    verify_scope_observation(self.request, observation, 1234)

    def test_affinity_available_but_not_restricted_is_rejected(self):
        observation = self.observation.__class__(
            **{**self.observation.__dict__, "affinity": (0, 1, 2)}
        )
        with self.assertRaisesRegex(IsolationError, "affinity"):
            verify_scope_observation(self.request, observation, 1234)

    def test_complete_kernel_readback_is_accepted(self):
        verify_scope_observation(self.request, self.observation, 1234)


class UserScopeManagerTests(unittest.TestCase):
    def test_unit_ownership_accepts_only_revision_20_prefix(self):
        UserScopeManager._validate_unit("punto-c2c-bfdc-i7-r20-owned.scope")
        for unit in (
            "punto-c2c-bfdc-i7-r14-owned.scope",
            "punto-c2c-bfdc-i7-r15-owned.scope",
            "punto-c2c-bfdc-i7-r16-owned.scope",
            "punto-c2c-bfdc-i7-r17-owned.scope",
            "punto-c2c-bfdc-i7-r18-owned.scope",
            "punto-c2c-bfdc-i7-r19-owned.scope",
            "punto-c2c-bfdc-i7-r20-owned.service",
        ):
            with self.subTest(unit=unit), self.assertRaisesRegex(
                IsolationError, "non-owned"
            ):
                UserScopeManager._validate_unit(unit)

    def test_terminal_properties_include_result_and_oom_policy(self):
        run = mock.Mock(
            return_value=subprocess.CompletedProcess(
                [], 0, "Result=oom-kill\nOOMPolicy=kill\n", ""
            )
        )
        manager = UserScopeManager(run=run)
        properties = manager.properties("punto-c2c-bfdc-i7-r20-test.scope", timeout=1)
        command = run.call_args.args[0]
        self.assertIn("Result", command)
        self.assertIn("OOMPolicy", command)
        self.assertEqual(properties["Result"], "oom-kill")
        self.assertEqual(properties["OOMPolicy"], "kill")

    def test_manager_failure_does_not_spawn_or_fallback(self):
        run = mock.Mock(return_value=subprocess.CompletedProcess([], 1, "", "no bus"))
        popen = mock.Mock()
        manager = UserScopeManager(run=run, popen=popen)
        with self.assertRaisesRegex(IsolationError, "user manager unavailable"):
            manager.start(
                "punto-c2c-bfdc-i7-r20-test.scope",
                ResourceRequest(256 * 1024**2, 32, 16 * 1024**2),
                [sys.executable, "-c", "pass"],
            )
        popen.assert_not_called()

    def test_cleanup_addresses_scope_after_leader_exit_and_is_idempotent(self):
        process = mock.Mock()
        process.poll.return_value = 0
        process.stdout = None
        process.stderr = None
        manager = mock.Mock()
        manager.scope_state.side_effect = [
            {"members": (7777,), "populated": True, "unknown": False},
            {"members": (), "populated": False, "unknown": False},
            {"members": (), "populated": False, "unknown": False},
        ]
        scope = TransientScope(
            manager=manager,
            unit="punto-c2c-bfdc-i7-r20-test.scope",
            process=process,
            control_group="/user.slice/test.scope",
            starter_pid=1234,
            starter_identity="42",
        )
        scope.cleanup()
        scope.cleanup()
        self.assertEqual(manager.kill_scope.call_args.args[0], scope.unit)
        self.assertTrue(scope.cleaned)

    def test_cleanup_covers_child_in_new_process_group(self):
        process = mock.Mock()
        process.poll.return_value = 0
        process.stdout = None
        process.stderr = None
        manager = mock.Mock()
        manager.scope_state.side_effect = [
            {"members": (8888,), "populated": True, "unknown": False},
            {"members": (), "populated": False, "unknown": False},
            {"members": (), "populated": False, "unknown": False},
        ]
        scope = TransientScope(
            manager=manager,
            unit="punto-c2c-bfdc-i7-r20-test.scope",
            process=process,
            control_group="/user.slice/test.scope",
            starter_pid=1234,
            starter_identity="42",
        )
        scope.cleanup()
        self.assertEqual(manager.kill_scope.call_args.args[0], scope.unit)

    def test_cleanup_rejects_remaining_survivor(self):
        process = mock.Mock()
        process.poll.return_value = 0
        process.stdout = None
        process.stderr = None
        manager = mock.Mock()
        manager.scope_state.return_value = {
            "members": (8888,), "populated": True, "unknown": False
        }
        scope = TransientScope(
            manager=manager,
            unit="punto-c2c-bfdc-i7-r20-test.scope",
            process=process,
            control_group="/user.slice/test.scope",
            starter_pid=1234,
            starter_identity="42",
        )
        with (
            mock.patch("isolated_target.CLEANUP_DEADLINE_SECONDS", 0.01),
            mock.patch("isolated_target.time.sleep"),
            self.assertRaisesRegex(IsolationError, "cleanup incomplete"),
        ):
            scope.cleanup()


class Revision14VerifierTests(unittest.TestCase):
    def test_limit_mismatch_prevents_launch_marker(self):
        request = ResourceRequest(256 * 1024**2, 32, 16 * 1024**2)
        observation = ScopeObservation(
            control_group="/user.slice/punto-c2c-bfdc-i7-r20-test.scope",
            member_pids=(1234,),
            cpu_quota=200000,
            cpu_period=100000,
            affinity=(0, 1),
            memory_max=request.memory_bytes + 1,
            memory_swap_max=0,
            tasks_max=32,
            scratch_bytes=None,
            scratch_inode_count=None,
            writable_mounts=(),
        )
        with tempfile.TemporaryDirectory() as tmp:
            marker = Path(tmp) / "launch-go"
            with self.assertRaisesRegex(IsolationError, "memory"):
                write_launch_marker(
                    Path(tmp), request, observation, 1234,
                    "punto-c2c-bfdc-i7-r20-test.scope",
                )
            self.assertFalse(marker.exists())

    def test_production_smoke_requires_exact_length_and_digest(self):
        verify_production_smoke({
            "small_write_bytes": PRODUCTION_SMOKE_BYTES,
            "small_write_sha256": PRODUCTION_SMOKE_SHA256,
        })
        with self.assertRaisesRegex(IsolationError, "digest is missing"):
            verify_production_smoke({"small_write_bytes": PRODUCTION_SMOKE_BYTES})
        with self.assertRaisesRegex(IsolationError, "digest mismatch"):
            verify_production_smoke({
                "small_write_bytes": PRODUCTION_SMOKE_BYTES,
                "small_write_sha256": "0" * 64,
            })

    def test_cleanup_failure_takes_precedence_over_primary_failure(self):
        proof = {}
        with self.assertRaisesRegex(SafetyAbort, "cleanup failed"):
            finalize_scope_errors(
                proof, IsolationError("primary failed"), IsolationError("cleanup failed")
            )
        self.assertEqual(proof["primary_error"], "primary failed")
        self.assertFalse(proof["cleanup"]["complete"])

    def test_partial_line_without_eof_obeys_deadline(self):
        process = subprocess.Popen(
            [sys.executable, "-c", "import sys,time;sys.stdout.write('{');sys.stdout.flush();time.sleep(5)"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            with self.assertRaisesRegex(IsolationError, "timed out"):
                _read_json_line(process, 0.05)
            self.assertIsNone(process.poll())
        finally:
            process.terminate()
            process.wait(timeout=2)
            process.stdout.close()
            process.stderr.close()

    def test_partial_line_at_eof_is_rejected(self):
        process = subprocess.Popen(
            [sys.executable, "-c", "print('{', end='')"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            with self.assertRaisesRegex(IsolationError, "partial line"):
                _read_json_line(process, 1)
        finally:
            process.wait(timeout=2)
            process.stdout.close()
            process.stderr.close()

    def test_failed_host_observations_do_not_compare_equal(self):
        unknown = {
            "status": "unknown",
            "processes": [],
            "sessions": {"exit_code": None, "stdout": None},
        }
        result = compare_protected_identity(unknown, unknown)
        self.assertEqual(result["status"], "unknown")
        self.assertFalse(result["protected_processes_unchanged"])
        self.assertFalse(result["protected_sessions_unchanged"])

    def test_owned_starter_reap_precedes_fresh_survivor_membership(self):
        script = r'''
import json
import os
import subprocess
import sys
import time

child = subprocess.Popen(
    [sys.executable, "-c", "import time; time.sleep(30)"],
    stdin=subprocess.DEVNULL,
    stdout=subprocess.DEVNULL,
    stderr=subprocess.DEVNULL,
    start_new_session=True,
)
print(json.dumps({
    "pid": child.pid,
    "start_identity": open(f"/proc/{child.pid}/stat").read().rsplit(")", 1)[1].split()[19],
    "pgid": os.getpgid(child.pid),
    "sid": os.getsid(child.pid),
    "starter_pgid": os.getpgid(0),
    "starter_sid": os.getsid(0),
}), flush=True)
time.sleep(0.1)
'''
        process = subprocess.Popen(
            [sys.executable, "-c", script],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        child = None
        try:
            starter = {
                "pid": process.pid,
                "start_identity": isolated_target.process_start_identity(process.pid),
                "pgid": os.getpgid(process.pid),
                "sid": os.getsid(process.pid),
            }
            self.assertIsNotNone(process.stdout)
            child = isolated_target.json.loads(process.stdout.readline())
            deadline = isolated_target.time.monotonic() + 2
            while isolated_target.time.monotonic() < deadline:
                stat = Path(f"/proc/{process.pid}/stat").read_text(encoding="ascii")
                if stat.rsplit(")", 1)[1].split()[0] == "Z":
                    break
                isolated_target.time.sleep(0.01)
            else:
                self.fail("starter did not become an exited unreaped process")
            exit_evidence = isolated_target.wait_for_owned_starter_exit(
                process, starter, 1
            )
            lifecycle = accept_production_survivor(child, (child["pid"],))
            verify_production_relationship(starter, lifecycle)
            self.assertEqual(exit_evidence["exit_code"], 0)
            self.assertEqual(exit_evidence["owned_process_pid"], process.pid)
            self.assertTrue(exit_evidence["ownership_verified"])
            self.assertTrue(exit_evidence["natural_exit"])
            self.assertTrue(exit_evidence["reaped"])
            self.assertEqual(process.returncode, 0)
            self.assertEqual(lifecycle["pid"], child["pid"])
            self.assertTrue(lifecycle["starter_exited"])
        finally:
            if process.poll() is None:
                process.terminate()
            process.wait(timeout=2)
            if process.stdout is not None:
                process.stdout.close()
            if process.stderr is not None:
                process.stderr.close()
            if child is not None:
                try:
                    if (
                        isolated_target.process_start_identity(child["pid"])
                        == child["start_identity"]
                    ):
                        os.kill(child["pid"], signal.SIGTERM)
                except (FileNotFoundError, ProcessLookupError):
                    pass

    def test_first_scope_failure_prevents_second_scope_and_retry(self):
        def fail_first(*_args, attempt, **_kwargs):
            attempt.update({
                "unit": "punto-c2c-bfdc-i7-r20-test.scope",
                "phase": "TERMINAL_DIAGNOSTICS",
                "status": "FAILED",
                "primary_error": "first failed",
            })
            raise IsolationError("first failed")

        runner = mock.Mock(side_effect=fail_first)
        snapshot = mock.Mock()
        results = []
        before = {
            "status": "valid",
            "processes": [],
            "sessions": {"exit_code": 0, "stdout": ""},
        }
        with (
            tempfile.TemporaryDirectory() as tmp,
            self.assertRaisesRegex(IsolationError, "first failed"),
        ):
            _run_scope_sequence(
                mock.Mock(), Path(tmp), before, runner=runner, snapshot=snapshot,
                results=results,
            )
        self.assertEqual(runner.call_count, 1)
        snapshot.assert_not_called()
        self.assertEqual(len(results), 1)
        self.assertEqual(results[0]["label"], "scope-small")
        self.assertEqual(results[0]["status"], "FAILED")
        self.assertEqual(results[0]["primary_error"], "first failed")
        self.assertEqual(results[0]["unit"], "punto-c2c-bfdc-i7-r20-test.scope")


class Revision15VerifierTests(unittest.TestCase):
    def setUp(self):
        self.request = ResourceRequest(256 * 1024**2, 32, 16 * 1024**2)
    def workload(self, sizes, *, successful=None, file_bytes=None):
        total = sum(sizes)
        return {
            "memory_children": [{"exit_code": -9}, {"exit_code": 0}, {"exit_code": 0}],
            "tasks_spawn_errno": isolated_target.errno.EAGAIN,
            "tasks_spawned": 31,
            "scratch_files": [
                {"size": size, "intended_bytes": intended}
                for size, intended in zip(
                    sizes, (6 * 1024**2, 6 * 1024**2, 8 * 1024**2), strict=True
                )
            ],
            "scratch_successful_write_bytes": total if successful is None else successful,
            "scratch_file_bytes": total if file_bytes is None else file_bytes,
            "scratch_errno": isolated_target.errno.ENOSPC,
        }

    def verify(self, workload, available=16 * 1024**2):
        verify_scratch_proof(self.request, workload, available)

    def test_accepts_consistent_enospc_at_measured_ceiling(self):
        self.verify(self.workload([6 * 1024**2, 6 * 1024**2, 4 * 1024**2]))

    def test_rejects_19_mib_under_16_mib_request(self):
        with self.assertRaisesRegex(IsolationError, "exceed"):
            self.verify(
                self.workload([6 * 1024**2, 6 * 1024**2, 7 * 1024**2]),
                available=20 * 1024**2,
            )

    def test_rejects_early_enospc(self):
        with self.assertRaisesRegex(IsolationError, "early"):
            self.verify(self.workload([6 * 1024**2, 6 * 1024**2, 2 * 1024**2]))

    def test_rejects_inconsistent_write_and_file_accounting(self):
        sizes = [6 * 1024**2, 6 * 1024**2, 4 * 1024**2]
        with self.assertRaisesRegex(IsolationError, "inconsistent"):
            self.verify(self.workload(sizes, successful=sum(sizes) - 1))
        with self.assertRaisesRegex(IsolationError, "inconsistent"):
            self.verify(self.workload(sizes, file_bytes=sum(sizes) - 1))

    def test_task_verdict_rejects_missing_decreasing_boolean_and_negative_counters(self):
        task = {
            "tasks_spawned": 1,
            "tasks_spawn_attempts": 2,
            "tasks_spawn_errno": isolated_target.errno.EAGAIN,
            "task_children": [{"exit_code": -15}],
        }
        for before, after, message in (
            ({}, {"max": 1}, "max is missing"),
            ({"max": 0}, {}, "max is missing"),
            ({"max": 2}, {"max": 1}, "max decreased"),
            ({"max": False}, {"max": 1}, "max is not an integer"),
            ({"max": -1}, {"max": 1}, "max is negative"),
        ):
            with self.subTest(message=message), self.assertRaisesRegex(IsolationError, message):
                verify_task_proof(task, before, after)


class Revision14OrchestrationTests(unittest.TestCase):
    request = ResourceRequest(256 * 1024**2, 32, 16 * 1024**2)

    @staticmethod
    def sandbox_ready():
        mountinfo = (
            "1 0 0:1 / / rw - tmpfs tmpfs rw\n"
            "2 1 8:1 /usr /usr ro - ext4 /dev/root rw\n"
            "3 1 0:2 / /proc rw - proc proc rw\n"
            "4 1 8:1 /control /control ro - ext4 /dev/root rw\n"
        )
        return {
            "kind": "sandbox-ready",
            "attempt_id": "test",
            "phase": "sandbox-ready",
            "sequence": 0,
            "scratch_bytes": 16 * 1024**2,
            "scratch_available_bytes": 16 * 1024**2,
            "scratch_inode_count": 4096,
            "mountinfo": mountinfo,
            "socket_fds": [],
            "paths": {
                "home": "/scratch/home", "tmpdir": "/scratch/tmp",
                "xdg_runtime_dir": "/scratch/run", "cwd": "/scratch",
                "scratch": "/scratch",
            },
            "devices": {name: 7 for name in ("home", "tmpdir", "xdg_runtime_dir", "cwd", "scratch")},
        }

    def enforcement_workload(self):
        total = 16 * 1024**2
        return {
            "kind": "workload-result",
            "mode": "enforcement",
            "memory_children": [{"exit_code": -9}, {"exit_code": 0}, {"exit_code": 0}],
            "tasks_spawn_errno": isolated_target.errno.EAGAIN,
            "tasks_spawned": 31,
            "scratch_files": [
                {"size": 6 * 1024**2, "intended_bytes": 6 * 1024**2},
                {"size": 6 * 1024**2, "intended_bytes": 6 * 1024**2},
                {"size": 4 * 1024**2, "intended_bytes": 8 * 1024**2},
            ],
            "scratch_successful_write_bytes": total,
            "scratch_file_bytes": total,
            "scratch_errno": isolated_target.errno.ENOSPC,
        }

    def run_failure(
        self, root, *, members=(1234,), diagnostics=None, cleanup=None, order=None,
        attempt=None, workload=None,
    ):
        manager = mock.Mock()
        process = mock.Mock()
        process.poll.return_value = None
        manager.start.return_value = process
        manager.properties.return_value = {
            "ControlGroup": "/user.slice/punto-c2c-bfdc-i7-r20-test.scope",
            "CPUQuotaPerSecUSec": "2s", "MemoryMax": str(self.request.memory_bytes),
            "MemorySwapMax": "0", "TasksMax": str(self.request.tasks_max),
        }
        manager.scope_members.side_effect = [members, members]
        manager.scope_state.return_value = {
            "members": (), "populated": False, "unknown": False,
            "survivor_gone": None,
        }
        order = order if order is not None else []
        starter = {
            "pid": 1234, "start_identity": "42", "affinity": [0, 1], "pgid": 1234, "sid": 1234,
        }

        def terminal(*_args):
            order.append("diagnostics")
            if isinstance(diagnostics, BaseException):
                raise diagnostics
            return diagnostics or {
                "status": "CAPTURED",
                "memory_events": {"available": False, "error": "gone"},
                "pids_events": {"available": False, "error": "gone"},
            }

        def clean(_scope):
            order.append("cleanup")
            if isinstance(cleanup, BaseException):
                raise cleanup
            return {"members": (), "populated": False, "unknown": False, "survivor_gone": None}

        attempt = attempt if attempt is not None else {}
        if workload is None:
            wait_for_file = mock.patch("isolated_target._wait_for_file", return_value=starter)
            staged = mock.patch(
                "isolated_target._run_staged_enforcement",
                side_effect=IsolationError("scope workload ended without evidence"),
            )
        else:
            wait_for_file = mock.patch(
                "isolated_target._wait_for_file",
                side_effect=[starter, {"exit_code": 0}],
            )
            staged = mock.patch("isolated_target._run_staged_enforcement", return_value=workload)
        patches = (
            mock.patch("isolated_target.secrets.token_hex", return_value="test"),
            mock.patch("isolated_target.os.sched_getaffinity", return_value={0, 1}),
            wait_for_file,
            mock.patch("isolated_target.process_start_identity", return_value="42"),
            mock.patch("isolated_target._cgroup_path", return_value=Path("/fake/cgroup")),
            mock.patch("isolated_target._cpu_max", return_value=(200000, 100000)),
            mock.patch("isolated_target._read_limit", side_effect=[self.request.memory_bytes, 0, 32]),
            mock.patch("isolated_target._event_counters", return_value={"max": 0}),
            mock.patch("isolated_target._read_json_line", return_value=self.sandbox_ready()),
            staged,
            mock.patch("isolated_target._capture_terminal_diagnostics", side_effect=terminal),
            mock.patch.object(TransientScope, "cleanup", clean),
        )
        with patches[0], patches[1], patches[2], patches[3], patches[4], patches[5], patches[6], patches[7], patches[8], patches[9], patches[10], patches[11]:
            _run_resource_scope(manager, root, "scope-small", self.request, "enforcement", attempt=attempt)
        return attempt, order

    def test_oom_like_eof_persists_baselines_and_diagnoses_before_cleanup(self):
        order = []
        attempt = {}
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            with self.assertRaisesRegex(IsolationError, "without evidence"):
                self.run_failure(root, order=order, attempt=attempt)
            proof = isolated_target.json.loads((root / "scope-small/proof.json").read_text())
        self.assertEqual(order, ["diagnostics", "cleanup"])
        self.assertEqual(proof["status"], "FAILED")
        self.assertEqual(proof["pids_events_before_raw"], {"max": 0})
        self.assertFalse(proof["terminal_diagnostics"]["memory_events"]["available"])
        self.assertNotEqual(proof["status"], "VERIFIED")
        self.assertEqual(attempt, proof)
        self.assertEqual(attempt["unit"], "punto-c2c-bfdc-i7-r20-test.scope")

    def test_diagnostic_and_cleanup_failures_keep_cleanup_safety_precedence(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(SafetyAbort, "cleanup failed"):
                self.run_failure(
                    Path(tmp), diagnostics=IsolationError("diagnostics failed"),
                    cleanup=IsolationError("cleanup failed"),
                )
            proof = isolated_target.json.loads((Path(tmp) / "scope-small/proof.json").read_text())
        self.assertIn("diagnostics failed", proof["diagnostic_error"])
        self.assertIn("without evidence", proof["primary_error"])
        self.assertIn("cleanup failed", proof["cleanup"]["error"])

    def test_foreign_membership_is_safety_abort_even_when_cleanup_succeeds(self):
        with tempfile.TemporaryDirectory() as tmp, self.assertRaisesRegex(SafetyAbort, "foreign PID"):
            self.run_failure(Path(tmp), members=(1234, 9999))

    def test_successful_workload_cleanup_failure_is_not_verified(self):
        attempt = {}
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            with self.assertRaisesRegex(SafetyAbort, "cleanup failed"):
                self.run_failure(
                    root,
                    cleanup=IsolationError("cleanup failed"),
                    attempt=attempt,
                    workload=self.enforcement_workload(),
                )
            proof = isolated_target.json.loads((root / "scope-small/proof.json").read_text())
        self.assertNotEqual(proof["status"], "VERIFIED")
        self.assertEqual(proof["status"], "ABORTED_SAFETY")
        self.assertEqual(attempt["status"], "ABORTED_SAFETY")
        self.assertFalse(proof["cleanup"]["complete"])
        self.assertIn("cleanup failed", proof["cleanup"]["error"])
        self.assertNotIn("primary_error", proof)
        self.assertEqual(proof["phase"], "CLEANUP_FAILED")


class TerminalDiagnosticsTests(unittest.TestCase):
    UNIT = "punto-c2c-bfdc-i7-r20-test.scope"

    def test_manager_timeout_and_oserror_preserve_captured_counters(self):
        failures = (
            subprocess.TimeoutExpired(cmd=["systemctl"], timeout=1),
            OSError("bus closed"),
        )
        for error in failures:
            with self.subTest(error=type(error).__name__), tempfile.TemporaryDirectory() as tmp:
                cgroup = Path(tmp)
                (cgroup / "memory.events").write_text("oom_kill 3\nlow 1\n", encoding="ascii")
                (cgroup / "pids.events").write_text("max 5\n", encoding="ascii")
                manager = mock.Mock()
                manager.properties.side_effect = error
                diagnostics = _capture_terminal_diagnostics(manager, self.UNIT, cgroup)
                self.assertEqual(diagnostics["status"], "CAPTURED")
                self.assertEqual(
                    diagnostics["memory_events"],
                    {"available": True, "value": {"oom_kill": 3, "low": 1}},
                )
                self.assertEqual(
                    diagnostics["pids_events"],
                    {"available": True, "value": {"max": 5}},
                )
                self.assertFalse(diagnostics["manager_properties"]["available"])
                self.assertIn(type(error).__name__, diagnostics["manager_properties"]["error"])
                manager.properties.assert_called_once_with(self.UNIT, timeout=1)


class Revision18StagedProtocolTests(unittest.TestCase):
    attempt_id = "attempt-18"
    control_group = "/user.slice/punto-c2c-bfdc-i7-r20-test.scope"
    request = ResourceRequest(256 * 1024**2, 32, 16 * 1024**2)

    @staticmethod
    def record(phase, sequence, **values):
        return {
            "kind": "phase-result",
            "attempt_id": Revision18StagedProtocolTests.attempt_id,
            "phase": phase,
            "sequence": sequence,
            **values,
        }

    def ready(self):
        ready = self.record(
            "sandbox-ready",
            0,
            scratch_available_bytes=16 * 1024**2,
        )
        ready["kind"] = "sandbox-ready"
        return ready

    def task(self, *, spawn_errno=isolated_target.errno.EAGAIN):
        children = [{"namespace_pid": index + 10, "exit_code": -15} for index in range(31)]
        return self.record(
            "task-proof",
            1,
            tasks_spawned=len(children),
            tasks_spawn_attempts=len(children) + 1,
            tasks_spawn_errno=spawn_errno,
            task_children=children,
        )

    def scratch(self):
        total = 16 * 1024**2
        return self.record(
            "scratch-proof",
            2,
            scratch_files=[
                {"size": 6 * 1024**2, "intended_bytes": 6 * 1024**2},
                {"size": 6 * 1024**2, "intended_bytes": 6 * 1024**2},
                {"size": 4 * 1024**2, "intended_bytes": 8 * 1024**2},
            ],
            scratch_successful_write_bytes=total,
            scratch_file_bytes=total,
            scratch_errno=isolated_target.errno.ENOSPC,
        )

    def prepared(self):
        return self.record(
            "memory-prepared",
            4,
            memory_workers=[
                {
                    "worker_id": f"memory-{index}",
                    "namespace_pid": 20 + index,
                    "requested_bytes": MEMORY_WORKER_BYTES,
                }
                for index in range(3)
            ],
        )

    def mapped_workers(self):
        return [
            {
                "worker_id": f"memory-{index}",
                "namespace_pid": 20 + index,
                "host_pid": 1020 + index,
                "host_start_identity": f"start-{index}",
                "requested_bytes": MEMORY_WORKER_BYTES,
            }
            for index in range(3)
        ]

    def reporter(self):
        return self.record(
            "memory-proof",
            5,
            memory_children=[
                {
                    "worker_id": f"memory-{index}",
                    "namespace_pid": 20 + index,
                    "exit_code": -9 if index == 0 else 0,
                }
                for index in range(3)
            ],
        )

    @staticmethod
    def kernel(victims=()):
        return {
            "status": "AVAILABLE",
            "query": {"exit_code": 0},
            "victims": list(victims),
        }

    def complete_transport(self, reporter=None):
        if reporter is None:
            reporter = self.reporter()
        return _reporter_transport_evidence("COMPLETE", record=reporter)

    def eof_transport(self, status="EOF_WITHOUT_RECORD", error="scope workload ended without evidence"):
        return _reporter_transport_evidence(status, error=error)

    def test_rejects_wrong_attempt_out_of_order_and_duplicate_ack(self):
        wrong_kind = self.record("task-proof", 1)
        wrong_kind["kind"] = "sandbox-ready"
        with self.assertRaisesRegex(IsolationError, "kind is invalid"):
            _accept_protocol_record(wrong_kind, self.attempt_id, "task-proof", 1)
        wrong = self.record("task-proof", 1)
        wrong["attempt_id"] = "stale"
        with self.assertRaisesRegex(IsolationError, "wrong attempt"):
            _accept_protocol_record(wrong, self.attempt_id, "task-proof", 1)
        with self.assertRaisesRegex(IsolationError, "out of order"):
            _accept_protocol_record(
                self.record("scratch-proof", 2), self.attempt_id, "task-proof", 1
            )
        with self.assertRaisesRegex(IsolationError, "sequence is invalid"):
            _accept_protocol_record(
                self.record("task-proof", 2), self.attempt_id, "task-proof", 1
            )
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            isolated_target._write_protocol_ack(root, self.attempt_id, "sandbox-ready", 0)
            with self.assertRaisesRegex(IsolationError, "duplicate"):
                isolated_target._write_protocol_ack(root, self.attempt_id, "sandbox-ready", 0)

    def test_task_rejection_prevents_scratch_and_memory_release(self):
        proof = {}
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            with (
                mock.patch("isolated_target._read_json_line", return_value=self.task(spawn_errno=None)),
                mock.patch("isolated_target._event_counters", return_value={"max": 1}),
                mock.patch("isolated_target._map_memory_workers") as mapper,
                self.assertRaisesRegex(IsolationError, "task enforcement"),
            ):
                _run_staged_enforcement(
                    mock.Mock(), mock.Mock(), root, Path("/fake/cgroup"),
                    self.control_group, 1000, self.request, self.ready(), {"max": 0},
                    self.attempt_id, proof, root / "proof.json", lambda maximum: maximum,
                )
            self.assertTrue((root / "ack-0.json").is_file())
            self.assertFalse((root / "ack-1.json").exists())
            self.assertFalse((root / "ack-4.json").exists())
            mapper.assert_not_called()

    def test_staged_orchestration_persists_each_acceptance_before_memory_release(self):
        proof = {}
        restored = self.record(
            "scratch-restored", 3, scratch_available_bytes=16 * 1024**2
        )
        reporter = self.reporter()
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)

            def observe(
                _process, _cgroup, _timeout, persist, *, attempt_id, control_group
            ):
                observed_at = isolated_target.time.time()
                observations = [{
                    "available": True,
                    "attempt_id": attempt_id,
                    "control_group": control_group,
                    "observed_at": observed_at,
                    "value": {"oom_kill": 1},
                }]
                persist(observations)
                return (
                    _reporter_transport_evidence("COMPLETE", record=reporter),
                    observations,
                    observed_at,
                )

            with (
                mock.patch(
                    "isolated_target._read_json_line",
                    side_effect=[self.task(), self.scratch(), restored, self.prepared()],
                ),
                mock.patch(
                    "isolated_target._event_counters",
                    side_effect=[{"max": 1}, {"oom_kill": 0}],
                ),
                mock.patch("isolated_target._map_memory_workers", return_value=self.mapped_workers()),
                mock.patch("isolated_target._observe_memory_phase", side_effect=observe),
                mock.patch(
                    "isolated_target._kernel_victim_records",
                    return_value=self.kernel(),
                ),
            ):
                result = _run_staged_enforcement(
                    mock.Mock(), mock.Mock(), root, Path("/fake/cgroup"),
                    self.control_group, 1000, self.request, self.ready(), {"max": 0},
                    self.attempt_id, proof, root / "proof.json", lambda maximum: maximum,
                )
            self.assertEqual(result["kind"], "staged-workload-result")
            self.assertEqual(proof["phase"], "MEMORY_PROOF_PERSISTED")
            self.assertEqual(proof["memory_reporter_transport"]["status"], "COMPLETE")
            self.assertEqual(proof["memory_reporter_transport"]["eligibility"], "REPORTER")
            self.assertEqual(
                [item["phase"] for item in proof["protocol"]["accepted"]],
                list(isolated_target.ENFORCEMENT_PHASES),
            )
            for sequence in range(6):
                self.assertTrue((root / f"ack-{sequence}.json").is_file())

    def run_staged_transport_rejection(self, process, expected_status):
        proof = {}
        restored = self.record(
            "scratch-restored", 3, scratch_available_bytes=16 * 1024**2
        )
        memory_reads = 0

        def counters(_path, name):
            nonlocal memory_reads
            if name == "pids.events":
                return {"max": 1}
            self.assertEqual(name, "memory.events")
            memory_reads += 1
            return {"oom_kill": 0 if memory_reads == 1 else 1}

        def kernel(_group, workers, attempt_id, started_at, ended_at):
            return self.kernel([{
                "attempt_id": attempt_id,
                "control_group": self.control_group,
                "host_pid": workers[0]["host_pid"],
                "observed_at": (started_at + ended_at) / 2,
                "record": "exact victim",
            }])

        try:
            with tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                proof_path = root / "proof.json"
                with (
                    mock.patch(
                        "isolated_target._read_json_line",
                        side_effect=[self.task(), self.scratch(), restored, self.prepared()],
                    ),
                    mock.patch("isolated_target._event_counters", side_effect=counters),
                    mock.patch(
                        "isolated_target._map_memory_workers",
                        return_value=self.mapped_workers(),
                    ),
                    mock.patch("isolated_target._kernel_victim_records", side_effect=kernel),
                    self.assertRaisesRegex(IsolationError, "transport is contradictory"),
                ):
                    _run_staged_enforcement(
                        mock.Mock(),
                        process,
                        root,
                        Path("/fake/cgroup"),
                        self.control_group,
                        1000,
                        self.request,
                        self.ready(),
                        {"max": 0},
                        self.attempt_id,
                        proof,
                        proof_path,
                        lambda maximum: min(maximum, 0.25),
                    )
                persisted = isolated_target.json.loads(proof_path.read_text())
                self.assertEqual(
                    persisted["memory_reporter_transport"]["status"], expected_status
                )
                self.assertEqual(
                    persisted["memory_reporter_transport"]["eligibility"], "REJECT"
                )
                self.assertGreater(
                    persisted["memory_reporter_transport"]["offending"]["byte_count"],
                    0,
                )
                self.assertIn(
                    "transport is contradictory",
                    persisted["protocol"]["rejected"][-1]["error"],
                )
                for sequence in range(5):
                    self.assertTrue((root / f"ack-{sequence}.json").is_file())
                self.assertFalse((root / "ack-5.json").exists())
                self.assertNotEqual(persisted.get("phase"), "MEMORY_PROOF_PERSISTED")
        finally:
            if process.poll() is None:
                process.terminate()
            process.wait(timeout=2)
            process.stdout.close()

    def test_staged_duplicate_transport_is_persisted_and_blocks_final_ack(self):
        payload = isolated_target.json.dumps(self.reporter(), separators=(",", ":"))
        process = subprocess.Popen(
            [
                sys.executable,
                "-c",
                "import sys; p=sys.argv[1]; sys.stdout.write(p+'\\n'+p+'\\n'); sys.stdout.flush()",
                payload,
            ],
            stdout=subprocess.PIPE,
            text=True,
        )
        self.run_staged_transport_rejection(process, "DUPLICATE")

    def test_staged_malformed_transport_is_persisted_and_blocks_final_ack(self):
        overflow_source = (
            "import sys; sys.stdout.buffer.write(b'x' * "
            f"{isolated_target.MAX_JSON_LINE_BYTES + 1}); sys.stdout.flush()"
        )
        cases = (
            ("import sys; sys.stdout.buffer.write(b'not-json\\n')", "INVALID_JSON"),
            ("import sys; sys.stdout.buffer.write(b'\\xff\\n')", "INVALID_UTF8"),
            ("print('[]')", "INVALID_OBJECT"),
            (overflow_source, "OVERFLOW"),
        )
        for source, status in cases:
            with self.subTest(status=status):
                process = subprocess.Popen(
                    [sys.executable, "-c", source],
                    stdout=subprocess.PIPE,
                    text=True,
                )
                self.run_staged_transport_rejection(process, status)

    def test_memory_requires_counter_and_corroborated_prepared_worker(self):
        workers = self.mapped_workers()
        observations = [{
            "available": True,
            "attempt_id": self.attempt_id,
            "control_group": self.control_group,
            "observed_at": 15.0,
            "value": {"oom_kill": 1},
        }]
        verify_memory_proof(
            {"oom_kill": 0}, observations, workers,
            self.complete_transport(), self.kernel(),
            attempt_id=self.attempt_id, control_group=self.control_group,
            release_started_at=10.0, observation_ended_at=20.0,
        )
        exact_victim = [{
            "attempt_id": self.attempt_id,
            "control_group": self.control_group,
            "host_pid": workers[0]["host_pid"],
            "observed_at": 15.0,
        }]
        verify_memory_proof(
            {"oom_kill": 0}, observations, workers,
            self.eof_transport(), self.kernel(exact_victim),
            attempt_id=self.attempt_id, control_group=self.control_group,
            release_started_at=10.0, observation_ended_at=20.0,
        )
        for victims in (
            [],
            [{**exact_victim[0], "control_group": "/wrong.scope"}],
            [{**exact_victim[0], "attempt_id": "stale"}],
            [{**exact_victim[0], "host_pid": 999999}],
            [{**exact_victim[0], "observed_at": 9.0}],
        ):
            with self.subTest(victims=victims), self.assertRaisesRegex(
                IsolationError, "corroborated"
            ):
                verify_memory_proof(
                    {"oom_kill": 0}, observations, workers,
                    self.eof_transport(), self.kernel(victims),
                    attempt_id=self.attempt_id, control_group=self.control_group,
                    release_started_at=10.0, observation_ended_at=20.0,
                )
        mismatched = self.reporter()
        mismatched["memory_children"][0]["namespace_pid"] = 999
        with self.assertRaisesRegex(IsolationError, "contradictory"):
            verify_memory_proof(
                {"oom_kill": 0}, observations, workers,
                self.complete_transport(mismatched), self.kernel(),
                attempt_id=self.attempt_id, control_group=self.control_group,
                release_started_at=10.0, observation_ended_at=20.0,
            )
        generic_nonzero = self.reporter()
        generic_nonzero["memory_children"][0]["exit_code"] = -15
        with self.assertRaisesRegex(IsolationError, "contradictory"):
            verify_memory_proof(
                {"oom_kill": 0}, observations, workers,
                self.complete_transport(generic_nonzero), self.kernel(),
                attempt_id=self.attempt_id, control_group=self.control_group,
                release_started_at=10.0, observation_ended_at=20.0,
            )
        stale_observation = [{**observations[0], "attempt_id": "stale"}]
        with self.assertRaisesRegex(IsolationError, "wrong attempt"):
            verify_memory_proof(
                {"oom_kill": 0}, stale_observation, workers,
                self.complete_transport(), self.kernel(),
                attempt_id=self.attempt_id, control_group=self.control_group,
                release_started_at=10.0, observation_ended_at=20.0,
            )

    def test_memory_verdict_rejects_missing_decreasing_boolean_and_negative_counters(self):
        workers = self.mapped_workers()
        for before, value, message in (
            ({}, {"oom_kill": 1}, "oom_kill is missing"),
            ({"oom_kill": 0}, {}, "oom_kill is missing"),
            ({"oom_kill": 2}, {"oom_kill": 1}, "oom_kill decreased"),
            ({"oom_kill": False}, {"oom_kill": 1}, "oom_kill is not an integer"),
            ({"oom_kill": -1}, {"oom_kill": 1}, "oom_kill is negative"),
        ):
            observation = [{
                "available": True,
                "attempt_id": self.attempt_id,
                "control_group": self.control_group,
                "observed_at": 15.0,
                "value": value,
            }]
            with self.subTest(message=message), self.assertRaisesRegex(IsolationError, message):
                verify_memory_proof(
                    before, observation, workers,
                    self.complete_transport(), self.kernel(),
                    attempt_id=self.attempt_id, control_group=self.control_group,
                    release_started_at=10.0, observation_ended_at=20.0,
                )

    def test_memory_verdict_rejects_decrease_between_readable_samples(self):
        observations = [
            {
                "available": True,
                "attempt_id": self.attempt_id,
                "control_group": self.control_group,
                "observed_at": observed_at,
                "value": {"oom_kill": value},
            }
            for observed_at, value in ((15.0, 2), (17.0, 1))
        ]
        observations.insert(
            1,
            {
                "available": False,
                "attempt_id": self.attempt_id,
                "control_group": self.control_group,
                "observed_at": 16.0,
                "error": "temporary read failure",
            },
        )
        with self.assertRaisesRegex(IsolationError, "oom_kill decreased"):
            verify_memory_proof(
                {"oom_kill": 0},
                observations,
                self.mapped_workers(),
                self.complete_transport(),
                {"status": "AVAILABLE", "query": {}, "victims": []},
                attempt_id=self.attempt_id,
                control_group=self.control_group,
                release_started_at=10.0,
                observation_ended_at=20.0,
            )

    def test_contradictory_reporter_cannot_fall_back_to_kernel_victim(self):
        workers = self.mapped_workers()
        observations = [{
            "available": True,
            "attempt_id": self.attempt_id,
            "control_group": self.control_group,
            "observed_at": 15.0,
            "value": {"oom_kill": 1},
        }]
        kernel = {
            "status": "AVAILABLE",
            "query": {"exit_code": 0},
            "victims": [{
                "attempt_id": self.attempt_id,
                "control_group": self.control_group,
                "host_pid": workers[0]["host_pid"],
                "observed_at": 15.0,
                "record": "exact victim",
            }],
        }
        mismatched = self.reporter()
        mismatched["memory_children"][0]["namespace_pid"] = 999
        duplicate = self.reporter()
        duplicate["memory_children"][1] = dict(duplicate["memory_children"][0])
        for reporter in (mismatched, duplicate):
            with self.subTest(reporter=reporter), self.assertRaisesRegex(
                IsolationError, "contradictory"
            ):
                verify_memory_proof(
                    {"oom_kill": 0}, observations, workers,
                    self.complete_transport(reporter), kernel,
                    attempt_id=self.attempt_id,
                    control_group=self.control_group,
                    release_started_at=10.0,
                    observation_ended_at=20.0,
                )
        verify_memory_proof(
            {"oom_kill": 0}, observations, workers,
            self.eof_transport(), kernel,
            attempt_id=self.attempt_id,
            control_group=self.control_group,
            release_started_at=10.0,
            observation_ended_at=20.0,
        )

    def test_kernel_parser_requires_exact_recognized_memcg_oom_fields(self):
        workers = self.mapped_workers()
        exact = (
            "15.000000 kernel: oom-kill:constraint=CONSTRAINT_MEMCG,"
            f"oom_memcg={self.control_group},task_memcg={self.control_group},"
            f"task=python3,pid={workers[0]['host_pid']},uid=1000"
        )
        rejected = (
            exact.replace("oom-kill:", "notice:"),
            exact.replace(
                f"oom_memcg={self.control_group}",
                f"oom_memcg={self.control_group}-prefix",
            ),
            exact.replace(f"pid={workers[0]['host_pid']}", "pid=999999"),
            exact.replace("15.000000", "9.000000"),
            exact.replace("constraint=CONSTRAINT_MEMCG", "constraint=CONSTRAINT_NONE"),
            exact.replace(
                "constraint=CONSTRAINT_MEMCG",
                "constraint=CONSTRAINT_MEMCG,constraint=CONSTRAINT_MEMCG",
            ),
        )
        with mock.patch(
            "isolated_target.run_capture",
            return_value={"exit_code": 0, "stdout": "\n".join((exact, *rejected)), "stderr": "", "elapsed_ms": 1},
        ):
            evidence = _kernel_victim_records(
                self.control_group, workers, self.attempt_id, 10.0, 20.0
            )
        self.assertEqual(evidence["status"], "AVAILABLE")
        self.assertEqual(len(evidence["victims"]), 1)
        self.assertEqual(evidence["victims"][0]["record"], exact)

        with mock.patch(
            "isolated_target.run_capture",
            return_value={"exit_code": None, "error": "TimeoutExpired", "elapsed_ms": 2000},
        ):
            unavailable = _kernel_victim_records(
                self.control_group, workers, self.attempt_id, 10.0, 20.0
            )
        self.assertEqual(unavailable["status"], "UNAVAILABLE")
        self.assertEqual(unavailable["victims"], [])

    def test_rejected_task_record_and_counter_snapshot_are_persisted(self):
        proof = {}
        rejected = self.task(spawn_errno=None)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            with (
                mock.patch("isolated_target._read_json_line", return_value=rejected),
                mock.patch("isolated_target._event_counters", return_value={"max": 1}),
                self.assertRaisesRegex(IsolationError, "task enforcement"),
            ):
                _run_staged_enforcement(
                    mock.Mock(), mock.Mock(), root, Path("/fake/cgroup"),
                    self.control_group, 1000, self.request, self.ready(), {"max": 0},
                    self.attempt_id, proof, root / "proof.json", lambda maximum: maximum,
                )
            persisted = isolated_target.json.loads((root / "proof.json").read_text())
            self.assertEqual(persisted["task_events_after"], {"max": 1})
            self.assertEqual(persisted["protocol"]["received"][-1]["record"], rejected)
            self.assertEqual(persisted["protocol"]["rejected"][-1]["record"], rejected)
            self.assertFalse((root / "ack-1.json").exists())

    def test_missing_baselines_prevent_corresponding_release_ack(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            with self.assertRaisesRegex(IsolationError, "max is missing"):
                _run_staged_enforcement(
                    mock.Mock(), mock.Mock(), root, Path("/fake/cgroup"),
                    self.control_group, 1000, self.request, self.ready(), {},
                    self.attempt_id, {}, root / "task-proof.json", lambda maximum: maximum,
                )
            self.assertFalse((root / "ack-0.json").exists())

        restored = self.record(
            "scratch-restored", 3, scratch_available_bytes=16 * 1024**2
        )
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            with (
                mock.patch(
                    "isolated_target._read_json_line",
                    side_effect=[self.task(), self.scratch(), restored, self.prepared()],
                ),
                mock.patch(
                    "isolated_target._event_counters",
                    side_effect=[{"max": 1}, {}],
                ),
                mock.patch(
                    "isolated_target._map_memory_workers",
                    return_value=self.mapped_workers(),
                ),
                self.assertRaisesRegex(IsolationError, "oom_kill is missing"),
            ):
                _run_staged_enforcement(
                    mock.Mock(), mock.Mock(), root, Path("/fake/cgroup"),
                    self.control_group, 1000, self.request, self.ready(), {"max": 0},
                    self.attempt_id, {}, root / "memory-proof.json", lambda maximum: maximum,
                )
            self.assertTrue((root / "ack-3.json").exists())
            self.assertFalse((root / "ack-4.json").exists())

    def test_outer_worker_mapping_rejects_namespace_or_attempt_mismatch(self):
        manager = mock.Mock()
        manager.scope_members.return_value = (1000, 1020, 1021, 1022)
        namespace = {1000: (1000, 1), 1020: (1020, 20), 1021: (1021, 21), 1022: (1022, 22)}
        cmdline = {
            pid: ("python3", "-c", self.attempt_id, f"memory-{pid - 1020}")
            for pid in (1020, 1021, 1022)
        }
        cmdline[1000] = ("scope-worker",)
        with (
            mock.patch("isolated_target._is_descendant", return_value=True),
            mock.patch("isolated_target._process_namespace_pids", side_effect=lambda pid: namespace[pid]),
            mock.patch("isolated_target._process_cmdline", side_effect=lambda pid: cmdline[pid]),
            mock.patch("isolated_target.process_start_identity", side_effect=lambda pid: f"start-{pid}"),
        ):
            mapped = _map_memory_workers(
                manager, self.control_group, 1000, self.prepared(), self.attempt_id
            )
            self.assertEqual([item["host_pid"] for item in mapped], [1020, 1021, 1022])
            wrong = self.prepared()
            wrong["memory_workers"][0]["namespace_pid"] = 99
            with self.assertRaisesRegex(IsolationError, "no owned host mapping"):
                _map_memory_workers(manager, self.control_group, 1000, wrong, self.attempt_id)
            with self.assertRaisesRegex(IsolationError, "does not match"):
                _map_memory_workers(manager, self.control_group, 1000, self.prepared(), "stale")

    def test_outer_observer_preserves_counter_before_reporter_eof(self):
        process = subprocess.Popen(
            [sys.executable, "-c", "pass"], stdout=subprocess.PIPE, text=True
        )
        persisted = []
        try:
            with tempfile.TemporaryDirectory() as tmp:
                cgroup = Path(tmp)
                (cgroup / "memory.events").write_text("oom_kill 1\n", encoding="ascii")
                transport, observations, _ended = _observe_memory_phase(
                    process,
                    cgroup,
                    1,
                    lambda values: persisted.append(list(values)),
                    attempt_id=self.attempt_id,
                    control_group=self.control_group,
                )
            self.assertEqual(transport["status"], "EOF_WITHOUT_RECORD")
            self.assertEqual(transport["eligibility"], "KERNEL_FALLBACK")
            self.assertIn("without evidence", transport["error"])
            self.assertEqual(observations[0]["value"]["oom_kill"], 1)
            self.assertEqual(persisted[-1], observations)
        finally:
            process.wait(timeout=2)
            process.stdout.close()

    def test_partial_reporter_preserves_earlier_and_terminal_counters(self):
        process = subprocess.Popen(
            [
                sys.executable,
                "-c",
                "import sys,time; sys.stdout.write('{'); sys.stdout.flush(); time.sleep(0.2)",
            ],
            stdout=subprocess.PIPE,
            text=True,
        )
        persisted = []
        try:
            with tempfile.TemporaryDirectory() as tmp:
                cgroup = Path(tmp)
                events = cgroup / "memory.events"
                events.write_text("oom_kill 1\n", encoding="ascii")

                def persist(values):
                    persisted.append(list(values))
                    if len(persisted) == 1:
                        events.write_text("oom_kill 2\n", encoding="ascii")

                transport, observations, _ended = _observe_memory_phase(
                    process,
                    cgroup,
                    0.35,
                    persist,
                    attempt_id=self.attempt_id,
                    control_group=self.control_group,
                )
            self.assertEqual(transport["status"], "PARTIAL_EOF")
            self.assertEqual(transport["eligibility"], "KERNEL_FALLBACK")
            self.assertIn("partial line", transport["error"])
            self.assertGreater(transport["offending"]["byte_count"], 0)
            self.assertGreaterEqual(len(observations), 2)
            self.assertEqual(observations[0]["value"]["oom_kill"], 1)
            self.assertEqual(observations[-1]["value"]["oom_kill"], 2)
            self.assertEqual(persisted[-1], observations)
        finally:
            if process.poll() is None:
                process.terminate()
            process.wait(timeout=2)
            process.stdout.close()

    def test_reporter_transport_errors_have_distinct_statuses(self):
        cases = (
            ("PARTIAL_EOF", "KERNEL_FALLBACK"),
            ("EOF_WITHOUT_RECORD", "KERNEL_FALLBACK"),
            ("OVERFLOW", "REJECT"),
            ("INVALID_UTF8", "REJECT"),
            ("INVALID_JSON", "REJECT"),
            ("INVALID_OBJECT", "REJECT"),
            ("TIMEOUT", "REJECT"),
            ("PIPE_UNAVAILABLE", "REJECT"),
            ("ERROR", "REJECT"),
        )
        for status, eligibility in cases:
            with self.subTest(status=status):
                evidence = _reporter_transport_evidence(status, error="test")
                self.assertEqual(evidence["status"], status)
                self.assertEqual(evidence["eligibility"], eligibility)

    def _verify_transport_with_exact_kernel(
        self, transport, observations, observation_ended_at
    ):
        workers = self.mapped_workers()
        observed_at = observations[0]["observed_at"]
        kernel = self.kernel([{
            "attempt_id": self.attempt_id,
            "control_group": self.control_group,
            "host_pid": workers[0]["host_pid"],
            "observed_at": observed_at,
            "record": "exact victim",
        }])
        verify_memory_proof(
            {"oom_kill": 0},
            observations,
            workers,
            transport,
            kernel,
            attempt_id=self.attempt_id,
            control_group=self.control_group,
            release_started_at=observed_at - 1,
            observation_ended_at=observation_ended_at,
        )

    def test_coalesced_and_delayed_duplicate_records_reject_with_exact_kernel(self):
        payload = isolated_target.json.dumps(self.reporter(), separators=(",", ":"))
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            cgroup = root / "cgroup"
            cgroup.mkdir()
            (cgroup / "memory.events").write_text("oom_kill 1\n", encoding="ascii")

            cases = []
            coalesced = subprocess.Popen(
                [
                    sys.executable,
                    "-c",
                    "import sys; p=sys.argv[1]; sys.stdout.write(p+'\\n'+p+'\\n'); sys.stdout.flush()",
                    payload,
                ],
                stdout=subprocess.PIPE,
                text=True,
            )
            cases.append(("coalesced", coalesced, lambda _values: None))

            ready = root / "ready"
            gate = root / "gate"
            delayed = subprocess.Popen(
                [
                    sys.executable,
                    "-c",
                    (
                        "import pathlib,sys,time; p=sys.argv[1]; ready=pathlib.Path(sys.argv[2]); "
                        "gate=pathlib.Path(sys.argv[3]); sys.stdout.write(p+'\\n'); "
                        "sys.stdout.flush(); ready.write_text('ready'); "
                        "deadline=time.monotonic()+2; "
                        "\nwhile not gate.exists() and time.monotonic()<deadline: time.sleep(0.001)"
                        "\nsys.stdout.write(p+'\\n'); sys.stdout.flush()"
                    ),
                    payload,
                    str(ready),
                    str(gate),
                ],
                stdout=subprocess.PIPE,
                text=True,
            )
            deadline = isolated_target.time.monotonic() + 2
            while not ready.exists() and isolated_target.time.monotonic() < deadline:
                isolated_target.time.sleep(0.001)
            self.assertTrue(ready.exists())
            persist_calls = 0

            def release_delayed(_values):
                nonlocal persist_calls
                persist_calls += 1
                if persist_calls == 2:
                    gate.write_text("go", encoding="ascii")

            cases.append(("delayed", delayed, release_delayed))

            for name, process, persist in cases:
                with self.subTest(name=name):
                    try:
                        transport, observations, ended_at = _observe_memory_phase(
                            process,
                            cgroup,
                            0.25,
                            persist,
                            attempt_id=self.attempt_id,
                            control_group=self.control_group,
                        )
                        self.assertEqual(transport["status"], "DUPLICATE")
                        self.assertEqual(transport["eligibility"], "REJECT")
                        self.assertEqual(transport["record"]["attempt_id"], self.attempt_id)
                        self.assertGreater(transport["offending"]["byte_count"], 0)
                        with self.assertRaisesRegex(IsolationError, "transport is contradictory"):
                            self._verify_transport_with_exact_kernel(
                                transport, observations, ended_at
                            )
                    finally:
                        if process.poll() is None:
                            process.terminate()
                        process.wait(timeout=2)
                        process.stdout.close()

    def test_duplicate_stops_pipe_drain_after_first_contradictory_chunk(self):
        payload = isolated_target.json.dumps(self.reporter(), separators=(",", ":"))
        process = mock.Mock(stdout=mock.Mock())
        process.stdout.fileno.return_value = 42
        reads = [payload.encode() + b"\n", b"trailing", b"must-not-be-read"]
        with tempfile.TemporaryDirectory() as tmp:
            cgroup = Path(tmp)
            (cgroup / "memory.events").write_text("oom_kill 1\n", encoding="ascii")
            with (
                mock.patch("isolated_target.os.set_blocking"),
                mock.patch("isolated_target.os.read", side_effect=reads) as read,
                mock.patch("isolated_target.select.select", return_value=([42], [], [])),
            ):
                transport, _observations, _ended_at = _observe_memory_phase(
                    process,
                    cgroup,
                    0.01,
                    lambda _values: None,
                    attempt_id=self.attempt_id,
                    control_group=self.control_group,
                )
        self.assertEqual(transport["status"], "DUPLICATE")
        self.assertEqual(transport["offending"]["byte_count"], len(b"trailing"))
        self.assertEqual(read.call_count, 2)

    def test_malformed_complete_evidence_rejects_with_exact_kernel(self):
        overflow_source = (
            "import sys; sys.stdout.buffer.write(b'x' * "
            f"{isolated_target.MAX_JSON_LINE_BYTES + 1}); sys.stdout.flush()"
        )
        cases = (
            ("import sys; sys.stdout.buffer.write(b'not-json\\n')", "INVALID_JSON"),
            ("import sys; sys.stdout.buffer.write(b'\\xff\\n')", "INVALID_UTF8"),
            ("print('[]')", "INVALID_OBJECT"),
            (overflow_source, "OVERFLOW"),
        )
        for source, status in cases:
            with self.subTest(status=status):
                process = subprocess.Popen(
                    [sys.executable, "-c", source],
                    stdout=subprocess.PIPE,
                    text=True,
                )
                try:
                    with tempfile.TemporaryDirectory() as tmp:
                        cgroup = Path(tmp)
                        (cgroup / "memory.events").write_text(
                            "oom_kill 1\n", encoding="ascii"
                        )
                        transport, observations, ended_at = _observe_memory_phase(
                            process,
                            cgroup,
                            1.5,
                            lambda _values: None,
                            attempt_id=self.attempt_id,
                            control_group=self.control_group,
                        )
                    self.assertEqual(transport["status"], status)
                    self.assertEqual(transport["eligibility"], "REJECT")
                    self.assertGreater(transport["offending"]["byte_count"], 0)
                    with self.assertRaisesRegex(IsolationError, "transport is contradictory"):
                        self._verify_transport_with_exact_kernel(
                            transport, observations, ended_at
                        )
                finally:
                    if process.poll() is None:
                        process.terminate()
                    process.wait(timeout=2)
                    process.stdout.close()

    def test_valid_report_and_genuine_eof_loss_remain_eligible(self):
        payload = isolated_target.json.dumps(self.reporter(), separators=(",", ":"))
        cases = (
            (f"import sys,time; print({payload!r}); sys.stdout.flush(); time.sleep(1)", "COMPLETE"),
            ("pass", "EOF_WITHOUT_RECORD"),
            ("import sys; sys.stdout.write('{'); sys.stdout.flush()", "PARTIAL_EOF"),
        )
        for source, status in cases:
            with self.subTest(status=status):
                process = subprocess.Popen(
                    [sys.executable, "-c", source],
                    stdout=subprocess.PIPE,
                    text=True,
                )
                try:
                    with tempfile.TemporaryDirectory() as tmp:
                        cgroup = Path(tmp)
                        (cgroup / "memory.events").write_text(
                            "oom_kill 1\n", encoding="ascii"
                        )
                        transport, observations, ended_at = _observe_memory_phase(
                            process,
                            cgroup,
                            0.15,
                            lambda _values: None,
                            attempt_id=self.attempt_id,
                            control_group=self.control_group,
                        )
                    self.assertEqual(transport["status"], status)
                    self._verify_transport_with_exact_kernel(
                        transport, observations, ended_at
                    )
                finally:
                    if process.poll() is None:
                        process.terminate()
                    process.wait(timeout=2)
                    process.stdout.close()

    def test_timeout_and_unavailable_pipe_cannot_use_kernel_fallback(self):
        observations = [{
            "available": True,
            "attempt_id": self.attempt_id,
            "control_group": self.control_group,
            "observed_at": 15.0,
            "value": {"oom_kill": 1},
        }]
        for status in ("TIMEOUT", "PIPE_UNAVAILABLE", "ERROR"):
            transport = _reporter_transport_evidence(status, error="unavailable")
            with self.subTest(status=status), self.assertRaisesRegex(
                IsolationError, "transport is contradictory"
            ):
                self._verify_transport_with_exact_kernel(
                    transport, observations, 20.0
                )

    def test_reporter_eof_does_not_stop_later_counter_observation(self):
        process = subprocess.Popen(
            [sys.executable, "-c", "pass"],
            stdout=subprocess.PIPE,
            text=True,
        )
        process.wait(timeout=2)
        persisted = []
        try:
            with tempfile.TemporaryDirectory() as tmp:
                cgroup = Path(tmp)
                events = cgroup / "memory.events"
                events.write_text("oom_kill 1\n", encoding="ascii")

                def persist(values):
                    persisted.append(list(values))
                    if len(persisted) == 1:
                        events.write_text("oom_kill 3\n", encoding="ascii")

                transport, observations, _ended = _observe_memory_phase(
                    process,
                    cgroup,
                    0.2,
                    persist,
                    attempt_id=self.attempt_id,
                    control_group=self.control_group,
                )
            self.assertEqual(transport["status"], "EOF_WITHOUT_RECORD")
            self.assertIn("without evidence", transport["error"])
            self.assertGreaterEqual(len(observations), 2)
            self.assertEqual(observations[0]["value"]["oom_kill"], 1)
            self.assertEqual(observations[-1]["value"]["oom_kill"], 3)
            self.assertEqual(persisted[-1], observations)
        finally:
            process.stdout.close()

    def test_terminal_counter_disappearance_is_explicit_and_preserves_earlier_read(self):
        process = subprocess.Popen(
            [sys.executable, "-c", "import time; time.sleep(1)"],
            stdout=subprocess.PIPE,
            text=True,
        )
        persisted = []
        try:
            with tempfile.TemporaryDirectory() as tmp:
                cgroup = Path(tmp)
                events = cgroup / "memory.events"
                events.write_text("oom_kill 1\n", encoding="ascii")

                def persist(values):
                    persisted.append(list(values))
                    if events.exists():
                        events.unlink()

                transport, observations, _ended = _observe_memory_phase(
                    process,
                    cgroup,
                    0.2,
                    persist,
                    attempt_id=self.attempt_id,
                    control_group=self.control_group,
                )
            self.assertEqual(transport["status"], "TIMEOUT")
            self.assertTrue(observations[0]["available"])
            self.assertEqual(observations[0]["value"]["oom_kill"], 1)
            self.assertFalse(observations[-1]["available"])
            self.assertIn("error", observations[-1])
            self.assertNotEqual(observations[-1].get("error"), "")
            self.assertEqual(persisted[-1], observations)
            self.assertEqual(transport["eligibility"], "REJECT")
        finally:
            if process.poll() is None:
                process.terminate()
            process.wait(timeout=2)
            process.stdout.close()

    def test_timeout_and_reporter_overflow_are_fail_closed_and_preserve_observations(self):
        overflow_source = (
            "import sys,time; sys.stdout.buffer.write(b'x' * "
            f"{isolated_target.MAX_JSON_LINE_BYTES + 1}); "
            "sys.stdout.buffer.flush(); time.sleep(2)"
        )
        cases = (
            (
                "import time; time.sleep(2)",
                0.05,
                "timed out",
            ),
            (overflow_source, 1.5, "size limit"),
        )
        for source, timeout, message in cases:
            with self.subTest(message=message):
                process = subprocess.Popen(
                    [sys.executable, "-c", source],
                    stdout=subprocess.PIPE,
                    text=True,
                )
                persisted = []
                try:
                    with tempfile.TemporaryDirectory() as tmp:
                        cgroup = Path(tmp)
                        (cgroup / "memory.events").write_text(
                            "oom_kill 1\n", encoding="ascii"
                        )
                        transport, observations, _ended = _observe_memory_phase(
                            process,
                            cgroup,
                            timeout,
                            lambda values, persisted=persisted: persisted.append(list(values)),
                            attempt_id=self.attempt_id,
                            control_group=self.control_group,
                        )
                    self.assertEqual(transport["eligibility"], "REJECT")
                    self.assertIn(message, transport["error"])
                    self.assertGreaterEqual(len(observations), 2)
                    self.assertEqual(observations[0]["value"]["oom_kill"], 1)
                    self.assertTrue(observations[0]["available"])
                    self.assertEqual(persisted[-1], observations)
                    self.assertTrue(observations[-1]["available"])
                finally:
                    if process.poll() is None:
                        process.terminate()
                    process.wait(timeout=2)
                    process.stdout.close()

    def test_reporter_utf8_json_and_object_errors_are_distinct_from_counters(self):
        cases = (
            ("import sys; sys.stdout.buffer.write(b'\\xff\\n')", "UTF-8"),
            ("print('not-json')", "invalid"),
            ("print('[]')", "not an object"),
        )
        for source, message in cases:
            with self.subTest(message=message):
                process = subprocess.Popen(
                    [sys.executable, "-c", source],
                    stdout=subprocess.PIPE,
                    text=True,
                )
                persisted = []
                try:
                    with tempfile.TemporaryDirectory() as tmp:
                        cgroup = Path(tmp)
                        (cgroup / "memory.events").write_text(
                            "oom_kill 1\n", encoding="ascii"
                        )
                        transport, observations, _ended = _observe_memory_phase(
                            process,
                            cgroup,
                            0.2,
                            lambda values, persisted=persisted: persisted.append(list(values)),
                            attempt_id=self.attempt_id,
                            control_group=self.control_group,
                        )
                    self.assertEqual(transport["eligibility"], "REJECT")
                    self.assertIn(message, transport["error"])
                    self.assertEqual(observations[0]["value"]["oom_kill"], 1)
                    self.assertTrue(observations[-1]["available"])
                    self.assertEqual(persisted[-1], observations)
                finally:
                    if process.poll() is None:
                        process.terminate()
                    process.wait(timeout=2)
                    process.stdout.close()

    def test_transport_rejection_prevents_scope_two(self):
        attempts = []
        payload = isolated_target.json.dumps(self.reporter(), separators=(",", ":"))
        process = subprocess.Popen(
            [
                sys.executable,
                "-c",
                "import sys; p=sys.argv[1]; sys.stdout.write(p+'\\n'+p+'\\n'); sys.stdout.flush()",
                payload,
            ],
            stdout=subprocess.PIPE,
            text=True,
        )

        def fail_memory(*_args, attempt, **_kwargs):
            attempts.append(attempt)
            self.run_staged_transport_rejection(process, "DUPLICATE")
            attempt.update({
                "phase": "MEMORY_RELEASED",
                "status": "FAILED",
                "primary_error": "memory reporter transport is contradictory",
            })
            raise IsolationError("memory reporter transport is contradictory")

        runner = mock.Mock(side_effect=fail_memory)
        with tempfile.TemporaryDirectory() as tmp, self.assertRaisesRegex(
            IsolationError, "transport is contradictory"
        ):
            _run_scope_sequence(
                mock.Mock(),
                Path(tmp),
                {"status": "valid", "processes": [], "sessions": {"exit_code": 0, "stdout": ""}},
                runner=runner,
                snapshot=mock.Mock(),
            )
        self.assertEqual(runner.call_count, 1)
        self.assertEqual(len(attempts), 1)
        self.assertEqual(attempts[0]["label"], "scope-small")


class Revision18GraphContractTests(unittest.TestCase):
    def test_transport_gate_precedes_preserved_downstream_contract(self):
        graph = isolated_target.json.loads(
            (isolated_target.REPO_ROOT / "protocol/c2c-bfdc-i7-recovery-r18-graph.json")
            .read_text(encoding="utf-8")
        )
        nodes = {item["id"]: item for item in graph["nodes"]}
        self.assertEqual(graph["plan_revision"], 18)
        self.assertEqual(
            nodes["r17_offline_transport_integration"]["needs"],
            ["r16_offline_observer_and_attribution"],
        )
        self.assertEqual(
            nodes["dedicated_resource_boundary_proof"]["needs"],
            ["r17_offline_transport_integration"],
        )
        self.assertIn("candidate_2_8_14_probe", nodes)
        self.assertIn("Repeated EN/RU Pause behavior", nodes["manual_word_vertical_slice"]["done"])
        self.assertIn("no unresolved P0-P2 finding", nodes["source_and_architecture_gate"]["done"])
        self.assertIn("match accepted bytes", nodes["release_review_and_publication"]["done"])


class Revision19GraphContractTests(unittest.TestCase):
    def test_revision_18_graph_file_is_unchanged(self):
        graph = isolated_target.json.loads(
            (isolated_target.REPO_ROOT / "protocol/c2c-bfdc-i7-recovery-r18-graph.json")
            .read_text(encoding="utf-8")
        )
        self.assertEqual(graph["plan_revision"], 18)
        self.assertEqual(graph["previous_plan_revision"], 17)
        nodes = {item["id"]: item for item in graph["nodes"]}
        self.assertEqual(
            nodes["dedicated_resource_boundary_proof"]["needs"],
            ["r17_offline_transport_integration"],
        )

    def test_r19_records_r18_evidence_and_preserves_downstream_contract(self):
        graph = isolated_target.json.loads(
            (isolated_target.REPO_ROOT / "protocol/c2c-bfdc-i7-recovery-r19-graph.json")
            .read_text(encoding="utf-8")
        )
        nodes = {item["id"]: item for item in graph["nodes"]}
        self.assertEqual(graph["plan_revision"], 19)
        self.assertEqual(graph["previous_plan_revision"], 18)
        self.assertEqual(nodes["r18_small_scope_enforcement"]["status"], "DONE_ACCEPTED")
        self.assertEqual(
            nodes["r18_production_configuration_smoke_cleanup"]["status"],
            "PARTIAL_ACCEPTED",
        )
        self.assertEqual(
            nodes["dedicated_resource_boundary_proof"]["needs"],
            ["r18_production_configuration_smoke_cleanup"],
        )
        self.assertIn("BLOCKED", nodes["dedicated_resource_boundary_proof"]["status"])
        self.assertEqual(
            nodes["baseline_2_8_5_probe"]["needs"],
            ["dedicated_resource_boundary_proof"],
        )
        self.assertEqual(
            nodes["candidate_2_8_14_probe"]["needs"],
            ["dedicated_resource_boundary_proof"],
        )
        self.assertIn("candidate_2_8_14_probe", nodes)
        self.assertIn("Repeated EN/RU Pause behavior", nodes["manual_word_vertical_slice"]["done"])
        self.assertIn("no unresolved P0-P2 finding", nodes["source_and_architecture_gate"]["done"])
        self.assertIn("match accepted bytes", nodes["release_review_and_publication"]["done"])


class Revision20GraphContractTests(unittest.TestCase):
    def test_revision_18_and_19_graph_files_are_unchanged(self):
        r18 = isolated_target.json.loads(
            (isolated_target.REPO_ROOT / "protocol/c2c-bfdc-i7-recovery-r18-graph.json")
            .read_text(encoding="utf-8")
        )
        r19 = isolated_target.json.loads(
            (isolated_target.REPO_ROOT / "protocol/c2c-bfdc-i7-recovery-r19-graph.json")
            .read_text(encoding="utf-8")
        )
        self.assertEqual(r18["plan_revision"], 18)
        self.assertEqual(r18["previous_plan_revision"], 17)
        self.assertEqual(r19["plan_revision"], 19)
        self.assertEqual(r19["previous_plan_revision"], 18)
        r18_nodes = {item["id"]: item for item in r18["nodes"]}
        r19_nodes = {item["id"]: item for item in r19["nodes"]}
        self.assertEqual(
            r18_nodes["dedicated_resource_boundary_proof"]["needs"],
            ["r17_offline_transport_integration"],
        )
        self.assertEqual(r19_nodes["r18_small_scope_enforcement"]["status"], "DONE_ACCEPTED")
        self.assertEqual(
            r19_nodes["r18_production_configuration_smoke_cleanup"]["status"],
            "PARTIAL_ACCEPTED",
        )
        self.assertEqual(
            r19_nodes["dedicated_resource_boundary_proof"]["needs"],
            ["r18_production_configuration_smoke_cleanup"],
        )

    def test_r20_records_r18_r19_evidence_and_preserves_downstream_contract(self):
        graph = isolated_target.json.loads(
            (isolated_target.REPO_ROOT / "protocol/c2c-bfdc-i7-recovery-r20-graph.json")
            .read_text(encoding="utf-8")
        )
        nodes = {item["id"]: item for item in graph["nodes"]}
        self.assertEqual(graph["plan_revision"], 20)
        self.assertEqual(graph["previous_plan_revision"], 19)
        self.assertEqual(nodes["r18_small_scope_enforcement"]["status"], "DONE_ACCEPTED")
        self.assertEqual(
            nodes["r18_production_configuration_smoke_cleanup"]["status"],
            "PARTIAL_ACCEPTED",
        )
        self.assertEqual(nodes["r19_starter_exit_reap"]["status"], "DONE_ACCEPTED")
        self.assertEqual(
            nodes["r19_starter_exit_reap"]["needs"],
            ["r18_production_configuration_smoke_cleanup"],
        )
        self.assertEqual(
            nodes["r20_survivor_state_observation"]["status"],
            "LOCAL_COMPLETE_AWAITING_C2C_REVIEW",
        )
        self.assertEqual(
            nodes["r20_survivor_state_observation"]["needs"],
            ["r19_starter_exit_reap"],
        )
        self.assertEqual(
            nodes["dedicated_resource_boundary_proof"]["needs"],
            ["r20_survivor_state_observation"],
        )
        self.assertIn("BLOCKED", nodes["dedicated_resource_boundary_proof"]["status"])
        self.assertEqual(
            nodes["baseline_2_8_5_probe"]["needs"],
            ["dedicated_resource_boundary_proof"],
        )
        self.assertEqual(
            nodes["candidate_2_8_14_probe"]["needs"],
            ["dedicated_resource_boundary_proof"],
        )
        self.assertIn("candidate_2_8_14_probe", nodes)
        self.assertIn("Repeated EN/RU Pause behavior", nodes["manual_word_vertical_slice"]["done"])
        self.assertIn("no unresolved P0-P2 finding", nodes["source_and_architecture_gate"]["done"])
        self.assertIn("match accepted bytes", nodes["release_review_and_publication"]["done"])


class Revision20LifecycleTests(unittest.TestCase):
    UNIT = "punto-c2c-bfdc-i7-r20-test.scope"
    request = ResourceRequest(256 * 1024**2, 32, 16 * 1024**2)

    def _stop(self, process, identity=None):
        if process is None:
            return
        pid = process.pid if hasattr(process, "pid") else process
        recorded = identity
        try:
            if recorded is None or isolated_target.process_start_identity(pid) == recorded:
                os.kill(pid, signal.SIGTERM)
        except (FileNotFoundError, ProcessLookupError, IsolationError, PermissionError):
            pass
        if hasattr(process, "wait"):
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                pass
        for stream in (getattr(process, "stdout", None), getattr(process, "stderr", None)):
            if stream is not None and not stream.closed:
                stream.close()

    def _starter_record(self, process):
        return {
            "pid": process.pid,
            "start_identity": isolated_target.process_start_identity(process.pid),
            "pgid": os.getpgid(process.pid),
            "sid": os.getsid(process.pid),
        }

    def _child_record(self, child, starter):
        return {
            "pid": child.pid,
            "start_identity": isolated_target.process_start_identity(child.pid),
            "pgid": os.getpgid(child.pid),
            "sid": os.getsid(child.pid),
            "starter_pgid": starter["pgid"],
            "starter_sid": starter["sid"],
        }

    @staticmethod
    def sandbox_ready():
        mountinfo = (
            "1 0 0:1 / / rw - tmpfs tmpfs rw\n"
            "2 1 8:1 /usr /usr ro - ext4 /dev/root rw\n"
            "3 1 0:2 / /proc rw - proc proc rw\n"
            "4 1 8:1 /control /control ro - ext4 /dev/root rw\n"
        )
        return {
            "kind": "sandbox-ready",
            "attempt_id": "test",
            "phase": "sandbox-ready",
            "sequence": 0,
            "scratch_bytes": 16 * 1024**2,
            "scratch_available_bytes": 16 * 1024**2,
            "scratch_inode_count": 4096,
            "mountinfo": mountinfo,
            "socket_fds": [],
            "paths": {
                "home": "/scratch/home", "tmpdir": "/scratch/tmp",
                "xdg_runtime_dir": "/scratch/run", "cwd": "/scratch",
                "scratch": "/scratch",
            },
            "devices": {name: 7 for name in ("home", "tmpdir", "xdg_runtime_dir", "cwd", "scratch")},
        }

    def test_wait_reaps_natural_zero_exit_without_signaling(self):
        process = subprocess.Popen(
            [sys.executable, "-c", "pass"],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        starter = self._starter_record(process)
        try:
            evidence = wait_for_owned_starter_exit(process, starter, 1)
            self.assertEqual(evidence["exit_code"], 0)
            self.assertTrue(evidence["reaped"])
            self.assertEqual(process.returncode, 0)
            process.terminate = mock.Mock()
            process.kill = mock.Mock()
            self.assertEqual(wait_for_owned_starter_exit(process, starter, 1)["exit_code"], 0)
            process.terminate.assert_not_called()
            process.kill.assert_not_called()
        finally:
            self._stop(process, starter["start_identity"])

    def test_running_starter_is_rejected_without_signal(self):
        process = subprocess.Popen(
            [sys.executable, "-c", "import time; time.sleep(30)"],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        starter = self._starter_record(process)
        try:
            with self.assertRaisesRegex(IsolationError, "starter still alive"):
                wait_for_owned_starter_exit(process, starter, 0)
            self.assertIsNone(process.poll())
        finally:
            self._stop(process, starter["start_identity"])

    def test_abnormal_starter_exit_is_rejected(self):
        process = subprocess.Popen(
            [sys.executable, "-c", "import sys; sys.exit(7)"],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        starter = self._starter_record(process)
        try:
            with self.assertRaisesRegex(IsolationError, "exited abnormally: 7"):
                wait_for_owned_starter_exit(process, starter, 1)
        finally:
            self._stop(process, starter["start_identity"])

    def test_popen_pid_and_identity_mismatch_are_rejected(self):
        process = subprocess.Popen(
            [sys.executable, "-c", "import time; time.sleep(30)"],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        starter = self._starter_record(process)
        try:
            wrapper = dict(starter)
            wrapper["pid"] = process.pid + 1
            with self.assertRaisesRegex(IsolationError, "Popen PID does not match"):
                wait_for_owned_starter_exit(process, wrapper, 1)
            self.assertIsNone(process.poll())
            wrong_identity = dict(starter)
            wrong_identity["start_identity"] = "not-the-owned-starter"
            with self.assertRaisesRegex(IsolationError, "identity does not match"):
                wait_for_owned_starter_exit(process, wrong_identity, 1)
            self.assertIsNone(process.poll())
        finally:
            self._stop(process, starter["start_identity"])

    def test_dead_replaced_and_outside_scope_children_are_rejected(self):
        dead = subprocess.Popen(
            [sys.executable, "-c", "pass"],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        dead.wait(timeout=2)
        starter = {"pgid": 1, "sid": 2}
        dead_child = {
            "pid": dead.pid,
            "start_identity": "stale",
            "pgid": 3,
            "sid": 4,
            "starter_pgid": 1,
            "starter_sid": 2,
        }
        with self.assertRaisesRegex(IsolationError, "identity changed"):
            accept_production_survivor(dead_child, (dead.pid,))

        live = subprocess.Popen(
            [sys.executable, "-c", "import time; time.sleep(30)"],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            start_new_session=True,
        )
        identity = isolated_target.process_start_identity(live.pid)
        try:
            replaced = self._child_record(live, starter)
            replaced["start_identity"] = identity + "-replaced"
            with self.assertRaisesRegex(IsolationError, "identity changed"):
                accept_production_survivor(replaced, (live.pid,))
            outside = self._child_record(live, starter)
            with self.assertRaisesRegex(IsolationError, "escaped the owned scope"):
                accept_production_survivor(outside, ())
        finally:
            self._stop(live, identity)

    def test_identity_matched_unreaped_survivor_is_rejected(self):
        read_fd, write_fd = os.pipe()
        child = subprocess.Popen(
            [sys.executable, "-c", f"import os; os.read({read_fd}, 1)"],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            pass_fds=(read_fd,),
        )
        os.close(read_fd)
        identity = isolated_target.process_start_identity(child.pid)
        starter = {"pgid": -1, "sid": -1}
        record = self._child_record(child, starter)
        membership_before_exit = (child.pid,)
        try:
            os.write(write_fd, b"x")
            os.close(write_fd)
            write_fd = -1
            deadline = isolated_target.time.monotonic() + 2
            while isolated_target.time.monotonic() < deadline:
                stat = Path(f"/proc/{child.pid}/stat").read_text(encoding="ascii")
                if stat.rsplit(")", 1)[1].split()[0] == "Z":
                    break
                isolated_target.time.sleep(0.01)
            else:
                self.fail("survivor fixture did not become exited and unreaped")
            self.assertEqual(
                isolated_target.process_start_identity(child.pid), identity
            )
            with self.assertRaisesRegex(IsolationError, "exited state"):
                accept_production_survivor(record, membership_before_exit)
        finally:
            if write_fd >= 0:
                os.close(write_fd)
            child.wait(timeout=2)
        self.assertEqual(child.returncode, 0)
        with self.assertRaises((FileNotFoundError, ProcessLookupError)):
            isolated_target.process_start_identity(child.pid)

    def test_live_survivor_state_is_accepted(self):
        child = subprocess.Popen(
            [sys.executable, "-c", "import time; time.sleep(30)"],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            start_new_session=True,
        )
        identity = isolated_target.process_start_identity(child.pid)
        starter = {
            "pgid": os.getpgid(child.pid) - 1,
            "sid": os.getsid(child.pid) - 1,
        }
        try:
            observation = observe_production_survivor_stat(child.pid)
            self.assertEqual(observation["pid"], child.pid)
            self.assertEqual(observation["start_identity"], identity)
            self.assertIn(
                observation["state"], isolated_target._PRODUCTION_SURVIVOR_LIVE_STATES
            )
            accepted = accept_production_survivor(
                self._child_record(child, starter), (child.pid,)
            )
            self.assertTrue(accepted["starter_exited"])
            self.assertEqual(accepted["pid"], child.pid)
            self.assertEqual(accepted["start_identity"], identity)
        finally:
            self._stop(child, identity)

    def test_cleanup_target_requires_fresh_owned_membership(self):
        child = subprocess.Popen(
            [sys.executable, "-c", "import time; time.sleep(30)"],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            start_new_session=True,
        )
        identity = isolated_target.process_start_identity(child.pid)
        record = self._child_record(child, {"pgid": -1, "sid": -1})
        try:
            observation = observe_production_survivor_stat(child.pid)
            target = production_survivor_cleanup_target(
                record, (child.pid,), observation
            )
            self.assertEqual(target, {"pid": child.pid, "start_identity": identity})
            self.assertNotIn("starter_exited", target)
            with self.assertRaisesRegex(IsolationError, "escaped the owned scope"):
                production_survivor_cleanup_target(record, (), observation)
        finally:
            self._stop(child, identity)

    def test_unknown_malformed_unreadable_and_replaced_survivor_state_are_rejected(self):
        child = subprocess.Popen(
            [sys.executable, "-c", "import time; time.sleep(30)"],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            start_new_session=True,
        )
        identity = isolated_target.process_start_identity(child.pid)
        starter = {"pgid": -1, "sid": -1}
        record = self._child_record(child, starter)
        try:
            observation = observe_production_survivor_stat(child.pid)
            with self.assertRaisesRegex(IsolationError, "unknown state"):
                accept_production_survivor(
                    record, (child.pid,), observation={**observation, "state": "Q"}
                )
            for terminal in ("X", "x"):
                with self.subTest(state=terminal), self.assertRaisesRegex(
                    IsolationError, "exited state"
                ):
                    accept_production_survivor(
                        record,
                        (child.pid,),
                        observation={**observation, "state": terminal},
                    )
            with self.assertRaisesRegex(IsolationError, "malformed"):
                parse_production_survivor_stat("not a stat line", child.pid)
            with self.assertRaisesRegex(IsolationError, "malformed"):
                parse_production_survivor_stat(
                    f"{child.pid} (unterminated S 1 2 3", child.pid
                )
            nested = (
                f"{child.pid} (python 3 (main)) S "
                + " ".join(["0"] * 18)
                + f" {identity}\n"
            )
            parsed = parse_production_survivor_stat(nested, child.pid)
            self.assertEqual(parsed["pid"], child.pid)
            self.assertEqual(parsed["state"], "S")
            self.assertEqual(parsed["start_identity"], identity)
            with mock.patch.object(
                isolated_target.Path, "read_text", side_effect=PermissionError("denied")
            ), self.assertRaisesRegex(IsolationError, "unreadable"):
                observe_production_survivor_stat(child.pid)
            replaced = {**observation, "start_identity": identity + "-replaced"}
            with self.assertRaisesRegex(IsolationError, "identity changed"):
                accept_production_survivor(
                    record, (child.pid,), observation=replaced
                )
        finally:
            self._stop(child, identity)

    def test_accepted_survivor_is_retained_after_relationship_failure(self):
        child = subprocess.Popen(
            [sys.executable, "-c", "import time; time.sleep(30)"],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        identity = isolated_target.process_start_identity(child.pid)
        process = mock.Mock()
        process.poll.return_value = 0
        process.stdout = None
        process.stderr = None
        scope = TransientScope(
            manager=mock.Mock(),
            unit=self.UNIT,
            process=process,
            control_group="/user.slice/test.scope",
            starter_pid=os.getpid(),
            starter_identity="ignored",
        )
        try:
            starter = {
                "pgid": os.getpgid(child.pid),
                "sid": os.getsid(child.pid),
            }
            record = self._child_record(child, starter)
            accepted = accept_production_survivor(record, (child.pid,))
            scope.survivor = accepted
            with self.assertRaisesRegex(IsolationError, "distinct process group"):
                verify_production_relationship(starter, accepted)
            self.assertEqual(scope.survivor["pid"], child.pid)
            self.assertEqual(scope.survivor["start_identity"], identity)
        finally:
            self._stop(child, identity)

    def run_production(
        self, root, *, members_after=(8888,), relationship_error=None, survivor_state="S"
    ):
        manager = mock.Mock()
        process = mock.Mock()
        process.pid = 1234
        process.stdout = None
        process.stderr = None
        order = []

        def poll():
            order.append("reap")
            return 0

        process.poll.side_effect = poll
        process.wait.return_value = 0
        manager.start.return_value = process
        manager.properties.return_value = {
            "ControlGroup": f"/user.slice/{self.UNIT}",
            "CPUQuotaPerSecUSec": "2s",
            "MemoryMax": str(self.request.memory_bytes),
            "MemorySwapMax": "0",
            "TasksMax": str(self.request.tasks_max),
        }
        member_values = iter(((1234,), (1234,), members_after))

        def scope_members(_control_group):
            order.append("members")
            return next(member_values)

        manager.scope_members.side_effect = scope_members
        manager.scope_state.return_value = {
            "members": (), "populated": False, "unknown": False, "survivor_gone": True,
        }
        starter = {
            "pid": 1234, "start_identity": "42", "affinity": [0, 1], "pgid": 1234, "sid": 1234,
        }
        escaped = {
            "pid": 8888, "start_identity": "99", "pgid": 8888, "sid": 8888,
            "starter_pgid": 1234, "starter_sid": 1234,
        }
        smoke = {
            "kind": "workload-result",
            "mode": "production",
            "small_write_bytes": PRODUCTION_SMOKE_BYTES,
            "small_write_sha256": PRODUCTION_SMOKE_SHA256,
        }
        captured = {}

        def clean(scope):
            order.append("cleanup")
            captured["survivor"] = None if scope.survivor is None else dict(scope.survivor)
            return {"members": (), "populated": False, "unknown": False, "survivor_gone": True}

        identities = {1234: "42", 8888: "99"}

        def observe(pid):
            return {
                "pid": pid,
                "state": survivor_state,
                "start_identity": identities[pid],
            }

        attempt = {}
        patches = (
            mock.patch("isolated_target.secrets.token_hex", return_value="test"),
            mock.patch("isolated_target.os.sched_getaffinity", return_value={0, 1}),
            mock.patch(
                "isolated_target._wait_for_file",
                side_effect=[starter, {"exit_code": 0}, escaped],
            ),
            mock.patch(
                "isolated_target.observe_production_survivor_stat",
                side_effect=observe,
            ),
            mock.patch(
                "isolated_target.process_start_identity",
                side_effect=lambda pid: identities[pid],
            ),
            mock.patch("isolated_target._cgroup_path", return_value=Path("/fake/cgroup")),
            mock.patch("isolated_target._cpu_max", return_value=(200000, 100000)),
            mock.patch(
                "isolated_target._read_limit",
                side_effect=[self.request.memory_bytes, 0, 32],
            ),
            mock.patch(
                "isolated_target._event_counters",
                side_effect=lambda _path, name: (
                    {"oom_kill": 0} if name == "memory.events" else {"max": 0}
                ),
            ),
            mock.patch(
                "isolated_target._read_json_line",
                side_effect=[self.sandbox_ready(), smoke],
            ),
            mock.patch("isolated_target.os.getpgid", side_effect=lambda pid: {8888: 8888}[pid]),
            mock.patch("isolated_target.os.getsid", side_effect=lambda pid: {8888: 8888}[pid]),
            mock.patch.object(TransientScope, "cleanup", clean),
        )
        if relationship_error is not None:
            patches = patches + (
                mock.patch(
                    "isolated_target.verify_production_relationship",
                    side_effect=relationship_error,
                ),
            )
        with ExitStack() as stack:
            for patcher in patches:
                stack.enter_context(patcher)
            _run_resource_scope(
                manager, root, "scope-production", self.request, "production",
                attempt=attempt,
            )
        return attempt, order, captured

    def test_fresh_membership_is_queried_after_starter_reap(self):
        with tempfile.TemporaryDirectory() as tmp:
            attempt, order, captured = self.run_production(Path(tmp))
        self.assertEqual(attempt["status"], "VERIFIED")
        self.assertEqual(attempt["phase"], "CLEANUP_COMPLETE")
        self.assertEqual(attempt["starter_exit"]["exit_code"], 0)
        self.assertTrue(attempt["starter_exit"]["reaped"])
        self.assertEqual(attempt["members_after_starter_exit"], [8888])
        self.assertEqual(captured["survivor"]["pid"], 8888)
        reap_at = order.index("reap")
        self.assertIn("members", order[reap_at + 1 :])
        self.assertEqual(order[:reap_at].count("members"), 2)
        self.assertIn("cleanup", order)

    def test_production_keeps_accepted_child_when_relationship_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            with self.assertRaisesRegex(IsolationError, "distinct session"):
                self.run_production(
                    root,
                    relationship_error=IsolationError(
                        "surviving child did not receive a distinct session"
                    ),
                )
            proof = isolated_target.json.loads((root / "scope-production/proof.json").read_text())
        self.assertEqual(proof["status"], "FAILED")
        self.assertEqual(proof["failure_phase"], "SURVIVOR_IDENTITY_ACCEPTED")
        self.assertEqual(proof["leader_exit_cleanup_case"]["pid"], 8888)
        self.assertIn("cleanup", proof)
        self.assertTrue(proof["cleanup"]["complete"])

    def test_production_failure_after_successful_smoke_stays_failed(self):
        manager = mock.Mock()
        process = mock.Mock()
        process.pid = 9999
        process.poll.return_value = 0
        process.stdout = None
        process.stderr = None
        manager.start.return_value = process
        manager.properties.return_value = {
            "ControlGroup": f"/user.slice/{self.UNIT}",
            "CPUQuotaPerSecUSec": "2s",
            "MemoryMax": str(self.request.memory_bytes),
            "MemorySwapMax": "0",
            "TasksMax": str(self.request.tasks_max),
        }
        manager.scope_members.side_effect = [(1234,), (1234,)]
        manager.scope_state.return_value = {
            "members": (), "populated": False, "unknown": False, "survivor_gone": None,
        }
        starter = {
            "pid": 1234, "start_identity": "42", "affinity": [0, 1], "pgid": 1234, "sid": 1234,
        }
        escaped = {
            "pid": 8888, "start_identity": "99", "pgid": 8888, "sid": 8888,
            "starter_pgid": 1234, "starter_sid": 1234,
        }
        smoke = {
            "kind": "workload-result",
            "mode": "production",
            "small_write_bytes": PRODUCTION_SMOKE_BYTES,
            "small_write_sha256": PRODUCTION_SMOKE_SHA256,
        }
        order = []

        def clean(_scope):
            order.append("cleanup")
            return {"members": (), "populated": False, "unknown": False, "survivor_gone": None}

        attempt = {}
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            patches = (
                mock.patch("isolated_target.secrets.token_hex", return_value="test"),
                mock.patch("isolated_target.os.sched_getaffinity", return_value={0, 1}),
                mock.patch(
                    "isolated_target._wait_for_file",
                    side_effect=[starter, {"exit_code": 0}, escaped],
                ),
                mock.patch("isolated_target.process_start_identity", return_value="42"),
                mock.patch("isolated_target._cgroup_path", return_value=Path("/fake/cgroup")),
                mock.patch("isolated_target._cpu_max", return_value=(200000, 100000)),
                mock.patch(
                    "isolated_target._read_limit",
                    side_effect=[self.request.memory_bytes, 0, 32],
                ),
                mock.patch(
                    "isolated_target._event_counters",
                    side_effect=lambda _path, name: (
                        {"oom_kill": 0} if name == "memory.events" else {"max": 0}
                    ),
                ),
                mock.patch(
                    "isolated_target._read_json_line",
                    side_effect=[self.sandbox_ready(), smoke],
                ),
                mock.patch.object(TransientScope, "cleanup", clean),
            )
            with (
                patches[0], patches[1], patches[2], patches[3], patches[4],
                patches[5], patches[6], patches[7], patches[8], patches[9],
                self.assertRaisesRegex(IsolationError, "Popen PID does not match"),
            ):
                _run_resource_scope(
                    manager, root, "scope-production", self.request, "production",
                    attempt=attempt,
                )
            proof = isolated_target.json.loads((root / "scope-production/proof.json").read_text())
        self.assertEqual(proof["phase"], "CLEANUP_COMPLETE")
        self.assertEqual(proof["status"], "FAILED")
        self.assertEqual(proof["failure_phase"], "SMOKE_COMPLETED")
        self.assertEqual(proof["workload"]["small_write_sha256"], PRODUCTION_SMOKE_SHA256)
        self.assertEqual(order, ["cleanup"])
        self.assertNotEqual(proof["status"], "VERIFIED")
        self.assertEqual(manager.scope_members.call_count, 2)

    def test_production_terminal_survivor_state_persists_cleanup_and_never_verifies(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            with self.assertRaisesRegex(IsolationError, "exited state"):
                self.run_production(root, survivor_state="Z")
            proof = isolated_target.json.loads(
                (root / "scope-production/proof.json").read_text()
            )
        self.assertEqual(proof["survivor_state"]["state"], "Z")
        self.assertEqual(proof["survivor_state"]["pid"], 8888)
        self.assertEqual(proof["survivor_state"]["start_identity"], "99")
        self.assertEqual(proof["leader_exit_cleanup_case"]["pid"], 8888)
        self.assertEqual(proof["leader_exit_cleanup_case"]["start_identity"], "99")
        self.assertNotEqual(proof["leader_exit_cleanup_case"].get("starter_exited"), True)
        self.assertEqual(proof["failure_phase"], "SURVIVOR_CLEANUP_TARGET")
        self.assertNotEqual(proof["status"], "VERIFIED")
        self.assertIn("cleanup", proof)
        self.assertTrue(proof["cleanup"]["complete"])
        self.assertIn("exited state", proof["primary_error"])


class OwnedProcessTests(unittest.TestCase):
    def test_timeout_terminates_only_owned_process_group(self):
        unrelated = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(10)"])
        owned = OwnedProcess.start(
            [sys.executable, "-c", "import time; time.sleep(10)"],
            env={"PATH": os.environ.get("PATH", "")},
        )
        try:
            result = owned.wait(timeout=0.05)
            self.assertEqual(result, "TIMEOUT")
            self.assertIsNone(unrelated.poll())
            self.assertIsNotNone(owned.process.poll())
        finally:
            unrelated.terminate()
            unrelated.wait(timeout=2)

    def test_cleanup_is_idempotent_and_checks_start_identity(self):
        owned = OwnedProcess.start(
            [sys.executable, "-c", "import time; time.sleep(10)"],
            env={"PATH": os.environ.get("PATH", "")},
        )
        owned.cleanup()
        owned.cleanup()
        self.assertIsNotNone(owned.process.poll())


class PreflightTests(unittest.TestCase):
    def test_missing_mandatory_boundary_prevents_spawn(self):
        preflight = Preflight(resource_boundary=ResourceBoundary(
            controllers={"memory", "pids"}, cpu_count=2,
            memory_bytes=4 * 1024**3, scratch_bytes=2 * 1024**3,
        ))
        with mock.patch.object(OwnedProcess, "start") as start:
            result = preflight.run()
        self.assertEqual(result["status"], "BLOCKED_ENVIRONMENT")
        self.assertEqual(result["target_graphics"], "NOT_STARTED")
        start.assert_not_called()

    def test_synthetic_diagnostic_never_claims_physical_acceptance(self):
        result = diagnostic_classification()
        self.assertEqual(result["run_class"], "NESTED_SYNTHETIC_DIAGNOSTIC")
        self.assertEqual(result["physical_input_acceptance"], "NOT_ESTABLISHED")


if __name__ == "__main__":
    unittest.main()
