#!/usr/bin/env python3

import os
import pathlib
import re
import signal
import subprocess
import tempfile
import textwrap
import time
import unittest


REPO = pathlib.Path(__file__).resolve().parents[1]
CLI = REPO / "punto-cli.sh"
STATS = (
    "OK x11_health=ready analysis_health=ready input_health=ready "
    "x11_last_progress_ms=0 analysis_last_progress_ms=0 "
    "input_last_progress_ms=0 analysis_outstanding=0 input_in_flight=0 "
    "log_dropped=0 text_mutation=disabled enabled=0 configured_enabled=1 "
    "config_pending=0 config_generation=1 config_result=ok analyzed=0 "
    "need_switch=0 corrections=0 pending_words=0 "
    "ready_results=0 worker_threads=1 daemon_peers=1 analysis_mode=auto "
    "control_plane=primary queued_tasks=0 avg_queue_us=0 avg_analysis_us=0 "
    "avg_macro_us=0 avg_tail_len=0"
)


def write_executable(path: pathlib.Path, contents: str) -> None:
    path.write_text(textwrap.dedent(contents).lstrip(), encoding="utf-8")
    path.chmod(0o755)


def process_exists(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    return True


class CliHarness:
    def __init__(self) -> None:
        self._temporary = tempfile.TemporaryDirectory(prefix="punto-cli-contract-")
        self.root = pathlib.Path(self._temporary.name)
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.service_state = self.root / "service.state"
        self.service_state.write_text("inactive\n", encoding="ascii")
        self.tray_state = self.root / "tray.state"
        self.tray_state.write_text("inactive\n", encoding="ascii")
        self.observer = self.root / "observer.pid"
        self.version = self.root / "VERSION"
        self.version.write_text("1.2.3\n", encoding="ascii")
        self._install_fixtures()

    def cleanup(self) -> None:
        if self.observer.exists():
            try:
                pid = int(self.observer.read_text(encoding="ascii").strip())
                os.kill(pid, signal.SIGKILL)
            except (ProcessLookupError, ValueError):
                pass
        self._temporary.cleanup()

    def _install_fixtures(self) -> None:
        write_executable(
            self.bin / "systemctl",
            """
            #!/usr/bin/env bash
            set -u
            if [[ ${1:-} == --user ]]; then
                shift
                action=${1:-}
                if [[ ${TEST_TRAY_MANAGER_MODE:-normal} == hang ]]; then
                    trap '' TERM
                    while :; do /bin/sleep 1; done
                fi
                if [[ ${TEST_TRAY_MANAGER_MODE:-normal} == unavailable ]]; then
                    exit 1
                fi
                case $action in
                    daemon-reload) exit 0 ;;
                    start|restart)
                        if [[ ${TEST_TRAY_MANAGER_MODE:-normal} == exit ]]; then
                            printf 'inactive\n' >"$TEST_TRAY_STATE"
                        else
                            printf 'active\n' >"$TEST_TRAY_STATE"
                        fi
                        ;;
                    stop) printf 'inactive\n' >"$TEST_TRAY_STATE" ;;
                    is-active)
                        [[ $(<"$TEST_TRAY_STATE") == active ]] && exit 0
                        exit 3
                        ;;
                    *) exit 2 ;;
                esac
                exit 0
            fi
            action=${1:-}
            if [[ $action == is-active ]]; then
                if [[ ${TEST_SYSTEMCTL_MODE:-normal} == hang-status ]]; then
                    trap '' TERM
                    while :; do /bin/sleep 1; done
                fi
                if [[ $(<"${TEST_SERVICE_STATE:?}") == active ]]; then
                    exit 0
                fi
                exit 3
            fi
            case $action in
                start|restart) printf 'active\n' >"$TEST_SERVICE_STATE" ;;
                stop|kill) printf 'inactive\n' >"$TEST_SERVICE_STATE" ;;
                *) exit 2 ;;
            esac
            """,
        )
        write_executable(
            self.bin / "sudo",
            """
            #!/usr/bin/env bash
            [[ ${1:-} == -n ]] || exit 2
            shift
            exec "$@"
            """,
        )
        write_executable(
            self.bin / "timeout",
            """
            #!/usr/bin/env bash
            if [[ ${TEST_TIMEOUT_MODE:-normal} == hang-mutation &&
                  " $* " == *" sudo -n systemctl start "* ]]; then
                printf '%s\n' "$$" >"${TEST_OBSERVER:?}"
                trap '' TERM
                while :; do /bin/sleep 1; done
            fi
            exec /usr/bin/timeout "$@"
            """,
        )
        write_executable(
            self.bin / "nc",
            f"""
            #!/usr/bin/env bash
            /bin/cat >/dev/null
            if [[ ${{TEST_NC_MODE:-normal}} == hang ]]; then
                printf '%s\n' "$$" >"${{TEST_OBSERVER:?}}"
                trap '' TERM
                while :; do /bin/sleep 1; done
            fi
            if [[ ${{TEST_NC_MODE:-normal}} == flood ]]; then
                trap '' PIPE
                while /usr/bin/head -c 65536 /dev/zero; do :; done
                /bin/sleep 0.2
                exit 0
            fi
            if [[ ${{TEST_NC_MODE:-normal}} == diagnostic-flood ]]; then
                trap '' PIPE
                while printf '%4096s' x >&2; do :; done
                /bin/sleep 0.2
                exit 1
            fi
            printf '%s\n' {STATS!r}
            """,
        )
        write_executable(self.bin / "tray", "#!/bin/sh\nexit 0\n")

    def environment(self, **overrides: str) -> dict[str, str]:
        environment = {
            **os.environ,
            "PATH": f"{self.bin}:/usr/bin:/bin",
            "PUNTO_TRAY": str(self.bin / "tray"),
            "PUNTO_SOCKET": str(self.root / "socket"),
            "PUNTO_VERSION_FILE": str(self.version),
            "PUNTO_UDEVMON_SERVICE": "udevmon",
            "PUNTO_IPC_TIMEOUT_MS": "30",
            "PUNTO_COMMAND_TIMEOUT_MS": "100",
            "PUNTO_START_TIMEOUT_MS": "30",
            "PUNTO_STOP_TIMEOUT_MS": "30",
            "PUNTO_POLL_INTERVAL_MS": "5",
            "TMPDIR": str(self.root),
            "TEST_SERVICE_STATE": str(self.service_state),
            "TEST_TRAY_STATE": str(self.tray_state),
            "TEST_OBSERVER": str(self.observer),
            "DISPLAY": ":99",
        }
        environment.update(overrides)
        return environment

    def run(self, command: str, **overrides: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [str(CLI), command],
            env=self.environment(**overrides),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=4,
            check=False,
        )


class CliContract(unittest.TestCase):
    def setUp(self) -> None:
        self.harness = CliHarness()

    def tearDown(self) -> None:
        self.harness.cleanup()

    def assert_observed_process_reaped(
        self, *, process_may_exit_before_exec: bool = False
    ) -> None:
        deadline = time.monotonic() + 1
        while not self.harness.observer.exists() and time.monotonic() < deadline:
            time.sleep(0.01)
        if process_may_exit_before_exec and not self.harness.observer.exists():
            return
        self.assertTrue(self.harness.observer.exists(), "fixture process never started")
        pid = int(self.harness.observer.read_text(encoding="ascii").strip())
        while process_exists(pid) and time.monotonic() < deadline:
            time.sleep(0.01)
        self.assertFalse(
            process_exists(pid), f"process {pid} survived failed operation"
        )

    def test_all_external_timeouts_escalate_after_term(self) -> None:
        source = CLI.read_text(encoding="utf-8")
        logical = source.replace("\\\n", " ")
        invocations = [
            line
            for line in logical.splitlines()
            if re.search(r"(^|\s)timeout\s+--", line)
        ]
        self.assertGreaterEqual(len(invocations), 3)
        for invocation in invocations:
            self.assertIn("--signal=TERM", invocation)
            self.assertIn("--kill-after=", invocation)

    def test_backend_status_timeout_is_bounded_and_reported(self) -> None:
        started = time.monotonic()
        result = self.harness.run("start", TEST_SYSTEMCTL_MODE="hang-status")
        self.assertLess(time.monotonic() - started, 1)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("ERROR service-timeout", result.stdout)

    def test_service_unit_override_is_strictly_bounded(self) -> None:
        for hostile in ("--user", "ssh.service", "udevmon@attacker.service"):
            with self.subTest(hostile=hostile):
                result = self.harness.run("start", PUNTO_UDEVMON_SERVICE=hostile)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(result.stdout, "ERROR invalid-configuration\n")

    def test_ipc_timeout_escalates_and_reaps_unresponsive_peer(self) -> None:
        result = self.harness.run("status", TEST_NC_MODE="hang")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("ERROR timeout", result.stdout)
        self.assert_observed_process_reaped()

    def test_ipc_flood_is_capped_during_receive_and_cleaned_up(self) -> None:
        source = CLI.read_text(encoding="utf-8")
        self.assertIn('head -c 4097 <"$response_fifo"', source)
        self.assertIn('head -c 4097 <"$error_fifo"', source)

        for mode, expected_error in (
            ("flood", "ERROR protocol-error\n"),
            ("diagnostic-flood", "ERROR unavailable\n"),
        ):
            with self.subTest(mode=mode):
                started = time.monotonic()
                process = subprocess.Popen(
                    [str(CLI), "status"],
                    env=self.harness.environment(
                        TEST_NC_MODE=mode, PUNTO_IPC_TIMEOUT_MS="300"
                    ),
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                )
                maximum_capture = 0
                deadline = started + 1
                while process.poll() is None and time.monotonic() < deadline:
                    for capture in self.harness.root.glob("punto-ipc.*/*"):
                        if capture.name in {"response", "error"}:
                            maximum_capture = max(
                                maximum_capture, capture.stat().st_size
                            )
                    time.sleep(0.002)
                stdout, stderr = process.communicate(timeout=1)
                self.assertLess(time.monotonic() - started, 1)
                self.assertNotEqual(process.returncode, 0)
                self.assertEqual(stdout, expected_error)
                self.assertLessEqual(maximum_capture, 4097)
                self.assertGreater(maximum_capture, 0)
                self.assertFalse(list(self.harness.root.glob("punto-ipc.*")))
                self.assertNotIn("No space left on device", stderr)

    def test_outer_mutation_watchdog_reaps_hung_job(self) -> None:
        result = self.harness.run("start", TEST_TIMEOUT_MODE="hang-mutation")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("ERROR service-timeout", result.stdout)
        self.assert_observed_process_reaped()

    def test_tray_lifecycle_has_no_pid_file_or_raw_signal_path(self) -> None:
        source = CLI.read_text(encoding="utf-8")
        self.assertNotIn("PUNTO_TRAY_PID_FILE", source)
        self.assertNotIn("tray_identity_matches", source)
        self.assertNotRegex(source, r"/proc/\$?tray")
        self.assertIn('systemctl --user "$action" -- "$TRAY_UNIT"', source)

    def test_unavailable_user_manager_is_optional_and_reported(self) -> None:
        result = self.harness.run("start", TEST_TRAY_MANAGER_MODE="unavailable")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "WARN tray-unavailable\n")
        self.assertEqual(self.harness.service_state.read_text(), "active\n")

    def test_hung_user_manager_is_bounded_and_reported(self) -> None:
        started = time.monotonic()
        result = self.harness.run("start", TEST_TRAY_MANAGER_MODE="hang")
        self.assertLess(time.monotonic() - started, 1)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "WARN tray-timeout\n")

    def test_immediate_tray_exit_fails_liveness_confirmation(self) -> None:
        result = self.harness.run("start", TEST_TRAY_MANAGER_MODE="exit")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "WARN tray-unavailable\n")
        self.assertEqual(self.harness.service_state.read_text(), "active\n")

    def test_tray_unit_override_is_strictly_bounded(self) -> None:
        result = self.harness.run("start", PUNTO_TRAY_UNIT="attacker.service")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, "ERROR invalid-configuration\n")

    def test_tray_stop_failure_does_not_block_backend_stop(self) -> None:
        self.harness.service_state.write_text("active\n", encoding="ascii")
        result = self.harness.run("stop", TEST_TRAY_MANAGER_MODE="unavailable")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "WARN tray-unavailable\n")
        self.assertEqual(self.harness.service_state.read_text(), "inactive\n")


if __name__ == "__main__":
    unittest.main()
