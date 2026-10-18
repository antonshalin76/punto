#!/usr/bin/env python3

import os
import pathlib
import re
import signal
import socket
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
    "log_dropped=0 text_mutation=x11 enabled=0 configured_enabled=1 "
    "config_pending=0 config_generation=1 config_result=ok analyzed=0 "
    "need_switch=0 corrections=0 word_dispatches=0 pending_words=0 "
    "ready_results=0 worker_threads=1 daemon_peers=1 analysis_mode=auto "
    "control_plane=primary queued_tasks=0 avg_queue_us=0 avg_analysis_us=0 "
    "avg_macro_us=0 avg_macro_payload_bytes=0 learning_ready=1 "
    "learning_pending=0 learning_failed=0 exclusions=0 learning_generation=2 "
    "learning_completed_generation=2 learning_failed_generation=0 "
    "daemon_epoch=1"
)
PREVIOUS_RELEASE_STATS = (
    STATS.split(" avg_macro_payload_bytes=", 1)[0] + " avg_tail_len=0"
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
        self.bridge_state = self.root / "bridge.state"
        self.bridge_state.write_text("inactive\n", encoding="ascii")
        self.calls = self.root / "calls"
        self.calls.touch()
        self.socket_path = self.root / "socket"
        self.runtime_socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.runtime_socket.bind(str(self.socket_path))
        self.runtime_socket.listen(1)
        self.observer = self.root / "observer.pid"
        self.version = self.root / "VERSION"
        self.version.write_text("1.2.3\n", encoding="ascii")
        self._install_fixtures()

    def cleanup(self) -> None:
        self.runtime_socket.close()
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
            printf 'systemctl %s\n' "$*" >>"$TEST_CALLS"
            if [[ ${1:-} == --user ]]; then
                shift
                action=${1:-}
                [[ $action == daemon-reload ]] && exit 0
                if [[ ${*: -1} == punto-session-bridge.service ]]; then
                    case $action in
                        start)
                            [[ ${TEST_BRIDGE_MODE:-ready} == start-fail ]] && exit 1
                            printf 'active\n' >"$TEST_BRIDGE_STATE"
                            ;;
                        stop)
                            [[ ${TEST_BRIDGE_MODE:-ready} == stop-fail ]] && exit 1
                            [[ ${TEST_BRIDGE_MODE:-ready} == sticky-stop ]] && exit 0
                            if [[ ${TEST_BRIDGE_MODE:-ready} == deactivating-stop ||
                                  ${TEST_BRIDGE_MODE:-ready} == stop-transition ]]; then
                                printf 'deactivating\n' >"$TEST_BRIDGE_STATE"
                                exit 0
                            fi
                            printf 'inactive\n' >"$TEST_BRIDGE_STATE"
                            ;;
                        show)
                            [[ $# == 5 && $2 == --property=ActiveState &&
                               $3 == --value && $4 == -- ]] || exit 2
                            if [[ ${TEST_BRIDGE_MODE:-ready} == stop-transition ]]; then
                                count=0
                                [[ ! -f $TEST_BRIDGE_PROBES ]] || count=$(<"$TEST_BRIDGE_PROBES")
                                printf '%s\n' "$((count + 1))" >"$TEST_BRIDGE_PROBES"
                                ((count < 1)) || printf 'inactive\n' >"$TEST_BRIDGE_STATE"
                            fi
                            /bin/cat "$TEST_BRIDGE_STATE"
                            ;;
                        is-active)
                            [[ $(<"$TEST_BRIDGE_STATE") == active ]] && exit 0
                            exit 3
                            ;;
                        *) exit 2 ;;
                    esac
                    exit 0
                fi
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
            if [[ $action == is-active || $action == show ]]; then
                if [[ ${TEST_SYSTEMCTL_MODE:-normal} == hang-status ]]; then
                    trap '' TERM
                    while :; do /bin/sleep 1; done
                fi
                if [[ $action == show ]]; then
                    [[ $# == 5 && $2 == --property=ActiveState &&
                       $3 == --value && $4 == -- ]] || exit 2
                    [[ ${TEST_SYSTEMCTL_MODE:-normal} == error-status ]] && exit 1
                    state=$(<"$TEST_SERVICE_STATE")
                    if [[ $state == deactivating &&
                          ( ${TEST_SYSTEMCTL_MODE:-normal} == stop-transition ||
                            ${TEST_SYSTEMCTL_MODE:-normal} == kill-transition ) ]]; then
                        count=0
                        [[ ! -f $TEST_SERVICE_PROBES ]] || count=$(<"$TEST_SERVICE_PROBES")
                        printf '%s\n' "$((count + 1))" >"$TEST_SERVICE_PROBES"
                        if ((count >= 1)); then
                            state=inactive
                            printf 'inactive\n' >"$TEST_SERVICE_STATE"
                        fi
                    fi
                    printf '%s\n' "$state"
                    exit 0
                fi
                if [[ $(<"${TEST_SERVICE_STATE:?}") == active ]]; then
                    exit 0
                fi
                exit 3
            fi
            case $action in
                start|restart) printf 'active\n' >"$TEST_SERVICE_STATE" ;;
                stop|kill)
                    [[ ${TEST_SYSTEMCTL_MODE:-normal} == stop-fail ]] && exit 1
                    [[ ${TEST_SYSTEMCTL_MODE:-normal} == sticky-transition ]] && exit 0
                    if [[ ${TEST_SYSTEMCTL_MODE:-normal} == kill-transition && $action == stop ]]; then
                        exit 0
                    fi
                    if [[ ${TEST_SYSTEMCTL_MODE:-normal} == stop-transition ||
                          ${TEST_SYSTEMCTL_MODE:-normal} == kill-transition ]]; then
                        printf 'deactivating\n' >"$TEST_SERVICE_STATE"
                        printf '0\n' >"$TEST_SERVICE_PROBES"
                        exit 0
                    fi
                    printf 'inactive\n' >"$TEST_SERVICE_STATE"
                    ;;
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
            request=$(/bin/cat)
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
            if [[ $request == CLEAR_EXCLUSIONS ]]; then
                if [[ -n ${{TEST_CLEAR_RESPONSE+x}} ]]; then
                    printf '%s\n' "$TEST_CLEAR_RESPONSE"
                else
                    printf 'OK EXCLUSIONS SCHEDULED 1 2\n'
                fi
            else
                if [[ -n ${{TEST_STATS_OVERRIDE+x}} ]]; then
                    printf '%s\n' "$TEST_STATS_OVERRIDE"
                else
                    printf '%s\n' {STATS!r}
                fi
            fi
            """,
        )
        write_executable(self.bin / "tray", "#!/bin/sh\nexit 0\n")
        write_executable(
            self.bin / "bridge",
            """
            #!/usr/bin/env bash
            [[ $# == 1 && $1 == --check-ready ]] || exit 2
            printf 'bridge probe\n' >>"$TEST_CALLS"
            [[ $(<"$TEST_BRIDGE_STATE") == active ]] || exit 75
            [[ ${TEST_BRIDGE_MODE:-ready} == probe-fail ]] && exit 75
            if [[ ${TEST_BRIDGE_MODE:-ready} == hang ]]; then
                printf '%s\n' "$$" >"$TEST_OBSERVER"
                trap '' TERM
                while :; do /bin/sleep 1; done
            fi
            if [[ ${TEST_BRIDGE_MODE:-ready} == delayed ]]; then
                count=0
                [[ ! -f $TEST_BRIDGE_PROBES ]] || count=$(<"$TEST_BRIDGE_PROBES")
                printf '%s\n' "$((count + 1))" >"$TEST_BRIDGE_PROBES"
                ((count >= 2)) || exit 75
            fi
            printf 'bridge ready\n' >>"$TEST_CALLS"
            """,
        )

    def environment(self, **overrides: str) -> dict[str, str]:
        environment = {
            **os.environ,
            "PATH": f"{self.bin}:/usr/bin:/bin",
            "PUNTO_TRAY": str(self.bin / "tray"),
            "PUNTO_SESSION_BRIDGE": str(self.bin / "bridge"),
            "PUNTO_SOCKET": str(self.socket_path),
            "PUNTO_VERSION_FILE": str(self.version),
            "PUNTO_UDEVMON_SERVICE": "udevmon",
            "PUNTO_IPC_TIMEOUT_MS": "30",
            "PUNTO_COMMAND_TIMEOUT_MS": "100",
            "PUNTO_START_TIMEOUT_MS": "30",
            "PUNTO_STOP_TIMEOUT_MS": "30",
            "PUNTO_POLL_INTERVAL_MS": "5",
            "TMPDIR": str(self.root),
            "TEST_SERVICE_STATE": str(self.service_state),
            "TEST_SERVICE_PROBES": str(self.root / "service.probes"),
            "TEST_TRAY_STATE": str(self.tray_state),
            "TEST_BRIDGE_STATE": str(self.bridge_state),
            "TEST_BRIDGE_PROBES": str(self.root / "bridge.probes"),
            "TEST_CALLS": str(self.calls),
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

    def test_reset_learning_uses_exact_bounded_ipc_contract(self) -> None:
        result = self.harness.run("reset-learning")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "OK EXCLUSIONS CLEARED\n")

    def test_status_accepts_previous_daemon_during_package_upgrade(self) -> None:
        result = self.harness.run("status", TEST_STATS_OVERRIDE=PREVIOUS_RELEASE_STATS)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, PREVIOUS_RELEASE_STATS + "\n")

    def test_reset_learning_accepts_durable_target_despite_stale_failure(self) -> None:
        stats = STATS.replace("exclusions=0", "exclusions=5").replace(
            "learning_failed_generation=0", "learning_failed_generation=1"
        )
        result = self.harness.run("reset-learning", TEST_STATS_OVERRIDE=stats)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "OK EXCLUSIONS CLEARED\n")

    def test_reset_learning_reports_target_generation_failure(self) -> None:
        stats = STATS.replace(
            "learning_completed_generation=2", "learning_completed_generation=1"
        ).replace("learning_failed_generation=0", "learning_failed_generation=2")
        result = self.harness.run("reset-learning", TEST_STATS_OVERRIDE=stats)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, "ERROR learning-error\n")

    def test_reset_learning_rejects_primary_failover_receipt(self) -> None:
        stats = STATS.replace("daemon_epoch=1", "daemon_epoch=2")
        result = self.harness.run("reset-learning", TEST_STATS_OVERRIDE=stats)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, "ERROR learning-error\n")

    def test_reset_learning_rejects_invalid_scheduled_generation(self) -> None:
        for response in (
            "OK EXCLUSIONS SCHEDULED 0 2",
            "OK EXCLUSIONS SCHEDULED 1 0",
            "OK EXCLUSIONS SCHEDULED invalid 2",
            "OK EXCLUSIONS SCHEDULED 1 invalid",
            "OK EXCLUSIONS SCHEDULED 1 2 trailing",
        ):
            with self.subTest(response=response):
                result = self.harness.run(
                    "reset-learning", TEST_CLEAR_RESPONSE=response
                )
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(result.stdout, "ERROR protocol-error\n")

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
                            try:
                                capture_size = capture.stat().st_size
                            except FileNotFoundError:
                                continue
                            maximum_capture = max(maximum_capture, capture_size)
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

    def test_bridge_readiness_precedes_every_backend_start_or_restart(self) -> None:
        for command in ("start", "restart"):
            for initial in ("active", "inactive"):
                for mode in ("ready", "delayed"):
                    with self.subTest(command=command, initial=initial, mode=mode):
                        self.harness.service_state.write_text(initial + "\n")
                        self.harness.bridge_state.write_text("inactive\n")
                        self.harness.calls.write_text("")
                        (self.harness.root / "bridge.probes").unlink(missing_ok=True)
                        result = self.harness.run(
                            command,
                            TEST_BRIDGE_MODE=mode,
                            PUNTO_START_TIMEOUT_MS="150",
                        )
                        self.assertEqual(result.returncode, 0, result.stdout)
                        calls = self.harness.calls.read_text().splitlines()
                        self.assertIn(
                            "systemctl --user start -- punto-session-bridge.service",
                            calls,
                        )
                        ready = calls.index("bridge ready")
                        mutations = [
                            i for i, call in enumerate(calls)
                            if call.startswith(("systemctl start ", "systemctl restart "))
                        ]
                        self.assertEqual(len(mutations), int(command == "restart" or initial == "inactive"))
                        self.assertTrue(all(ready < i for i in mutations))
                        self.assertNotIn(
                            "systemctl --user restart -- punto-session-bridge.service",
                            calls,
                        )
                        if mode == "delayed":
                            self.assertEqual(calls.count("bridge probe"), 3)

    def test_bridge_failure_leaves_backend_untouched(self) -> None:
        for command in ("start", "restart"):
            for initial in ("active", "inactive"):
                for mode in ("missing", "start-fail", "probe-fail", "hang"):
                    with self.subTest(command=command, initial=initial, mode=mode):
                        self.harness.service_state.write_text(initial + "\n")
                        self.harness.calls.write_text("")
                        overrides = {"TEST_BRIDGE_MODE": mode}
                        if mode == "missing":
                            overrides["PUNTO_SESSION_BRIDGE"] = str(self.harness.root / "missing")
                        result = self.harness.run(command, **overrides)
                        self.assertNotEqual(result.returncode, 0)
                        self.assertEqual(result.stdout, "ERROR bridge-unavailable\n")
                        self.assertEqual(self.harness.service_state.read_text(), initial + "\n")
                        self.assertFalse(any(
                            call.startswith(("systemctl start ", "systemctl restart ", "systemctl stop ", "systemctl kill "))
                            for call in self.harness.calls.read_text().splitlines()
                        ))
                        if mode == "hang":
                            self.assert_observed_process_reaped()

    def test_bridge_stops_only_after_backend_inactive(self) -> None:
        for initial in ("active", "inactive"):
            for mode in ("ready", "stop-fail", "sticky-stop"):
                with self.subTest(initial=initial, mode=mode):
                    self.harness.service_state.write_text(initial + "\n")
                    self.harness.bridge_state.write_text("active\n")
                    self.harness.calls.write_text("")
                    result = self.harness.run("stop", TEST_BRIDGE_MODE=mode)
                    self.assertEqual(result.returncode, int(mode != "ready"), result.stdout)
                    self.assertEqual(result.stdout, "ERROR bridge-unavailable\n" if mode != "ready" else "")
                    self.assertEqual(self.harness.service_state.read_text(), "inactive\n")
                    calls = self.harness.calls.read_text().splitlines()
                    self.assertIn("systemctl --user stop -- punto-session-bridge.service", calls)
                    stop = calls.index("systemctl --user stop -- punto-session-bridge.service")
                    if initial == "active":
                        self.assertLess(calls.index("systemctl stop udevmon"), stop)
                    self.assertEqual(self.harness.bridge_state.read_text(), "active\n" if mode != "ready" else "inactive\n")

    def test_failed_backend_stop_keeps_required_bridge_running(self) -> None:
        self.harness.service_state.write_text("active\n")
        self.harness.bridge_state.write_text("active\n")
        result = self.harness.run("stop", TEST_SYSTEMCTL_MODE="stop-fail")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, "ERROR service-error\n")
        self.assertEqual(self.harness.bridge_state.read_text(), "active\n")
        self.assertNotIn("systemctl --user stop -- punto-session-bridge.service", self.harness.calls.read_text())

    def test_backend_transition_must_finish_before_bridge_stop(self) -> None:
        for initial, mode in (
            ("activating", "stop-transition"),
            ("deactivating", "stop-transition"),
            ("active", "stop-transition"),
            ("active", "kill-transition"),
        ):
            with self.subTest(initial=initial, mode=mode):
                self.harness.service_state.write_text(initial + "\n")
                self.harness.bridge_state.write_text("active\n")
                self.harness.calls.write_text("")
                (self.harness.root / "service.probes").unlink(missing_ok=True)
                result = self.harness.run("stop", TEST_SYSTEMCTL_MODE=mode)
                self.assertEqual(result.returncode, 0, result.stdout)
                self.assertEqual(self.harness.service_state.read_text(), "inactive\n")
                calls = self.harness.calls.read_text().splitlines()
                helper_stop = calls.index("systemctl --user stop -- punto-session-bridge.service")
                self.assertIn("systemctl stop udevmon", calls[:helper_stop])
                if mode == "kill-transition":
                    kill = calls.index("systemctl kill --kill-who=all --signal=TERM udevmon")
                    self.assertGreaterEqual(calls[kill + 1:helper_stop].count(
                        "systemctl show --property=ActiveState --value -- udevmon"
                    ), 2)

    def test_start_during_transition_cannot_accept_old_ready_daemon(self) -> None:
        for state in ("activating", "deactivating"):
            with self.subTest(state=state):
                self.harness.service_state.write_text(state + "\n")
                self.harness.calls.write_text("")
                result = self.harness.run("start", PUNTO_START_TIMEOUT_MS="150")
                self.assertEqual(result.returncode, 0, result.stdout)
                self.assertEqual(self.harness.service_state.read_text(), "active\n")
                calls = self.harness.calls.read_text().splitlines()
                self.assertLess(calls.index("bridge ready"), calls.index("systemctl start udevmon"))

    def test_start_recovers_failed_backend_after_bridge_ready(self) -> None:
        self.harness.service_state.write_text("failed\n")
        result = self.harness.run("start", PUNTO_START_TIMEOUT_MS="150")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(self.harness.service_state.read_text(), "active\n")
        calls = self.harness.calls.read_text().splitlines()
        self.assertLess(calls.index("bridge ready"), calls.index("systemctl start udevmon"))

    def test_stop_failed_backend_requires_positive_inactive_before_bridge_stop(self) -> None:
        self.harness.service_state.write_text("failed\n")
        self.harness.bridge_state.write_text("active\n")
        result = self.harness.run("stop")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(self.harness.service_state.read_text(), "inactive\n")
        self.assertEqual(self.harness.bridge_state.read_text(), "inactive\n")
        calls = self.harness.calls.read_text().splitlines()
        stop = calls.index("systemctl stop udevmon")
        helper_stop = calls.index("systemctl --user stop -- punto-session-bridge.service")
        self.assertIn("systemctl show --property=ActiveState --value -- udevmon", calls[stop + 1:helper_stop])

    def test_unknown_backend_state_never_stops_bridge(self) -> None:
        for state, mode in (("unknown", "normal"), ("active", "error-status")):
            with self.subTest(state=state, mode=mode):
                self.harness.service_state.write_text(state + "\n")
                self.harness.bridge_state.write_text("active\n")
                self.harness.calls.write_text("")
                result = self.harness.run("stop", TEST_SYSTEMCTL_MODE=mode)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(result.stdout, "ERROR service-error\n")
                self.assertEqual(self.harness.bridge_state.read_text(), "active\n")
                self.assertNotIn("systemctl --user stop -- punto-session-bridge.service", self.harness.calls.read_text())

    def test_unfinished_backend_transition_keeps_bridge_running(self) -> None:
        for state in ("activating", "deactivating"):
            with self.subTest(state=state):
                self.harness.service_state.write_text(state + "\n")
                self.harness.bridge_state.write_text("active\n")
                self.harness.calls.write_text("")
                started = time.monotonic()
                result = self.harness.run("stop", TEST_SYSTEMCTL_MODE="sticky-transition")
                self.assertLess(time.monotonic() - started, 1)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(result.stdout, "ERROR service-error\n")
                self.assertEqual(self.harness.bridge_state.read_text(), "active\n")
                self.assertNotIn("systemctl --user stop -- punto-session-bridge.service", self.harness.calls.read_text())

    def test_bridge_deactivating_is_not_confirmed_stopped(self) -> None:
        self.harness.service_state.write_text("inactive\n")
        self.harness.bridge_state.write_text("active\n")
        result = self.harness.run("stop", TEST_BRIDGE_MODE="deactivating-stop")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, "ERROR bridge-unavailable\n")
        self.assertEqual(self.harness.bridge_state.read_text(), "deactivating\n")

    def test_bridge_stop_waits_for_transition_to_inactive(self) -> None:
        self.harness.service_state.write_text("inactive\n")
        self.harness.bridge_state.write_text("active\n")
        result = self.harness.run("stop", TEST_BRIDGE_MODE="stop-transition")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(self.harness.bridge_state.read_text(), "inactive\n")
        self.assertEqual(self.harness.calls.read_text().splitlines().count(
            "systemctl --user show --property=ActiveState --value -- punto-session-bridge.service"
        ), 2)


if __name__ == "__main__":
    unittest.main()
