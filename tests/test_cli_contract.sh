#!/usr/bin/env bash
# RED contract for B11/B26. The CLI runs privilege-dropped in bubblewrap,
# talks to a real AF_UNIX fixture, and can reach only command spies.

set -u -o pipefail

REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
CLI_SCRIPT="$REPO_ROOT/punto-cli.sh"
EXPECTED_VERSION=""
VERSION_VALID=0
SERVICE=udevmon.service
failures=0
checks=0
tmp_root=""
fixture_pid=""
CLI_RC=0
CLI_OUTPUT=""
CLI_OUTPUT_FILE=""
CLI_DURATION_MS=0
CLI_RUN_SERIAL=0
CLI_LAST_TRACE_DIR=""

SYSTEMCTL_MODE=ok
TRAY_MODE=ok
TRAY_PATH=""
IPC_TIMEOUT_MS=50
COMMAND_TIMEOUT_MS=120
START_TIMEOUT_MS=40
STOP_TIMEOUT_MS=40
POLL_INTERVAL_MS=10
TRANSITION_AT_MS=""
TRANSITION_STATE=""
SEED_TRAY=0
TRAY_PID_OVERRIDE=""
# Variables intentionally expand inside the bwrap shell.
# shellcheck disable=SC2016
INNER_RUNNER='
/usr/bin/id -u >"$TEST_EUID_FILE"
seed_pid=""
sentinel_pid=""
if [[ ${TEST_SEED_TRAY:-0} == 1 ]]; then
    TEST_TRAY_MODE=ok TEST_TRAY_ROLE=seed "${TEST_TRAY_FIXTURE:?}" &
    seed_pid=$!
    seed_ready=0
    for ((attempt = 0; attempt < 200; ++attempt)); do
        if [[ $(<"$TEST_TRAY_OWNER_FILE") == "$seed_pid" &&
              $(<"$TEST_TRAY_STATE") == active ]]; then
            seed_ready=1
            break
        fi
        /usr/bin/python3 -c "import time; time.sleep(0.002)"
    done
    if [[ $seed_ready != 1 ]]; then
        printf "HARNESS seed-not-ready pid=%s\n" "$seed_pid" >>"$TEST_CALLS"
        builtin kill -TERM "$seed_pid" 2>/dev/null || true
        exit 98
    fi
    printf "%s\n" "$seed_pid" >"$PUNTO_TRAY_PID_FILE"
    if [[ -n ${TEST_TRAY_PID_OVERRIDE:-} ]]; then
        if [[ $TEST_TRAY_PID_OVERRIDE == sentinel ]]; then
            "${TEST_SENTINEL_FIXTURE:?}" &
            sentinel_pid=$!
            sentinel_ready=0
            for ((attempt = 0; attempt < 200; ++attempt)); do
                if [[ $(<"$TEST_SENTINEL_READY_FILE") == "$sentinel_pid" ]]; then
                    sentinel_ready=1
                    break
                fi
                /usr/bin/python3 -c "import time; time.sleep(0.002)"
            done
            if [[ $sentinel_ready != 1 ]]; then
                printf "HARNESS sentinel-not-ready pid=%s\n" "$sentinel_pid" >>"$TEST_CALLS"
                builtin kill -TERM "$seed_pid" "$sentinel_pid" 2>/dev/null || true
                exit 98
            fi
            printf "%s\n" "$sentinel_pid" >"$PUNTO_TRAY_PID_FILE"
        else
            printf "%s\n" "$TEST_TRAY_PID_OVERRIDE" >"$PUNTO_TRAY_PID_FILE"
        fi
    fi
fi
rm -f -- "$TEST_CLI_RC_FILE"
/usr/bin/strace -ff -qq -s 4096 \
    -e trace=execve,kill,tgkill,tkill,pidfd_send_signal \
    -o "$TEST_EXEC_TRACE" "$TEST_SETSID_REAL" "$TEST_TRACE_WRAPPER" "$@" &
trace_pid=$!
cli_complete=0
for ((attempt = 0; attempt < 300; ++attempt)); do
    if [[ -s $TEST_CLI_RC_FILE ]]; then
        cli_complete=1
        break
    fi
    /usr/bin/python3 -c "import time; time.sleep(0.005)"
done
if [[ $cli_complete != 1 ]]; then
    printf "HARNESS cli-not-complete trace-pid=%s\n" "$trace_pid" >>"$TEST_CALLS"
    cli_rc=98
else
    cli_rc=$(<"$TEST_CLI_RC_FILE")
fi
/usr/bin/python3 -c "import time; time.sleep(0.01)"
owner=$(<"$TEST_TRAY_OWNER_FILE")
tray_state=$(<"$TEST_TRAY_STATE")
if [[ $owner =~ ^[1-9][0-9]*$ && $owner != "$seed_pid" && $tray_state == active ]]; then
    read -r cli_pid cli_pgid cli_sid <"$TEST_CLI_SESSION_FILE"
    read -r tray_pid tray_pgid tray_sid <"$TEST_TRAY_SESSION_FILE"
    read -r _ _ _ _ tracer_pgid tracer_sid _ <"/proc/$trace_pid/stat"
    if [[ $cli_pid =~ ^[1-9][0-9]*$ && $cli_pgid =~ ^[1-9][0-9]*$ &&
          $cli_sid =~ ^[1-9][0-9]*$ && $tracer_pgid =~ ^[1-9][0-9]*$ &&
          $tracer_sid =~ ^[1-9][0-9]*$ && $cli_pgid != "$tracer_pgid" &&
          $tray_pid == "$owner" && $owner != "$trace_pid" ]]; then
        printf "HUP_ORACLE separated cli-pid=%s cli-pgid=%s cli-sid=%s tracer-pid=%s tracer-pgid=%s tracer-sid=%s\n" \
            "$cli_pid" "$cli_pgid" "$cli_sid" "$trace_pid" "$tracer_pgid" "$tracer_sid" \
            >>"$TEST_CALLS"
        builtin kill -HUP -- "-$cli_pgid" 2>/dev/null || true
        builtin kill -HUP "$owner" 2>/dev/null || true
    else
        printf "HUP_ORACLE unsafe cli-pid=%s cli-pgid=%s cli-sid=%s tracer-pid=%s tracer-pgid=%s tracer-sid=%s\n" \
            "${cli_pid:-}" "${cli_pgid:-}" "${cli_sid:-}" "$trace_pid" \
            "${tracer_pgid:-}" "${tracer_sid:-}" >>"$TEST_CALLS"
    fi
    /usr/bin/python3 -c "import time; time.sleep(0.01)"
    if builtin kill -0 "$trace_pid" 2>/dev/null; then
        printf "TRACER_OBSERVER alive %s\n" "$trace_pid" >>"$TEST_CALLS"
    else
        printf "TRACER_OBSERVER stopped %s\n" "$trace_pid" >>"$TEST_CALLS"
    fi
fi
seed_running=0
if [[ $seed_pid =~ ^[1-9][0-9]*$ ]]; then
    while IFS= read -r running_pid; do
        [[ $running_pid == "$seed_pid" ]] && seed_running=1
    done < <(jobs -pr)
fi
if [[ $owner =~ ^[1-9][0-9]*$ ]]; then
    if [[ $owner == "$seed_pid" && $seed_running == 1 ]] ||
       [[ $owner != "$seed_pid" ]] && builtin kill -0 "$owner" 2>/dev/null; then
        printf "TRAY_OBSERVER current-alive %s\n" "$owner" >>"$TEST_CALLS"
    else
        printf "TRAY_OBSERVER current-dead %s\n" "$owner" >>"$TEST_CALLS"
    fi
fi
if [[ $seed_pid =~ ^[1-9][0-9]*$ ]]; then
    if [[ $seed_running == 1 ]]; then
        printf "TRAY_OBSERVER seed-alive %s\n" "$seed_pid" >>"$TEST_CALLS"
    else
        printf "TRAY_OBSERVER seed-stopped %s\n" "$seed_pid" >>"$TEST_CALLS"
    fi
fi
if [[ $sentinel_pid =~ ^[1-9][0-9]*$ ]]; then
    sentinel_running=0
    while IFS= read -r running_pid; do
        [[ $running_pid == "$sentinel_pid" ]] && sentinel_running=1
    done < <(jobs -pr)
    if [[ $sentinel_running == 1 ]]; then
        printf "SENTINEL_OBSERVER alive %s signals=%s\n" "$sentinel_pid" \
            "$(<"$TEST_SENTINEL_SIGNAL_FILE")" >>"$TEST_CALLS"
    else
        printf "SENTINEL_OBSERVER stopped %s signals=%s\n" "$sentinel_pid" \
            "$(<"$TEST_SENTINEL_SIGNAL_FILE")" >>"$TEST_CALLS"
    fi
fi
: >"$TEST_HARNESS_CLEANUP_FILE"
if [[ $owner =~ ^[1-9][0-9]*$ ]]; then
    builtin kill -TERM "$owner" 2>/dev/null || true
fi
if [[ $seed_pid =~ ^[1-9][0-9]*$ && $seed_pid != "$owner" ]]; then
    builtin kill -TERM "$seed_pid" 2>/dev/null || true
fi
if [[ $sentinel_pid =~ ^[1-9][0-9]*$ ]]; then
    builtin kill -TERM "$sentinel_pid" 2>/dev/null || true
fi
hang_pid=$(<"$TEST_HANG_PID_FILE")
if [[ $hang_pid =~ ^[1-9][0-9]*$ ]]; then
    builtin kill -HUP "$hang_pid" 2>/dev/null || true
fi
/usr/bin/python3 -c "import time; time.sleep(0.01)"
wait "$trace_pid" 2>/dev/null || true
exit "$cli_rc"
'

VALID_STATS='OK x11_health=ready analysis_health=degraded input_health=failed x11_last_progress_ms=0 analysis_last_progress_ms=18446744073709551615 input_last_progress_ms=30 analysis_outstanding=2 input_in_flight=1 log_dropped=12 text_mutation=disabled enabled=0 configured_enabled=1 config_pending=0 config_generation=13 config_result=ok analyzed=0 need_switch=1 corrections=2 pending_words=3 ready_results=4 worker_threads=5 daemon_peers=6 analysis_mode=fixed control_plane=primary queued_tasks=7 avg_queue_us=8 avg_analysis_us=9 avg_macro_us=10 avg_tail_len=11'
ERROR_CATEGORIES=(
    unavailable denied timeout protocol-error daemon-error service-error
    service-timeout tray-error invalid-configuration usage-error
)

pass() {
    checks=$((checks + 1))
    printf 'PASS: %s\n' "$1"
}

fail() {
    checks=$((checks + 1))
    failures=$((failures + 1))
    printf 'FAIL: %s\n' "$1" >&2
}

plain_output() {
    sed $'s/\033\\[[0-9;]*m//g'
}

assert_zero() {
    local rc=$1 message=$2
    if [[ $rc -eq 0 ]]; then
        pass "$message"
    else
        fail "$message (rc=$rc)"
    fi
}

assert_nonzero() {
    local rc=$1 message=$2
    if [[ $rc -ge 1 && $rc -le 125 ]]; then
        pass "$message"
    else
        fail "$message (rc=$rc)"
    fi
}

assert_contains() {
    local haystack=$1 needle=$2 message=$3
    if grep -Fq -- "$needle" <<<"$haystack"; then
        pass "$message"
    else
        fail "$message (missing: $needle)"
    fi
}

assert_not_contains() {
    local haystack=$1 needle=$2 message=$3
    if grep -Fq -- "$needle" <<<"$haystack"; then
        fail "$message (found: $needle)"
    else
        pass "$message"
    fi
}

assert_matches() {
    local haystack=$1 pattern=$2 message=$3
    if grep -Eiq -- "$pattern" <<<"$haystack"; then
        pass "$message"
    else
        fail "$message (pattern: $pattern)"
    fi
}

parse_canonical_version_file() {
    /usr/bin/python3 - "$1" <<'PY'
import pathlib
import re
import sys

data = pathlib.Path(sys.argv[1]).read_bytes()
pattern = rb"[0-9]+(?:[.][0-9]+){2}"
if not (2 <= len(data) <= 128 and data.endswith(b"\n") and data.count(b"\n") == 1):
    raise SystemExit(2)
value = data[:-1]
if re.fullmatch(pattern, value) is None:
    raise SystemExit(2)
sys.stdout.write(value.decode("ascii"))
PY
}

load_root_version() {
    local version_file="$REPO_ROOT/VERSION" parsed parse_rc
    if [[ ! -f $version_file || -L $version_file ]]; then
        fail "B26: repository VERSION is an existing regular non-symlink file"
        return
    fi
    set +e
    parsed=$(parse_canonical_version_file "$version_file" 2>/dev/null)
    parse_rc=$?
    set +e
    EXPECTED_VERSION=$parsed
    if [[ $parse_rc -ne 0 ]] || \
       ! dpkg --validate-version "$EXPECTED_VERSION" >/dev/null 2>&1; then
        fail "B26: repository VERSION is one LF-terminated valid Debian version"
        EXPECTED_VERSION=""
        return
    fi
    VERSION_VALID=1
    pass "B26: repository VERSION is one LF-terminated valid Debian version"
}

assert_bounded() {
    local rc=$1 duration=$2 bound=$3 message=$4
    if [[ $rc -ne 124 && $rc -ne 137 && $duration -le $bound ]]; then
        pass "$message"
    else
        fail "$message (rc=$rc, duration=${duration}ms, bound=${bound}ms)"
    fi
}

assert_error_category() {
    local expected=$1 message=$2 allowed_warning=${3:-} plain expected_output
    local category error_lines success_lines
    plain=$(plain_output <<<"$CLI_OUTPUT")
    expected_output="ERROR $expected"
    if [[ -n $allowed_warning ]]; then
        expected_output+=$'\n'"$allowed_warning"
    fi
    error_lines=$(grep -Ec '^ERROR([[:space:]]|$)' <<<"$plain" || true)
    success_lines=$(grep -Ec '^OK([[:space:]]|$)' <<<"$plain" || true)
    if [[ $error_lines -eq 1 && $plain == "$expected_output" ]] && \
       cli_output_is_exact "$expected_output"; then
        pass "$message has exact stable output for category $expected"
    else
        fail "$message expected exact output '${expected_output//$'\n'/; }' (output: ${plain//$'\n'/; })"
    fi
    if [[ $success_lines -eq 0 ]]; then
        pass "$message does not also report success"
    else
        fail "$message mixed ERROR $expected with $success_lines OK response line(s)"
    fi
    for category in "${ERROR_CATEGORIES[@]}"; do
        [[ $category == "$expected" ]] && continue
        if grep -Fxq -- "ERROR $category" <<<"$plain"; then
            fail "$message also reported mutually exclusive category $category"
        fi
    done
}

assert_output_exact_line() {
    local expected=$1 message=$2 plain count
    plain=$(plain_output <<<"$CLI_OUTPUT")
    count=$(grep -Fxc -- "$expected" <<<"$plain" || true)
    if [[ $count -eq 1 ]]; then
        pass "$message"
    else
        fail "$message (expected one '$expected' line, got $count)"
    fi
}

cli_output_is_exact() {
    local expected=$1
    printf '%s\n' "$expected" | cmp -s - "$CLI_OUTPUT_FILE"
}

stop_fixture() {
    local attempt
    if [[ -n ${fixture_pid:-} ]]; then
        kill -TERM "$fixture_pid" 2>/dev/null || true
        for ((attempt = 0; attempt < 100; ++attempt)); do
            kill -0 "$fixture_pid" 2>/dev/null || break
            /usr/bin/sleep 0.01
        done
        if kill -0 "$fixture_pid" 2>/dev/null; then
            kill -KILL "$fixture_pid" 2>/dev/null || true
        fi
        wait "$fixture_pid" 2>/dev/null || true
        fixture_pid=""
    fi
    if [[ -n ${tmp_root:-} ]]; then
        rm -f -- "$tmp_root/run/punto.sock" "$tmp_root/fixture.ready"
    fi
}

cleanup() {
    stop_fixture
    if [[ ${PUNTO_CONTRACT_KEEP_TMP:-0} == 1 ]]; then
        printf 'NOTE: retained CLI workspace: %s\n' "$tmp_root" >&2
        return
    fi
    if [[ -n ${tmp_root:-} && $tmp_root == /tmp/punto-cli-contract.* ]]; then
        chmod -R u+w -- "$tmp_root" 2>/dev/null || true
        rm -rf -- "$tmp_root"
    fi
}
trap cleanup EXIT
trap 'cleanup; trap - EXIT; exit 130' INT
trap 'cleanup; trap - EXIT; exit 143' TERM

missing_tools=""
# bwrap/python3/nc exercise product-facing isolation and transport. The rest
# are deterministic harness utilities supplied by Debian Essential/base tools.
for required in bwrap cmp dpkg python3 strace timeout head mkfifo rmdir nc sed grep wc chmod \
    mktemp cp rm tr cut basename; do
    if ! command -v "$required" >/dev/null 2>&1; then
        missing_tools+=" $required"
    fi
done
if [[ -n $missing_tools ]]; then
    printf 'SKIP: CLI contract prerequisites missing:%s\n' "$missing_tools"
    exit 77
fi
if [[ ! -x /usr/bin/nc.openbsd ]]; then
    printf 'SKIP: CLI contract requires /usr/bin/nc.openbsd from netcat-openbsd\n'
    exit 77
fi

tmp_root=$(mktemp -d /tmp/punto-cli-contract.XXXXXX)
mkdir -p "$tmp_root/bin" "$tmp_root/execve" "$tmp_root/home" "$tmp_root/run"
chmod 0777 "$tmp_root" "$tmp_root/home" "$tmp_root/run"
for file in calls.log requests.log timeline.log response.bin service.state \
    tray.state tray.pid tray.owner tray.real tray.ready tray.session harness.cleanup \
    clock.ms euid.log cli.session \
    sentinel.ready sentinel.signals hang.pid; do
    : >"$tmp_root/$file"
    chmod 0666 "$tmp_root/$file"
done
mkfifo "$tmp_root/tray.wait"
chmod 0666 "$tmp_root/tray.wait"
printf 'inactive\n' >"$tmp_root/service.state"
printf 'inactive\n' >"$tmp_root/tray.state"
printf '0\n' >"$tmp_root/clock.ms"
printf '0\n' >"$tmp_root/sentinel.signals"
TRAY_PATH="$tmp_root/bin/punto-tray-fixture"
load_root_version
if [[ $VERSION_VALID -eq 1 ]]; then
    cp -- "$REPO_ROOT/VERSION" "$tmp_root/VERSION"
    chmod 0644 "$tmp_root/VERSION"
else
    rm -f -- "$tmp_root/VERSION"
fi

cat >"$tmp_root/trace-wrapper" <<'SH'
#!/usr/bin/env bash
read -r _ _ _ _ process_group session_id _ <"/proc/$$/stat"
printf '%s %s %s\n' "$$" "$process_group" "$session_id" \
    >"${TEST_CLI_SESSION_FILE:?}"
/bin/bash "$@"
rc=$?
printf '%s\n' "$rc" >"${TEST_CLI_RC_FILE:?}"
exit "$rc"
SH
chmod 0755 "$tmp_root/trace-wrapper"

cat >"$tmp_root/ipc_fixture.py" <<'PY'
#!/usr/bin/env python3
import os
import signal
import socket
import sys
import threading

socket_path, mode, response_path, requests_path, timeline_path, ready_path = sys.argv[1:]
stopping = threading.Event()


def stop(_signum, _frame):
    stopping.set()


def append(path, payload):
    descriptor = os.open(path, os.O_WRONLY | os.O_APPEND)
    try:
        os.write(descriptor, payload)
    finally:
        os.close(descriptor)


signal.signal(signal.SIGTERM, stop)
signal.signal(signal.SIGINT, stop)
try:
    os.unlink(socket_path)
except FileNotFoundError:
    pass

server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
server.bind(socket_path)
server.listen(16)
server.settimeout(0.05)
os.chmod(socket_path, 0 if mode == "denied" else 0o660)
with open(ready_path, "w", encoding="ascii") as ready:
    ready.write("READY\n")

while not stopping.is_set():
    try:
        connection, _ = server.accept()
    except socket.timeout:
        continue
    except OSError:
        break
    with connection:
        connection.settimeout(0.5)
        request = bytearray()
        try:
            while len(request) < 4096 and b"\n" not in request:
                chunk = connection.recv(512)
                if not chunk:
                    break
                request.extend(chunk)
        except (socket.timeout, OSError):
            pass
        append(requests_path, bytes(request).hex().encode("ascii") + b"\n")
        printable = bytes(request).rstrip(b"\n").decode("ascii", "replace")
        append(timeline_path, f"IPC {printable}\n".encode("ascii", "replace"))
        if mode == "stall":
            stopping.wait(30.0)
            continue
        try:
            with open(response_path, "rb") as response_file:
                response = response_file.read()
            if response:
                connection.sendall(response)
        except OSError:
            pass

server.close()
try:
    os.unlink(socket_path)
except FileNotFoundError:
    pass
PY
chmod 0755 "$tmp_root/ipc_fixture.py"

cat >"$tmp_root/fixture_probe.py" <<'PY'
#!/usr/bin/env python3
import os
import socket
import sys

path = sys.argv[1]
sys.stdout.write(f"EUID={os.geteuid()}\n")
sys.stdout.flush()
client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
client.settimeout(0.5)
client.connect(path)
client.sendall(b"STATS\n")
response = bytearray()
while True:
    part = client.recv(4096)
    if not part:
        break
    response.extend(part)
client.close()
sys.stdout.write(f"RESPONSE_HEX={bytes(response).hex()}\n")
PY
chmod 0755 "$tmp_root/fixture_probe.py"

cat >"$tmp_root/bin/systemctl" <<'SPY'
#!/usr/bin/env bash
set -u

log() {
    printf 'SYSTEMCTL' >>"${TEST_CALLS:?}"
    printf ' %q' "$@" >>"$TEST_CALLS"
    printf '\n' >>"$TEST_CALLS"
    printf 'SYSTEMCTL' >>"${TEST_TIMELINE:?}"
    printf ' %q' "$@" >>"$TEST_TIMELINE"
    printf '\n' >>"$TEST_TIMELINE"
}

has_mode() {
    case ",${TEST_SYSTEMCTL_MODE:-ok}," in *",$1,"*) return 0 ;; *) return 1 ;; esac
}

hang_forever() {
    printf '%s\n' "$$" >"${TEST_HANG_PID_FILE:?}"
    exec /usr/bin/python3 -c 'import signal,time; signal.signal(signal.SIGTERM, signal.SIG_IGN); signal.signal(signal.SIGINT, signal.SIG_IGN); time.sleep(30)'
}

log "$@"
if [[ ${1:-} == --user ]]; then
    action=${2:-}
    case $action in
        daemon-reload)
            if [[ $# -ne 2 ]]; then
                printf 'MUTATION invalid-tray-reload-argv\n' >>"$TEST_CALLS"
                exit 97
            fi
            [[ ${TEST_TRAY_MODE:-ok} == manager-fail ]] && exit 5
            [[ ${TEST_TRAY_MODE:-ok} == manager-hang ]] && hang_forever
            exit 0
            ;;
        start|restart|stop)
            if [[ $# -ne 4 || ${3:-} != -- ||
                  ${4:-} != punto-tray.service ]]; then
                printf 'MUTATION invalid-tray-%s-argv\n' "$action" >>"$TEST_CALLS"
                exit 97
            fi
            [[ ${TEST_TRAY_MODE:-ok} == hang ]] && hang_forever
            [[ ${TEST_TRAY_MODE:-ok} == fail ]] && exit 5
            if [[ $action == stop ]]; then
                printf 'inactive\n' >"$TEST_TRAY_STATE"
            else
                printf 'active\n' >"$TEST_TRAY_STATE"
            fi
            exit 0
            ;;
        is-active)
            if [[ $# -ne 5 || ${3:-} != --quiet || ${4:-} != -- ||
                  ${5:-} != punto-tray.service ]]; then
                printf 'MUTATION invalid-tray-is-active-argv\n' >>"$TEST_CALLS"
                exit 97
            fi
            [[ ${TEST_TRAY_MODE:-ok} == status-hang ]] && hang_forever
            [[ ${TEST_TRAY_MODE:-ok} == status-fail ]] && exit 5
            [[ $(<"$TEST_TRAY_STATE") == active ]] && exit 0
            exit 3
            ;;
        *)
            printf 'MUTATION unsupported-tray-systemctl-action %q\n' \
                "$action" >>"$TEST_CALLS"
            exit 97
            ;;
    esac
fi
action=${1:-}
case $action in
    is-active)
        if [[ $# -ne 4 || ${2:-} != --quiet || ${3:-} != -- ||
              ${4:-} != "${TEST_SERVICE:?}" ]]; then
            printf 'MUTATION invalid-is-active-argv\n' >>"$TEST_CALLS"
            exit 97
        fi
        [[ $(<"${TEST_SERVICE_STATE:?}") == active ]] && exit 0
        exit 3
        ;;
    start|restart)
        if [[ $# -ne 2 || ${2:-} != "${TEST_SERVICE:?}" ]]; then
            printf 'MUTATION invalid-%s-argv\n' "$action" >>"$TEST_CALLS"
            exit 97
        fi
        has_mode "hang-$action" && hang_forever
        has_mode "fail-$action" && exit 5
        printf 'active\n' >"$TEST_SERVICE_STATE"
        ;;
    stop)
        if [[ $# -ne 2 || ${2:-} != "${TEST_SERVICE:?}" ]]; then
            printf 'MUTATION invalid-stop-argv\n' >>"$TEST_CALLS"
            exit 97
        fi
        has_mode hang-stop && hang_forever
        has_mode fail-stop && exit 5
        if ! has_mode sticky-stop && ! has_mode sticky-all; then
            printf 'inactive\n' >"$TEST_SERVICE_STATE"
        fi
        ;;
    kill)
        if [[ $# -ne 4 || ${2:-} != --kill-who=all || ${3:-} != --signal=TERM || \
              ${4:-} != "${TEST_SERVICE:?}" ]]; then
            printf 'MUTATION invalid-systemctl-kill-argv\n' >>"$TEST_CALLS"
            exit 97
        fi
        has_mode hang-kill && hang_forever
        has_mode fail-kill && exit 5
        if ! has_mode sticky-kill && ! has_mode sticky-all; then
            printf 'inactive\n' >"$TEST_SERVICE_STATE"
        fi
        ;;
    *)
        printf 'MUTATION unsupported-systemctl-action %q\n' "$action" >>"$TEST_CALLS"
        exit 97
        ;;
esac
SPY

cat >"$tmp_root/bin/sudo" <<'SPY'
#!/usr/bin/env bash
set -u
printf 'SUDO' >>"${TEST_CALLS:?}"
printf ' %q' "$@" >>"$TEST_CALLS"
printf '\n' >>"$TEST_CALLS"
printf 'SUDO' >>"${TEST_TIMELINE:?}"
printf ' %q' "$@" >>"$TEST_TIMELINE"
printf '\n' >>"$TEST_TIMELINE"
if [[ ${1:-} != -n || ${2:-} != systemctl ]]; then
    printf 'MUTATION invalid-sudo-argv' >>"$TEST_CALLS"
    printf ' %q' "$@" >>"$TEST_CALLS"
    printf '\n' >>"$TEST_CALLS"
    exit 97
fi
shift 2
exec systemctl "$@"
SPY

for forbidden in pgrep pkill pidof killall ps; do
    cat >"$tmp_root/bin/$forbidden" <<'SPY'
#!/usr/bin/env bash
set -u
printf 'FORBIDDEN %s' "$(basename "$0")" >>"${TEST_CALLS:?}"
printf ' %q' "$@" >>"$TEST_CALLS"
printf '\n' >>"$TEST_CALLS"
exit 97
SPY
done

cat >"$tmp_root/bin/kill" <<'SPY'
#!/usr/bin/env bash
set -u
printf 'FORBIDDEN external-kill' >>"${TEST_CALLS:?}"
printf ' %q' "$@" >>"$TEST_CALLS"
printf '\n' >>"$TEST_CALLS"
exit 97
SPY

cat >"$tmp_root/bin/timeout" <<'SPY'
#!/usr/bin/env bash
set -u
printf 'TIMEOUT' >>"${TEST_CALLS:?}"
printf ' %q' "$@" >>"$TEST_CALLS"
printf '\n' >>"$TEST_CALLS"
previous=""
for argument in "$@"; do
    case $argument in
        --kill-after=0.200s)
            ;;
        -k|--kill-after|-k*|--kill-after=*)
            printf 'SIGKILL forbidden-timeout-escalation %q\n' "$argument" >>"$TEST_CALLS"
            exit 97
            ;;
        -s9|-sKILL|-sSIGKILL|--signal=9|--signal=KILL|--signal=SIGKILL)
            printf 'SIGKILL forbidden-timeout-signal %q\n' "$argument" >>"$TEST_CALLS"
            exit 97
            ;;
        9|KILL|SIGKILL)
            if [[ $previous == -s || $previous == --signal ]]; then
                printf 'SIGKILL forbidden-timeout-signal %q %q\n' "$previous" "$argument" >>"$TEST_CALLS"
                exit 97
            fi
            ;;
    esac
    previous=$argument
done
exec /usr/bin/timeout "$@"
SPY

cat >"$tmp_root/bin/nc" <<'SPY'
#!/usr/bin/env bash
set -u
printf 'NC' >>"${TEST_CALLS:?}"
printf ' %q' "$@" >>"$TEST_CALLS"
printf '\n' >>"$TEST_CALLS"
if [[ $# -ne 2 || ${1:-} != -U || ${2:-} != "${PUNTO_SOCKET:?}" ]]; then
    printf 'MUTATION invalid-nc-argv' >>"$TEST_CALLS"
    printf ' %q' "$@" >>"$TEST_CALLS"
    printf '\n' >>"$TEST_CALLS"
    exit 97
fi
exec "${TEST_NC_REAL:?}" "$@"
SPY
cp -- /usr/bin/nc.openbsd "$tmp_root/bin/nc-real"
chmod 0755 "$tmp_root/bin/nc-real"
cp -- /usr/bin/nohup "$tmp_root/bin/nohup-real"
cp -- /usr/bin/setsid "$tmp_root/bin/setsid-real"
chmod 0755 "$tmp_root/bin/nohup-real" "$tmp_root/bin/setsid-real"

cat >"$tmp_root/bin/nohup" <<'SPY'
#!/usr/bin/env bash
set -u
printf 'LAUNCHER nohup' >>"${TEST_CALLS:?}"
printf ' %q' "$@" >>"$TEST_CALLS"
printf '\n' >>"$TEST_CALLS"
exec "${TEST_NOHUP_REAL:?}" "$@"
SPY

cat >"$tmp_root/bin/setsid" <<'SPY'
#!/usr/bin/env bash
set -u
printf 'LAUNCHER setsid' >>"${TEST_CALLS:?}"
printf ' %q' "$@" >>"$TEST_CALLS"
printf '\n' >>"$TEST_CALLS"
exec "${TEST_SETSID_REAL:?}" "$@"
SPY

for launcher in sg newgrp; do
    cat >"$tmp_root/bin/$launcher" <<'SPY'
#!/usr/bin/env bash
set -u
printf 'MUTATION unsupported-tray-launcher %s' "$(basename "$0")" >>"${TEST_CALLS:?}"
printf ' %q' "$@" >>"$TEST_CALLS"
printf '\n' >>"$TEST_CALLS"
exit 97
SPY
done

cat >"$tmp_root/bin/id" <<'SPY'
#!/usr/bin/env bash
case ${1:-} in
    -u) printf '65534\n' ;;
    -un) printf 'punto-contract-user\n' ;;
    -nG) printf 'punto\n' ;;
    *) printf 'uid=65534(punto-contract-user) gid=65534(nogroup) groups=65534(nogroup),981(punto)\n' ;;
esac
SPY

cat >"$tmp_root/bin/sleep" <<'SPY'
#!/usr/bin/env bash
set -u
value=${1:-}
printf 'SLEEP %q\n' "$value" >>"${TEST_CALLS:?}"
if [[ ! $value =~ ^([0-9]+)([.]([0-9]+))?s?$ ]]; then
    printf 'MUTATION invalid-sleep %q\n' "$value" >>"$TEST_CALLS"
    exit 97
fi
whole=${BASH_REMATCH[1]}
fraction=${BASH_REMATCH[3]:-}
fraction="${fraction}000"
milliseconds=$((10#$whole * 1000 + 10#${fraction:0:3}))
current=$(<"${TEST_CLOCK:?}")
next=$((current + milliseconds))
printf '%s\n' "$next" >"$TEST_CLOCK"
printf 'CLOCK %s\n' "$next" >>"${TEST_TIMELINE:?}"
if [[ -n ${TEST_TRANSITION_AT_MS:-} && $next -ge TEST_TRANSITION_AT_MS ]]; then
    printf '%s\n' "${TEST_TRANSITION_STATE:?}" >"${TEST_SERVICE_STATE:?}"
fi
SPY

cat >"$tmp_root/bin/date" <<'SPY'
#!/usr/bin/env bash
set -u
case ${1:-} in
    +%s%3N) printf '%s\n' "$(<"${TEST_CLOCK:?}")" ;;
    +%s) printf '%s\n' "$(( $(<"${TEST_CLOCK:?}") / 1000 ))" ;;
    *) printf 'MUTATION unsupported-date' >>"${TEST_CALLS:?}"; printf ' %q' "$@" >>"$TEST_CALLS"; printf '\n' >>"$TEST_CALLS"; exit 97 ;;
esac
SPY

cat >"$tmp_root/bin/journalctl" <<'SPY'
#!/usr/bin/env bash
set -u
printf 'MUTATION unexpected-journalctl' >>"${TEST_CALLS:?}"
printf ' %q' "$@" >>"$TEST_CALLS"
printf '\n' >>"$TEST_CALLS"
exit 97
SPY

cat >"$tmp_root/bin/punto-daemon-fixture" <<'SPY'
#!/usr/bin/env bash
exit 0
SPY
cat >"$tmp_root/bin/unowned-sentinel-fixture" <<'SH'
#!/usr/bin/env bash
set -u

pid=$$
printf 'SENTINEL start %s\n' "$pid" >>"${TEST_CALLS:?}"
printf '%s\n' "$pid" >"${TEST_SENTINEL_READY_FILE:?}"
on_signal() {
    if [[ -e ${TEST_HARNESS_CLEANUP_FILE:?} && \
          ! -s ${TEST_HARNESS_CLEANUP_FILE:?} ]]; then
        exit 0
    fi
    count=$(<"${TEST_SENTINEL_SIGNAL_FILE:?}")
    printf '%s\n' "$((count + 1))" >"$TEST_SENTINEL_SIGNAL_FILE"
    printf 'SENTINEL signal %s\n' "$pid" >>"${TEST_CALLS:?}"
    exit 0
}
trap on_signal TERM INT HUP
while :; do
    read -r -t 30 _ <>"${TEST_TRAY_WAIT_FIFO:?}" || true
done
SH
cat >"$tmp_root/bin/punto-tray-fixture" <<'SH'
#!/usr/bin/env bash
set -u

pid=$$
if [[ ${TEST_TRAY_ROLE:-product} == seed ]]; then
    prefix=TRAY_SEED_PROCESS
else
    prefix=TRAY_PROCESS
fi

printf '%s start %s\n' "$prefix" "$pid" >>"${TEST_CALLS:?}"
printf '%s start %s\n' "$prefix" "$pid" >>"${TEST_TIMELINE:?}"
printf '%s\n' "$pid" >"${TEST_TRAY_OWNER_FILE:?}"
printf '%s\n' "$pid" >"${TEST_TRAY_REAL_FILE:?}"
read -r _ _ _ _ process_group session_id _ <"/proc/$$/stat"
printf '%s %s %s\n' "$pid" "$process_group" "$session_id" \
    >"${TEST_TRAY_SESSION_FILE:?}"
if [[ ${TEST_TRAY_MODE:-ok} == fail ]]; then
    printf 'failed\n' >"${TEST_TRAY_READY_FILE:?}"
    exit 8
fi
printf 'active\n' >"${TEST_TRAY_STATE:?}"
printf 'ready\n' >"${TEST_TRAY_READY_FILE:?}"

stop() {
    if [[ -e ${TEST_HARNESS_CLEANUP_FILE:?} && \
          ! -s ${TEST_HARNESS_CLEANUP_FILE:?} ]]; then
        exit 0
    fi
    printf '%s signal-stop %s\n' "$prefix" "$pid" >>"${TEST_CALLS:?}"
    printf '%s stop %s\n' "$prefix" "$pid" >>"${TEST_CALLS:?}"
    printf '%s stop %s\n' "$prefix" "$pid" >>"${TEST_TIMELINE:?}"
    printf 'inactive\n' >"${TEST_TRAY_STATE:?}"
    exit 0
}
trap stop TERM INT
while :; do
    read -r -t 30 _ <>"${TEST_TRAY_WAIT_FIFO:?}" || true
done
SH
chmod 0755 "$tmp_root/bin/"*

cat >"$tmp_root/bash-env" <<'ENV'
kill() {
    local signal=TERM target pid owner state real attempt running spawned=0
    case ${1:-} in
        -0|-TERM|-SIGTERM|-KILL|-SIGKILL) signal=${1#-}; shift ;;
        -s|--signal) signal=${2:-}; shift 2 ;;
        --signal=*) signal=${1#*=}; shift ;;
    esac
    [[ ${1:-} == -- ]] && shift
    if [[ $# -ne 1 || ! ${1:-} =~ ^-?[1-9][0-9]*$ ]]; then
        printf 'FORBIDDEN raw-pid-kill invalid-argv' >>"${TEST_CALLS:?}"
        printf ' %q' "$@" >>"$TEST_CALLS"
        printf '\n' >>"$TEST_CALLS"
        return 97
    fi
    target=$1
    pid=${target#-}
    for running in $(jobs -pr); do
        if [[ $running == "$pid" ]]; then
            spawned=1
            break
        fi
    done
    owner=$(<"${TEST_TRAY_OWNER_FILE:?}")
    state=$(<"${TEST_TRAY_STATE:?}")
    real=$(<"${TEST_TRAY_REAL_FILE:?}")
    # A completed but unreaped $! is absent from jobs -pr, but its PID cannot
    # be reused. Preserve that exact launch provenance for the cleanup call.
    if [[ $spawned == 0 && $target != -* && $owner == "$pid" && \
          $real == "$pid" && $state == inactive ]]; then
        spawned=1
    fi
    if [[ $spawned == 1 ]]; then
        case $signal in
            0|TERM|SIGTERM|KILL|SIGKILL)
                [[ $signal == SIGTERM ]] && signal=TERM
                [[ $signal == SIGKILL ]] && signal=KILL
                printf 'SHELL_KILL spawned signal=%s pid=%s group=%s\n' \
                    "$signal" "$pid" "$([[ $target == -* ]] && printf 1 || printf 0)" \
                    >>"$TEST_CALLS"
                builtin kill "-${signal}" -- "$target"
                return
                ;;
        esac
    fi
    if [[ $target == -* || -z $owner || $pid != "$owner" || $state != active ]]; then
        printf 'FORBIDDEN raw-pid-kill unowned signal=%q pid=%q owner=%q state=%q\n' \
            "$signal" "$pid" "$owner" "$state" >>"${TEST_CALLS:?}"
        return 97
    fi
    case $signal in
        0)
            printf 'SHELL_KILL owned signal=0 pid=%s\n' "$pid" >>"$TEST_CALLS"
            [[ $state == active ]]
            ;;
        TERM|SIGTERM)
            printf 'SHELL_KILL owned signal=TERM pid=%s\n' "$pid" >>"$TEST_CALLS"
            if [[ $real == "$pid" ]]; then
                builtin kill -TERM "$pid" || return
                for ((attempt = 0; attempt < 200; ++attempt)); do
                    [[ $(<"$TEST_TRAY_STATE") == inactive ]] && break
                    read -r -t 0.002 _ <>"${TEST_TRAY_WAIT_FIFO:?}" || true
                done
                [[ $(<"$TEST_TRAY_STATE") == inactive ]] || return 96
            else
                printf 'TRAY_PROCESS stop %s\n' "$pid" >>"$TEST_CALLS"
                printf 'TRAY_PROCESS stop %s\n' "$pid" >>"${TEST_TIMELINE:?}"
                printf 'inactive\n' >"$TEST_TRAY_STATE"
            fi
            ;;
        *)
            printf 'FORBIDDEN raw-pid-kill forbidden-signal=%q pid=%q\n' \
                "$signal" "$pid" >>"$TEST_CALLS"
            return 97
            ;;
    esac
}
ENV
chmod 0644 "$tmp_root/bash-env"

set_response_line() {
    printf '%s\n' "$1" >"$tmp_root/response.bin"
}

set_response_no_lf() {
    printf '%s' "$1" >"$tmp_root/response.bin"
}

set_response_with_invalid_byte() {
    local hex_byte=$1 marker=${2:-x11_health}
    /usr/bin/python3 - "$tmp_root/response.bin" "$VALID_STATS" "$hex_byte" "$marker" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
payload = sys.argv[2].encode("ascii")
invalid = bytes.fromhex(sys.argv[3])
marker = sys.argv[4].encode("ascii")
if marker not in payload:
    raise SystemExit("marker missing from canonical STATS fixture")
path.write_bytes(payload.replace(marker, marker + invalid, 1) + b"\n")
PY
}

start_fixture() {
    local mode=${1:-normal} ready rc
    stop_fixture
    : >"$tmp_root/requests.log"
    : >"$tmp_root/fixture.out"
    : >"$tmp_root/fixture.ready"
    chmod 0666 "$tmp_root/requests.log" "$tmp_root/fixture.out" "$tmp_root/fixture.ready"
    rm -f -- "$tmp_root/run/punto.sock"
    rm -f -- "$tmp_root/ready.fifo"
    mkfifo "$tmp_root/ready.fifo"
    /usr/bin/python3 "$tmp_root/ipc_fixture.py" \
        "$tmp_root/run/punto.sock" "$mode" "$tmp_root/response.bin" \
        "$tmp_root/requests.log" "$tmp_root/timeline.log" "$tmp_root/ready.fifo" \
        >"$tmp_root/fixture.out" 2>&1 &
    fixture_pid=$!
    set +e
    ready=$(timeout --signal=KILL 2s /usr/bin/head -n 1 "$tmp_root/ready.fifo")
    rc=$?
    set +e
    rm -f -- "$tmp_root/ready.fifo"
    if [[ $rc -eq 0 && $ready == READY ]]; then
        return 0
    fi
    fail "AF_UNIX fixture mode $mode reached its ready barrier (rc=$rc)"
    return 1
}

verify_fixture_permissions() {
    local mode rc output output_file="$tmp_root/fixture-probe.out" expected_hex
    expected_hex=$(/usr/bin/python3 - "$VALID_STATS" <<'PY'
import sys
print((sys.argv[1] + "\n").encode("ascii").hex())
PY
)
    for mode in normal denied; do
        reset_case
        start_fixture "$mode" || continue
        set +e
        timeout --signal=KILL 2s bwrap \
            --ro-bind / / --bind "$tmp_root" "$tmp_root" --tmpfs /run \
            --proc /proc --dev /dev --unshare-net --unshare-pid --unshare-user \
            --uid 65534 --gid 65534 --die-with-parent --new-session --clearenv \
            --setenv LC_ALL C.UTF-8 \
            /usr/bin/python3 "$tmp_root/fixture_probe.py" "$tmp_root/run/punto.sock" \
            >"$output_file" 2>&1
        rc=$?
        set +e
        output=$(<"$output_file")
        assert_contains "$output" 'EUID=65534' "AF_UNIX $mode fixture probe is privilege-dropped"
        if [[ $mode == normal ]]; then
            assert_zero "$rc" "AF_UNIX normal fixture accepts the privilege-dropped probe"
            assert_contains "$output" "RESPONSE_HEX=$expected_hex" \
                "AF_UNIX normal fixture returns the byte-exact canonical payload"
            assert_exact_stats_requests "AF_UNIX normal fixture probe" 1 0
        else
            assert_nonzero "$rc" "AF_UNIX denied fixture rejects the privilege-dropped probe"
            assert_contains "$output" 'PermissionError' \
                "AF_UNIX denied fixture fails specifically with a permission error"
            assert_no_requests "AF_UNIX denied fixture probe"
        fi
        assert_bounded "$rc" 0 1 "AF_UNIX $mode fixture probe avoids harness timeout"
    done
    reset_case
}

reset_case() {
    stop_fixture
    : >"$tmp_root/calls.log"
    : >"$tmp_root/requests.log"
    : >"$tmp_root/timeline.log"
    : >"$tmp_root/euid.log"
    : >"$tmp_root/cli.session"
    printf 'inactive\n' >"$tmp_root/service.state"
    printf 'inactive\n' >"$tmp_root/tray.state"
    : >"$tmp_root/tray.pid"
    : >"$tmp_root/tray.owner"
    : >"$tmp_root/tray.real"
    : >"$tmp_root/tray.ready"
    : >"$tmp_root/tray.session"
    : >"$tmp_root/sentinel.ready"
    printf '0\n' >"$tmp_root/sentinel.signals"
    : >"$tmp_root/hang.pid"
    rm -f -- "$tmp_root/harness.cleanup"
    printf '0\n' >"$tmp_root/clock.ms"
    set_response_line "$VALID_STATS"
    SYSTEMCTL_MODE=ok
    TRAY_MODE=ok
    TRAY_PATH="$tmp_root/bin/punto-tray-fixture"
    IPC_TIMEOUT_MS=50
    COMMAND_TIMEOUT_MS=120
    START_TIMEOUT_MS=40
    STOP_TIMEOUT_MS=40
    POLL_INTERVAL_MS=10
    TRANSITION_AT_MS=""
    TRANSITION_STATE=""
    SEED_TRAY=0
    TRAY_PID_OVERRIDE=""
}

seed_active_tray() {
    printf 'active\n' >"$tmp_root/tray.state"
}

clear_observations() {
    : >"$tmp_root/calls.log"
    : >"$tmp_root/requests.log"
    : >"$tmp_root/timeline.log"
    : >"$tmp_root/euid.log"
}

run_cli() {
    local name=$1
    shift
    local output_file="$tmp_root/${name}.out" start_ms end_ms trace_file trace_dir
    CLI_RUN_SERIAL=$((CLI_RUN_SERIAL + 1))
    printf -v trace_dir '%s/execve/%04d-%s' \
        "$tmp_root" "$CLI_RUN_SERIAL" "${name//[^a-zA-Z0-9_.-]/_}"
    mkdir -p "$trace_dir"
    chmod 0777 "$trace_dir"
    CLI_LAST_TRACE_DIR=$trace_dir
    trace_file="$trace_dir/trace"
    start_ms=$(/usr/bin/date +%s%3N)
    set +e
    timeout --signal=KILL 3s bwrap \
        --ro-bind / / --ro-bind "$REPO_ROOT" "$REPO_ROOT" --bind "$tmp_root" "$tmp_root" \
        --ro-bind "$tmp_root/bin/sudo" /usr/bin/sudo \
        --ro-bind "$tmp_root/bin/systemctl" /usr/bin/systemctl \
        --ro-bind "$tmp_root/bin/nc" /usr/bin/nc.openbsd \
        --ro-bind "$tmp_root/bin/pgrep" /usr/bin/pgrep \
        --ro-bind "$tmp_root/bin/pkill" /usr/bin/pkill \
        --ro-bind "$tmp_root/bin/pidof" /usr/bin/pidof \
        --ro-bind "$tmp_root/bin/killall" /usr/bin/killall \
        --ro-bind "$tmp_root/bin/ps" /usr/bin/ps \
        --ro-bind "$tmp_root/bin/kill" /usr/bin/kill \
        --ro-bind "$tmp_root/bin/nohup" /usr/bin/nohup \
        --ro-bind "$tmp_root/bin/setsid" /usr/bin/setsid \
        --ro-bind "$tmp_root/bin/sg" /usr/bin/sg \
        --ro-bind "$tmp_root/bin/newgrp" /usr/bin/newgrp \
        --ro-bind "$tmp_root/bin/sleep" /usr/bin/sleep \
        --ro-bind "$tmp_root/bin/date" /usr/bin/date \
        --ro-bind "$tmp_root/bin/journalctl" /usr/bin/journalctl \
        --tmpfs /run --proc /proc --dev /dev --unshare-net --unshare-pid \
        --unshare-user --uid 65534 --gid 65534 --die-with-parent --new-session --clearenv \
        --setenv PATH "$tmp_root/bin:/usr/bin:/bin" --setenv HOME "$tmp_root/home" \
        --setenv TMPDIR "$tmp_root" --setenv LC_ALL C.UTF-8 --setenv DISPLAY :99999 \
        --setenv BASH_ENV "$tmp_root/bash-env" \
        --setenv USER nobody --setenv LOGNAME nobody --setenv TEST_CALLS "$tmp_root/calls.log" \
        --setenv TEST_TIMELINE "$tmp_root/timeline.log" --setenv TEST_SERVICE "$SERVICE" \
        --setenv TEST_NC_REAL "$tmp_root/bin/nc-real" \
        --setenv TEST_SERVICE_STATE "$tmp_root/service.state" \
        --setenv TEST_SYSTEMCTL_MODE "$SYSTEMCTL_MODE" --setenv TEST_TRAY_MODE "$TRAY_MODE" \
        --setenv TEST_TRAY_STATE "$tmp_root/tray.state" --setenv TEST_CLOCK "$tmp_root/clock.ms" \
        --setenv TEST_TRAY_OWNER_FILE "$tmp_root/tray.owner" \
        --setenv TEST_TRAY_REAL_FILE "$tmp_root/tray.real" \
        --setenv TEST_TRAY_READY_FILE "$tmp_root/tray.ready" \
        --setenv TEST_TRAY_SESSION_FILE "$tmp_root/tray.session" \
        --setenv TEST_TRAY_WAIT_FIFO "$tmp_root/tray.wait" \
        --setenv TEST_HARNESS_CLEANUP_FILE "$tmp_root/harness.cleanup" \
        --setenv TEST_TRAY_FIXTURE "$tmp_root/bin/punto-tray-fixture" \
        --setenv TEST_SENTINEL_FIXTURE "$tmp_root/bin/unowned-sentinel-fixture" \
        --setenv TEST_SENTINEL_READY_FILE "$tmp_root/sentinel.ready" \
        --setenv TEST_SENTINEL_SIGNAL_FILE "$tmp_root/sentinel.signals" \
        --setenv TEST_HANG_PID_FILE "$tmp_root/hang.pid" \
        --setenv TEST_SEED_TRAY "$SEED_TRAY" \
        --setenv TEST_TRAY_PID_OVERRIDE "$TRAY_PID_OVERRIDE" \
        --setenv TEST_TRANSITION_AT_MS "$TRANSITION_AT_MS" \
        --setenv TEST_TRANSITION_STATE "$TRANSITION_STATE" \
        --setenv TEST_EUID_FILE "$tmp_root/euid.log" \
        --setenv TEST_EXEC_TRACE "$trace_file" \
        --setenv TEST_CLI_RC_FILE "$trace_dir/cli.rc" \
        --setenv TEST_CLI_SESSION_FILE "$tmp_root/cli.session" \
        --setenv TEST_TRACE_WRAPPER "$tmp_root/trace-wrapper" \
        --setenv TEST_NOHUP_REAL "$tmp_root/bin/nohup-real" \
        --setenv TEST_SETSID_REAL "$tmp_root/bin/setsid-real" \
        --setenv PUNTO_UDEVMON_SERVICE "$SERVICE" \
        --setenv PUNTO_DAEMON "$tmp_root/bin/punto-daemon-fixture" \
        --setenv PUNTO_TRAY "$TRAY_PATH" --setenv PUNTO_SOCKET "$tmp_root/run/punto.sock" \
        --setenv PUNTO_TRAY_PID_FILE "$tmp_root/tray.pid" \
        --setenv PUNTO_VERSION_FILE "$tmp_root/VERSION" \
        --setenv PUNTO_IPC_TIMEOUT_MS "$IPC_TIMEOUT_MS" \
        --setenv PUNTO_COMMAND_TIMEOUT_MS "$COMMAND_TIMEOUT_MS" \
        --setenv PUNTO_START_TIMEOUT_MS "$START_TIMEOUT_MS" \
        --setenv PUNTO_STOP_TIMEOUT_MS "$STOP_TIMEOUT_MS" \
        --setenv PUNTO_POLL_INTERVAL_MS "$POLL_INTERVAL_MS" \
        --setenv PUNTO_NONINTERACTIVE 1 \
        /bin/bash -c "$INNER_RUNNER" \
        _ "$CLI_SCRIPT" "$@" >"$output_file" 2>&1
    CLI_RC=$?
    set +e
    end_ms=$(/usr/bin/date +%s%3N)
    CLI_DURATION_MS=$((end_ms - start_ms))
    CLI_OUTPUT_FILE=$output_file
    CLI_OUTPUT=$(<"$output_file")
}

assert_privilege_dropped() {
    local message=$1
    if [[ $(<"$tmp_root/euid.log") == 65534 ]]; then
        pass "$message runs as euid 65534"
    else
        fail "$message did not run privilege-dropped (euid=$(<"$tmp_root/euid.log"))"
    fi
}

assert_exact_stats_requests() {
    local message=$1 expected_count=${2:-1}
    local expected_nc=${3:-$expected_count}
    local requests actual_count
    requests=$(<"$tmp_root/requests.log")
    actual_count=$(grep -c '^' "$tmp_root/requests.log" || true)
    if [[ $actual_count -eq $expected_count ]] && \
       ! grep -Ev '^53544154530a$' <<<"$requests" | grep -q .; then
        pass "$message sends exactly $expected_count request(s), each byte-exact STATS\\n"
    else
        fail "$message requests were ${requests:-<none>} (count=$actual_count, expected=$expected_count)"
    fi
    assert_nc_count "$expected_nc" "$message"
}

assert_no_requests() {
    local message=$1
    if [[ ! -s $tmp_root/requests.log ]]; then
        pass "$message sends no IPC request"
    else
        fail "$message unexpectedly sent $(<"$tmp_root/requests.log")"
    fi
}

assert_no_pid_or_undeclared_calls() {
    local message=$1 calls invalid_owned
    calls=$(<"$tmp_root/calls.log")
    assert_not_contains "$calls" 'FORBIDDEN ' "$message uses no PID/process heuristic"
    assert_not_contains "$calls" 'MUTATION ' "$message invokes no undeclared command"
    if grep -Eiq -- 'SIGKILL|(^|[[:space:]])-9([[:space:]]|$)|--signal(=|[[:space:]]+)9([[:space:]]|$)|(^|[[:space:]])-s(=|[[:space:]]*)9([[:space:]]|$)|SIGKILL forbidden' <<<"$calls"; then
        fail "$message requested an unvalidated SIGKILL path: ${calls//$'\n'/; }"
    else
        pass "$message uses only bounded timeout or owned-child escalation"
    fi
    invalid_owned=$(grep -E '^SHELL_KILL([[:space:]]|$)' "$tmp_root/calls.log" | \
        grep -Ev '^SHELL_KILL (owned signal=(0|TERM) pid=[1-9][0-9]*|spawned signal=(0|TERM|KILL) pid=[1-9][0-9]* group=[01])$' || true)
    if [[ -z $invalid_owned ]]; then
        pass "$message permits shell kill only for an owned tray or current child job"
    else
        fail "$message contains an unproven shell kill: ${invalid_owned//$'\n'/; }"
    fi
}

assert_no_control_calls() {
    local message=$1 calls
    calls=$(grep -Ev '^(TIMEOUT|NC|SHELL_KILL)([[:space:]]|$)' "$tmp_root/calls.log" || true)
    if [[ -z $calls ]]; then
        pass "$message invokes no service/process control command"
    else
        fail "$message calls: ${calls//$'\n'/; }"
    fi
}

assert_nc_count() {
    local expected=$1 message=$2 total exact timeout_count duration
    total=$(grep -Ec '^NC([[:space:]]|$)' "$tmp_root/calls.log" || true)
    exact=$(grep -Fxc -- "NC -U $tmp_root/run/punto.sock" "$tmp_root/calls.log" || true)
    if [[ $total -eq $expected && $exact -eq $expected ]]; then
        pass "$message invokes nc -U with the exact socket $expected time(s)"
    else
        fail "$message nc calls are total=$total exact=$exact, expected=$expected"
    fi
    if [[ $expected -eq 0 ]]; then
        timeout_count=$(grep -Ec '^TIMEOUT .*nc([[:space:]]|$)' "$tmp_root/calls.log" || true)
        if [[ $timeout_count -eq 0 ]]; then
            pass "$message has no stray nc timeout wrapper"
        else
            fail "$message has $timeout_count stray nc timeout wrapper(s)"
        fi
        return
    fi
    printf -v duration '%d.%03ds' "$((IPC_TIMEOUT_MS / 1000))" "$((IPC_TIMEOUT_MS % 1000))"
    timeout_count=$(grep -Fxc -- \
        "TIMEOUT --signal=TERM --kill-after=0.200s $duration nc -U $tmp_root/run/punto.sock" \
        "$tmp_root/calls.log" || true)
    if [[ $timeout_count -eq $expected ]]; then
        pass "$message applies the exact bounded $duration timeout to every nc request"
    else
        fail "$message exact nc timeout wrappers=$timeout_count, expected=$expected"
    fi
}

assert_call_count() {
    local expected_line=$1 expected_count=$2 message=$3 actual
    actual=$(grep -Fxc -- "$expected_line" "$tmp_root/calls.log" || true)
    if [[ $actual -eq $expected_count ]]; then
        pass "$message"
    else
        fail "$message (expected $expected_count, got $actual for '$expected_line')"
    fi
}

assert_tray_start_count() {
    local expected=$1 message=$2 actual
    actual=$(grep -Ec '^SYSTEMCTL --user (start|restart) -- punto-tray[.]service$' \
        "$tmp_root/calls.log" || true)
    if [[ $actual -eq $expected ]]; then
        pass "$message starts exactly $expected tray process(es)"
    else
        fail "$message starts $actual tray process(es), expected $expected"
    fi
}

assert_tray_stop_count() {
    local expected=$1 message=$2 actual
    actual=$(grep -Ec '^SYSTEMCTL --user (stop|restart) -- punto-tray[.]service$' \
        "$tmp_root/calls.log" || true)
    if [[ $actual -eq $expected ]]; then
        pass "$message stops exactly $expected owned tray process(es)"
    else
        fail "$message stops $actual owned tray process(es), expected $expected"
    fi
}

assert_tray_state() {
    local expected=$1 message=$2 actual
    actual=$(<"$tmp_root/tray.state")
    if [[ $actual == "$expected" ]]; then
        pass "$message tray state is $expected"
    else
        fail "$message tray state is '$actual', expected '$expected'"
    fi
    if ! grep -Eq '(^|[[:space:]])PUNTO_TRAY_PID_FILE=' "$CLI_SCRIPT"; then
        pass "$message has no product PID-file lifecycle"
    else
        fail "$message unexpectedly retains a product PID-file lifecycle"
    fi
}

assert_launcher_hup_isolation() {
    local message=$1 launchers
    launchers=$(grep -E '^LAUNCHER (nohup|setsid)([[:space:]]|$)' \
        "$tmp_root/calls.log" | grep -F -- " $TRAY_PATH" || true)
    if [[ -z $launchers ]]; then
        pass "$message delegates tray process and signal ownership to systemd --user"
    else
        fail "$message unexpectedly launches tray directly: ${launchers//$'\n'/; }"
    fi
}

assert_seed_liveness() {
    local expected=$1 message=$2 state
    state=$(<"$tmp_root/tray.state")
    if [[ ($expected == alive && $state == active) ||
          ($expected == stopped && $state == inactive) ]]; then
        pass "$message systemd-owned tray is observably $expected"
    else
        fail "$message systemd-owned tray state is '$state', expected $expected"
    fi
}

assert_unowned_sentinel_untouched() {
    local message=$1 observed sentinel attempted syscall_rc
    observed=$(grep -E '^SENTINEL_OBSERVER (alive|stopped) [1-9][0-9]* signals=[0-9]+$' \
        "$tmp_root/calls.log" || true)
    if [[ $observed =~ ^SENTINEL_OBSERVER[[:space:]]alive[[:space:]][1-9][0-9]*[[:space:]]signals=0$ ]] && \
       ! grep -q '^SENTINEL signal ' "$tmp_root/calls.log"; then
        pass "$message leaves the independent unowned PID alive and unsignalled"
    else
        fail "$message touched the unowned PID sentinel (observed: ${observed//$'\n'/; })"
    fi
    sentinel=$(<"$tmp_root/sentinel.ready")
    set +e
    attempted=$(/usr/bin/python3 - "$CLI_LAST_TRACE_DIR" "$sentinel" <<'PY'
import pathlib
import re
import sys

trace_root = pathlib.Path(sys.argv[1])
pid = re.escape(sys.argv[2])
patterns = (
    re.compile(rf"^(?:kill|tkill)\({pid},"),
    re.compile(rf"^tgkill\([^,]+, {pid},"),
    re.compile(r"^pidfd_send_signal\("),
)
files = sorted(trace_root.glob("trace.*"))
if not files:
    raise SystemExit("no syscall trace")
for path in files:
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if any(pattern.search(line) for pattern in patterns):
            print(f"{path.name}: {line}")
PY
    )
    syscall_rc=$?
    set +e
    if [[ $syscall_rc -ne 0 ]]; then
        fail "$message signal-syscall parser failed closed (rc=$syscall_rc)"
    elif [[ -z $attempted ]]; then
        pass "$message makes no kill-family syscall for the unowned PID, including signal 0"
    else
        fail "$message attempted a kill-family syscall for the unowned PID: ${attempted//$'\n'/; }"
    fi
}

assert_service_mutation_sequence() {
    local message=$1 action command expected_sudo="" expected_systemctl=""
    local actual_sudo actual_systemctl
    shift
    for action in "$@"; do
        case $action in
            start|stop|restart) command="$action $SERVICE" ;;
            term) command="kill --kill-who=all --signal=TERM $SERVICE" ;;
            *) fail "$message test requested unknown service action '$action'"; return ;;
        esac
        expected_sudo+="SUDO -n systemctl $command"$'\n'
        expected_systemctl+="SYSTEMCTL $command"$'\n'
    done
    expected_sudo=${expected_sudo%$'\n'}
    expected_systemctl=${expected_systemctl%$'\n'}
    actual_sudo=$(grep -E '^SUDO([[:space:]]|$)' "$tmp_root/calls.log" || true)
    actual_systemctl=$(grep -E '^SYSTEMCTL (start|stop|restart|kill)([[:space:]]|$)' \
        "$tmp_root/calls.log" || true)
    if [[ $actual_sudo == "$expected_sudo" && $actual_systemctl == "$expected_systemctl" ]]; then
        pass "$message has the exact ordered service mutation sequence"
    else
        fail "$message service sequence differs (sudo='${actual_sudo//$'\n'/; }', systemctl='${actual_systemctl//$'\n'/; }')"
    fi
}

assert_timeline_before() {
    local first=$1 second=$2 message=$3 first_line second_line
    first_line=$(grep -n -m1 -F -- "$first" "$tmp_root/timeline.log" | cut -d: -f1 || true)
    second_line=$(grep -n -m1 -F -- "$second" "$tmp_root/timeline.log" | cut -d: -f1 || true)
    if [[ -n $first_line && -n $second_line && $first_line -lt $second_line ]]; then
        pass "$message"
    else
        fail "$message (timeline: $(tr '\n' ';' <"$tmp_root/timeline.log"))"
    fi
}

assert_service_state() {
    local expected=$1 message=$2 actual
    actual=$(<"$tmp_root/service.state")
    if [[ $actual == "$expected" ]]; then
        pass "$message final state is $expected"
    else
        fail "$message final state is '$actual', expected '$expected'"
    fi
}

assert_status_success() {
    local expected=$1 message=$2 plain
    assert_zero "$CLI_RC" "$message succeeds"
    plain=$(plain_output <<<"$CLI_OUTPUT")
    if [[ $plain == "$expected" ]] && cli_output_is_exact "$expected"; then
        pass "$message renders only the exact one-line STATS response"
    else
        fail "$message output is not byte-exact STATS (output: ${plain//$'\n'/; })"
    fi
    if grep -Eq '^ERROR([[:space:]]|$)' <<<"$plain"; then
        fail "$message mixed its STATS success with an ERROR response"
    else
        pass "$message reports no error category"
    fi
    assert_exact_stats_requests "$message"
    assert_no_control_calls "$message"
    assert_no_pid_or_undeclared_calls "$message"
}

run_protocol_rejection() {
    local label=$1
    start_fixture normal || return
    run_cli "protocol-${label//[^a-zA-Z0-9]/_}" status
    assert_nonzero "$CLI_RC" "B11 grammar: $label"
    assert_error_category protocol-error "B11 grammar: $label"
    assert_exact_stats_requests "B11 grammar: $label"
    assert_no_control_calls "B11 grammar: $label"
    assert_no_pid_or_undeclared_calls "B11 grammar: $label"
    assert_bounded "$CLI_RC" "$CLI_DURATION_MS" 800 "B11 grammar: $label is bounded"
}

run_stats_grammar_matrix() {
    local field value payload label token invalid_numeric index cursor
    local -a tokens=(
        x11_health=ready analysis_health=degraded input_health=failed
        x11_last_progress_ms=0 analysis_last_progress_ms=18446744073709551615
        input_last_progress_ms=30 analysis_outstanding=2 input_in_flight=1
        log_dropped=12 text_mutation=disabled enabled=0 configured_enabled=1
        config_pending=0 config_generation=13 config_result=ok analyzed=0
        need_switch=1 corrections=2 pending_words=3
        ready_results=4 worker_threads=5 daemon_peers=6 analysis_mode=fixed
        control_plane=primary queued_tasks=7 avg_queue_us=8 avg_analysis_us=9
        avg_macro_us=10 avg_tail_len=11
    )
    local -a numeric_tokens=(
        x11_last_progress_ms=0 analysis_last_progress_ms=18446744073709551615
        input_last_progress_ms=30 analysis_outstanding=2 input_in_flight=1
        log_dropped=12 configured_enabled=1 config_pending=0 config_generation=13
        analyzed=0 need_switch=1 corrections=2 pending_words=3
        ready_results=4 worker_threads=5 daemon_peers=6 queued_tasks=7
        avg_queue_us=8 avg_analysis_us=9 avg_macro_us=10 avg_tail_len=11
    )
    local -a boolean_tokens=(
        input_in_flight=1 configured_enabled=1 config_pending=0
    )

    reset_case
    start_fixture normal || return
    run_cli status-canonical status
    assert_status_success "$VALID_STATS" "B11 canonical status"
    assert_privilege_dropped "B11 canonical status"

    for field in x11_health analysis_health input_health; do
        for value in ready degraded failed; do
            case $field in
                x11_health) payload=${VALID_STATS/x11_health=ready/x11_health=$value} ;;
                analysis_health) payload=${VALID_STATS/analysis_health=degraded/analysis_health=$value} ;;
                input_health) payload=${VALID_STATS/input_health=failed/input_health=$value} ;;
            esac
            reset_case
            set_response_line "$payload"
            start_fixture normal || continue
            run_cli "valid-${field}-${value}" status
            assert_status_success "$payload" "B11 valid $field=$value"
        done
    done

    for token in input_in_flight=0 configured_enabled=0 config_pending=1 config_result=none config_result=error analysis_mode=auto control_plane=secondary; do
        case $token in
            input_in_flight=0) payload=${VALID_STATS/input_in_flight=1/$token} ;;
            configured_enabled=0) payload=${VALID_STATS/configured_enabled=1/$token} ;;
            config_pending=1) payload=${VALID_STATS/config_pending=0/$token} ;;
            config_result=none) payload=${VALID_STATS/config_result=ok/$token} ;;
            config_result=error) payload=${VALID_STATS/config_result=ok/$token} ;;
            analysis_mode=auto) payload=${VALID_STATS/analysis_mode=fixed/$token} ;;
            control_plane=secondary) payload=${VALID_STATS/control_plane=primary/$token} ;;
        esac
        reset_case
        set_response_line "$payload"
        start_fixture normal || continue
        run_cli "valid-${token//=/_}" status
        assert_status_success "$payload" "B11 valid $token"
    done

    for token in "${numeric_tokens[@]}"; do
        field=${token%%=*}
        if [[ " ${boolean_tokens[*]} " == *" $token "* ]]; then
            for value in 0 1; do
                payload=${VALID_STATS/"$token"/"$field=$value"}
                reset_case
                set_response_line "$payload"
                start_fixture normal || continue
                run_cli "valid-boolean-${field}-${value}" status
                assert_status_success "$payload" "B11 valid boolean $field=$value"
            done
        else
            for value in 0 18446744073709551615; do
                payload=${VALID_STATS/"$token"/"$field=$value"}
                reset_case
                set_response_line "$payload"
                start_fixture normal || continue
                run_cli "valid-boundary-${field}-${value}" status
                assert_status_success "$payload" "B11 valid uint64 boundary $field=$value"
            done
        fi
    done

    for field in x11_health analysis_health input_health; do
        case $field in
            x11_health) payload=${VALID_STATS/x11_health=ready/x11_health=unknown} ;;
            analysis_health) payload=${VALID_STATS/analysis_health=degraded/analysis_health=unknown} ;;
            input_health) payload=${VALID_STATS/input_health=failed/input_health=unknown} ;;
        esac
        reset_case
        set_response_line "$payload"
        run_protocol_rejection "invalid-enum-$field"
    done

    for token in "${numeric_tokens[@]}"; do
        field=${token%%=*}
        for invalid_numeric in not-a-number -1 18446744073709551616; do
            payload=${VALID_STATS/"$token"/"$field=$invalid_numeric"}
            reset_case
            set_response_line "$payload"
            run_protocol_rejection \
                "invalid-numeric-$field-${invalid_numeric//[^a-zA-Z0-9]/_}"
        done
    done


    for token in "${boolean_tokens[@]}"; do
        field=${token%%=*}
        payload=${VALID_STATS/"$token"/"$field=2"}
        reset_case
        set_response_line "$payload"
        run_protocol_rejection "invalid-boolean-$field"
    done

    for token in "${tokens[@]}"; do
        field=${token%%=*}
        if [[ $VALID_STATS == "OK $token "* ]]; then
            payload=${VALID_STATS/"$token "/}
        else
            payload=${VALID_STATS/" $token"/}
        fi
        reset_case
        set_response_line "$payload"
        run_protocol_rejection "missing-$field"
    done

    for ((index = 0; index < ${#tokens[@]} - 1; ++index)); do
        payload=OK
        for ((cursor = 0; cursor < ${#tokens[@]}; ++cursor)); do
            if [[ $cursor -eq $index ]]; then
                payload+=" ${tokens[$((index + 1))]}"
            elif [[ $cursor -eq $((index + 1)) ]]; then
                payload+=" ${tokens[$index]}"
            else
                payload+=" ${tokens[$cursor]}"
            fi
        done
        reset_case
        set_response_line "$payload"
        run_protocol_rejection "reordered-${tokens[$index]%%=*}-${tokens[$((index + 1))]%%=*}"
    done

    for token in "${tokens[@]}"; do
        field=${token%%=*}
        reset_case
        set_response_line "$VALID_STATS $token"
        run_protocol_rejection "duplicate-$field"
    done

    local -a invalid_payloads=(
        'OK analysis_health=degraded x11_health=ready input_health=failed x11_last_progress_ms=0 analysis_last_progress_ms=1 input_last_progress_ms=2 analysis_outstanding=0 input_in_flight=0 log_dropped=0 text_mutation=disabled enabled=0 configured_enabled=1 config_pending=0 config_generation=1 config_result=ok analyzed=0 need_switch=0 corrections=0 pending_words=0 ready_results=0 worker_threads=1 daemon_peers=1 analysis_mode=auto control_plane=primary queued_tasks=0 avg_queue_us=0 avg_analysis_us=0 avg_macro_us=0 avg_tail_len=0'
        "$VALID_STATS extra=1"
        "$VALID_STATS x11_health=ready"
        "${VALID_STATS/x11_health=ready/x11_health=READY}"
        "${VALID_STATS/analysis_health=degraded/analysis_health=unknown}"
        "${VALID_STATS/input_health=failed/input_health=}"
        "${VALID_STATS/x11_last_progress_ms=0/x11_last_progress_ms=-1}"
        "${VALID_STATS/input_last_progress_ms=30/input_last_progress_ms=+30}"
        "${VALID_STATS/analysis_last_progress_ms=18446744073709551615/analysis_last_progress_ms=18446744073709551616}"
        "${VALID_STATS/analysis_outstanding=2/analysis_outstanding=2.0}"
        "${VALID_STATS/input_in_flight=1/input_in_flight=2}"
        "${VALID_STATS/enabled=0/enabled=true}"
        "${VALID_STATS/text_mutation=disabled/text_mutation=enabled}"
        "${VALID_STATS/configured_enabled=1/configured_enabled=true}"
        "${VALID_STATS/config_pending=0/config_pending=true}"
        "${VALID_STATS/config_result=ok/config_result=success}"
        "${VALID_STATS/analysis_mode=fixed/analysis_mode=dynamic}"
        "${VALID_STATS/control_plane=primary/control_plane=leader}"
        "${VALID_STATS/ worker_threads=5/  worker_threads=5}"
        " $VALID_STATS"
        "$VALID_STATS "
        "${VALID_STATS/OK /SUCCESS }"
    )
    local -a invalid_labels=(
        reordered extra-key duplicate-key uppercase-health unknown-health empty-value
        negative-integer signed-integer integer-overflow decimal-count invalid-in-flight
        invalid-enabled invalid-text-mutation invalid-configured-enabled invalid-config-pending
        invalid-config-result
        invalid-analysis-mode invalid-control-plane double-space leading-space trailing-space
        wrong-prefix
    )
    for ((value = 0; value < ${#invalid_payloads[@]}; ++value)); do
        reset_case
        set_response_line "${invalid_payloads[$value]}"
        run_protocol_rejection "${invalid_labels[$value]}"
    done

    reset_case
    printf '%s\r\n' "$VALID_STATS" >"$tmp_root/response.bin"
    run_protocol_rejection crlf

    reset_case
    printf '%s\n%s\n' "$VALID_STATS" "$VALID_STATS" >"$tmp_root/response.bin"
    run_protocol_rejection multiple-lines

    reset_case
    set_response_no_lf "$VALID_STATS"
    run_protocol_rejection missing-lf

    reset_case
    set_response_with_invalid_byte 00 x11_health
    run_protocol_rejection embedded-nul

    reset_case
    {
        printf '%s padding=' "$VALID_STATS"
        /usr/bin/head -c 4096 /dev/zero | /usr/bin/tr '\0' x
        printf '\n'
    } >"$tmp_root/response.bin"
    run_protocol_rejection oversized-line

    reset_case
    set_response_with_invalid_byte ff x11_health
    run_protocol_rejection non-ascii
}

run_transport_error_matrix() {
    reset_case
    run_cli status-unavailable status
    assert_nonzero "$CLI_RC" "B11 unavailable transport"
    assert_error_category unavailable "B11 unavailable transport"
    assert_no_requests "B11 unavailable transport"
    assert_nc_count 1 "B11 unavailable transport"
    assert_no_control_calls "B11 unavailable transport"
    assert_no_pid_or_undeclared_calls "B11 unavailable transport"
    assert_bounded "$CLI_RC" "$CLI_DURATION_MS" 800 "B11 unavailable transport is bounded"

    reset_case
    start_fixture denied || return
    run_cli status-denied status
    assert_nonzero "$CLI_RC" "B11 denied transport"
    assert_error_category denied "B11 denied transport"
    assert_privilege_dropped "B11 denied transport"
    assert_no_requests "B11 denied transport"
    assert_nc_count 1 "B11 denied transport"
    assert_no_control_calls "B11 denied transport"
    assert_no_pid_or_undeclared_calls "B11 denied transport"
    assert_bounded "$CLI_RC" "$CLI_DURATION_MS" 800 "B11 denied transport is bounded"

    reset_case
    start_fixture stall || return
    run_cli status-timeout status
    assert_nonzero "$CLI_RC" "B11 stalled transport"
    assert_error_category timeout "B11 stalled transport"
    assert_exact_stats_requests "B11 stalled transport"
    assert_no_control_calls "B11 stalled transport"
    assert_no_pid_or_undeclared_calls "B11 stalled transport"
    assert_bounded "$CLI_RC" "$CLI_DURATION_MS" 800 "B11 stalled transport honors IPC timeout"

    reset_case
    set_response_line 'ERROR internal-sentinel'
    start_fixture normal || return
    run_cli status-daemon-error status
    assert_nonzero "$CLI_RC" "B11 daemon ERROR response"
    assert_error_category daemon-error "B11 daemon ERROR response"
    assert_not_contains "$CLI_OUTPUT" 'internal-sentinel' "B11 daemon error detail is not reflected"
    assert_exact_stats_requests "B11 daemon ERROR response"
    assert_no_control_calls "B11 daemon ERROR response"
    assert_no_pid_or_undeclared_calls "B11 daemon ERROR response"
}

assert_start_command() {
    local action=$1 count=${2:-1}
    assert_call_count "SUDO -n systemctl $action $SERVICE" "$count" \
        "B26: sudo argv for $action is exact"
    assert_call_count "SYSTEMCTL $action $SERVICE" "$count" \
        "B26: systemctl argv for $action is exact"
}

prepare_response() {
    case $1 in
        valid) set_response_line "$VALID_STATS" ;;
        protocol) set_response_line 'OK x11_health=ready' ;;
        daemon) set_response_line 'ERROR contract-sentinel' ;;
    esac
}

assert_readiness_observation() {
    local fixture_mode=$1 message=$2
    case $fixture_mode in
        none|denied)
            assert_no_requests "$message"
            assert_nc_count 1 "$message"
            ;;
        *)
            assert_exact_stats_requests "$message"
            ;;
    esac
}

run_start_success_and_idempotency() {
    reset_case
    start_fixture normal || return
    run_cli start-success start
    assert_zero "$CLI_RC" "B26 start success"
    assert_start_command start
    assert_service_mutation_sequence "B26 start success" start
    assert_exact_stats_requests "B26 start readiness"
    assert_tray_start_count 1 "B26 start success"
    assert_tray_stop_count 0 "B26 start success"
    assert_timeline_before "SYSTEMCTL start $SERVICE" 'IPC STATS' \
        "B26 start checks readiness after service transition"
    assert_timeline_before 'IPC STATS' \
        'SYSTEMCTL --user daemon-reload' \
        "B26 start launches tray only after daemon readiness"
    assert_service_state active "B26 start success"
    assert_tray_state active "B26 start success"
    assert_launcher_hup_isolation "B26 start success"
    assert_no_pid_or_undeclared_calls "B26 start success"
    assert_bounded "$CLI_RC" "$CLI_DURATION_MS" 1200 "B26 start success is bounded"

    reset_case
    printf 'active\n' >"$tmp_root/service.state"
    seed_active_tray
    start_fixture normal || return
    run_cli start-idempotent-first start
    assert_zero "$CLI_RC" "B26 start on an already active service is idempotent"
    assert_service_mutation_sequence "B26 idempotent start"
    assert_exact_stats_requests "B26 idempotent start"
    assert_tray_start_count 1 "B26 idempotent start"
    assert_tray_stop_count 0 "B26 idempotent start"
    assert_service_state active "B26 idempotent start"
    assert_tray_state active "B26 idempotent start"
    assert_no_pid_or_undeclared_calls "B26 idempotent start"

    clear_observations
    run_cli start-idempotent-second start
    assert_zero "$CLI_RC" "B26 repeated start remains idempotent"
    assert_service_mutation_sequence "B26 repeated idempotent start"
    assert_exact_stats_requests "B26 repeated idempotent start"
    assert_tray_start_count 1 "B26 repeated idempotent start"
    assert_tray_stop_count 0 "B26 repeated idempotent start"
    assert_service_state active "B26 repeated start"
    assert_tray_state active "B26 repeated start"
    assert_no_pid_or_undeclared_calls "B26 repeated start"

    reset_case
    TRAY_PATH="$tmp_root/bin/not-installed-tray"
    start_fixture normal || return
    run_cli start-daemon-only start
    assert_zero "$CLI_RC" "B26 daemon-only start succeeds"
    assert_service_mutation_sequence "B26 daemon-only start" start
    assert_exact_stats_requests "B26 daemon-only start readiness"
    assert_tray_start_count 0 "B26 daemon-only start"
    assert_tray_stop_count 0 "B26 daemon-only start"
    assert_service_state active "B26 daemon-only start"
    assert_tray_state inactive "B26 daemon-only start"
    assert_no_pid_or_undeclared_calls "B26 daemon-only start"
}

run_start_transport_failure() {
    local label=$1 fixture_mode=$2 response_kind=$3 expected=$4 rollback_mode=${5:-ok}
    local expected_state=inactive allowed_warning=""
    if [[ $rollback_mode != ok ]]; then
        allowed_warning='WARN rollback-incomplete: service-active'
    fi
    reset_case
    SYSTEMCTL_MODE=$rollback_mode
    prepare_response "$response_kind"
    if [[ $fixture_mode != none ]]; then
        start_fixture "$fixture_mode" || return
    fi
    run_cli "start-${label}-${rollback_mode//,/_}" start
    assert_nonzero "$CLI_RC" "B26 start $label with rollback=$rollback_mode"
    assert_error_category "$expected" "B26 start $label with rollback=$rollback_mode" \
        "$allowed_warning"
    assert_start_command start
    assert_service_mutation_sequence "B26 start $label rollback=$rollback_mode" start stop
    assert_call_count "SUDO -n systemctl stop $SERVICE" 1 \
        "B26 start $label attempts exactly one rollback stop"
    if [[ $rollback_mode != ok ]]; then
        expected_state=active
        assert_output_exact_line 'WARN rollback-incomplete: service-active' \
            "B26 start $label rollback=$rollback_mode reports the surviving backend"
    else
        assert_not_contains "$(plain_output <<<"$CLI_OUTPUT")" \
            'WARN rollback-incomplete:' \
            "B26 start $label successful rollback reports no false survivor"
    fi
    assert_service_state "$expected_state" "B26 start $label rollback=$rollback_mode"
    assert_tray_start_count 0 "B26 start $label rollback=$rollback_mode"
    assert_tray_stop_count 0 "B26 start $label rollback=$rollback_mode"
    assert_timeline_before "SYSTEMCTL start $SERVICE" "SYSTEMCTL stop $SERVICE" \
        "B26 start $label rollback occurs after the transition"
    assert_readiness_observation "$fixture_mode" "B26 start $label readiness"
    if [[ $fixture_mode != none && $fixture_mode != denied ]]; then
        assert_timeline_before "SYSTEMCTL start $SERVICE" 'IPC STATS' \
            "B26 start $label checks IPC only after service start"
        assert_timeline_before 'IPC STATS' "SYSTEMCTL stop $SERVICE" \
            "B26 start $label rolls back only after IPC failure"
    fi
    assert_no_pid_or_undeclared_calls "B26 start $label rollback=$rollback_mode"
    assert_bounded "$CLI_RC" "$CLI_DURATION_MS" 1500 \
        "B26 start $label rollback=$rollback_mode is bounded"
}

run_start_failure_matrix() {
    local label fixture response expected rollback
    local -a readiness_cases=(
        'unavailable none valid unavailable'
        'denied denied valid denied'
        'timeout stall valid timeout'
        'protocol normal protocol protocol-error'
        'daemon normal daemon daemon-error'
    )

    for rollback in ok fail-stop hang-stop sticky-stop; do
        for label in "${readiness_cases[@]}"; do
            read -r label fixture response expected <<<"$label"
            run_start_transport_failure "$label" "$fixture" "$response" "$expected" "$rollback"
        done
    done

    reset_case
    SYSTEMCTL_MODE=fail-start
    run_cli start-service-error start
    assert_nonzero "$CLI_RC" "B26 start service failure"
    assert_error_category service-error "B26 start service failure"
    assert_service_mutation_sequence "B26 failed service start" start
    assert_no_requests "B26 failed service start"
    assert_nc_count 0 "B26 failed service start"
    assert_tray_start_count 0 "B26 failed service start"
    assert_service_state inactive "B26 failed service start"
    assert_no_pid_or_undeclared_calls "B26 failed service start"

    reset_case
    SYSTEMCTL_MODE=hang-start
    run_cli start-service-timeout start
    assert_nonzero "$CLI_RC" "B26 hung service start"
    assert_error_category service-timeout "B26 hung service start"
    assert_service_mutation_sequence "B26 hung service start" start
    assert_no_requests "B26 hung service start"
    assert_nc_count 0 "B26 hung service start"
    assert_tray_start_count 0 "B26 hung service start"
    assert_service_state inactive "B26 hung service start"
    assert_no_pid_or_undeclared_calls "B26 hung service start"
    assert_bounded "$CLI_RC" "$CLI_DURATION_MS" 900 "B26 command timeout bounds hung start"

    local -a active_readiness_cases=(
        'unavailable none valid unavailable'
        'denied denied valid denied'
        'timeout stall valid timeout'
        'protocol normal protocol protocol-error'
        'daemon normal daemon daemon-error'
    )
    for label in "${active_readiness_cases[@]}"; do
        read -r label fixture response expected <<<"$label"
        reset_case
        printf 'active\n' >"$tmp_root/service.state"
        TRAY_PATH="$tmp_root/bin/not-installed-tray"
        prepare_response "$response"
        if [[ $fixture != none ]]; then
            start_fixture "$fixture" || continue
        fi
        run_cli "start-active-$label" start
        assert_nonzero "$CLI_RC" "B26 start on active backend with $label IPC"
        assert_error_category "$expected" \
            "B26 start on active backend with $label IPC"
        assert_service_mutation_sequence \
            "B26 active $label start does not mutate the shared service"
        assert_call_count "SUDO -n systemctl restart $SERVICE" 0 \
            "B26 active $label start never restarts the shared service"
        assert_service_state active "B26 active $label backend"
        assert_tray_start_count 0 "B26 active $label backend"
        if [[ $fixture == none ]]; then
            assert_no_requests "B26 active $label backend"
            assert_nc_count 0 "B26 active $label backend"
        else
            assert_readiness_observation "$fixture" \
                "B26 active $label backend readiness"
        fi
        assert_no_pid_or_undeclared_calls "B26 active $label backend"
        assert_bounded "$CLI_RC" "$CLI_DURATION_MS" 900 \
            "B26 active $label start is bounded"
    done

    reset_case
    TRAY_MODE=fail
    start_fixture normal || return
    run_cli start-tray-error start
    assert_zero "$CLI_RC" "B26 optional tray failure preserves start success"
    assert_output_exact_line 'WARN tray-unavailable' \
        "B26 start tray failure is explicit"
    assert_service_mutation_sequence "B26 start tray failure" start
    assert_exact_stats_requests "B26 start tray failure readiness"
    assert_tray_start_count 1 "B26 start tray failure"
    assert_tray_stop_count 0 "B26 start tray failure"
    assert_service_state active "B26 tray failure preserves healthy backend"
    assert_tray_state inactive "B26 failed tray launch"
    assert_no_pid_or_undeclared_calls "B26 start tray failure"
}

run_restart_transport_failure() {
    local label=$1 fixture_mode=$2 response_kind=$3 expected=$4
    reset_case
    printf 'active\n' >"$tmp_root/service.state"
    seed_active_tray
    prepare_response "$response_kind"
    if [[ $fixture_mode != none ]]; then
        start_fixture "$fixture_mode" || return
    fi
    run_cli "restart-$label" restart
    assert_nonzero "$CLI_RC" "B26 restart $label"
    assert_error_category "$expected" "B26 restart $label"
    assert_service_mutation_sequence "B26 restart $label" restart
    assert_readiness_observation "$fixture_mode" "B26 restart $label readiness"
    assert_tray_stop_count 0 "B26 restart $label"
    assert_tray_start_count 0 "B26 restart $label"
    assert_service_state active "B26 restart $label"
    assert_tray_state active "B26 restart $label preserves existing tray"
    if [[ $fixture_mode != none && $fixture_mode != denied ]]; then
        assert_timeline_before "SYSTEMCTL restart $SERVICE" 'IPC STATS' \
            "B26 restart $label checks IPC only after restart"
    fi
    assert_no_pid_or_undeclared_calls "B26 restart $label"
    assert_bounded "$CLI_RC" "$CLI_DURATION_MS" 1200 "B26 restart $label is bounded"
}

run_restart_matrix() {
    reset_case
    printf 'active\n' >"$tmp_root/service.state"
    seed_active_tray
    start_fixture normal || return
    run_cli restart-success restart
    assert_zero "$CLI_RC" "B26 restart success"
    assert_service_mutation_sequence "B26 restart success" restart
    assert_exact_stats_requests "B26 restart readiness"
    assert_tray_stop_count 1 "B26 restart success"
    assert_tray_start_count 1 "B26 restart success"
    assert_timeline_before "SYSTEMCTL restart $SERVICE" 'IPC STATS' \
        "B26 restart checks readiness after transition"
    assert_timeline_before 'IPC STATS' \
        'SYSTEMCTL --user daemon-reload' \
        "B26 restart preserves tray lifecycle until backend is ready"
    assert_service_state active "B26 restart success"
    assert_tray_state active "B26 restart success"
    assert_launcher_hup_isolation "B26 restart success"
    assert_no_pid_or_undeclared_calls "B26 restart success"

    reset_case
    printf 'active\n' >"$tmp_root/service.state"
    TRAY_PATH="$tmp_root/bin/not-installed-tray"
    start_fixture normal || return
    run_cli restart-daemon-only restart
    assert_zero "$CLI_RC" "B26 daemon-only restart succeeds"
    assert_service_mutation_sequence "B26 daemon-only restart" restart
    assert_exact_stats_requests "B26 daemon-only restart readiness"
    assert_tray_stop_count 0 "B26 daemon-only restart"
    assert_tray_start_count 0 "B26 daemon-only restart"
    assert_service_state active "B26 daemon-only restart"
    assert_tray_state inactive "B26 daemon-only restart"
    assert_no_pid_or_undeclared_calls "B26 daemon-only restart"

    reset_case
    printf 'active\n' >"$tmp_root/service.state"
    seed_active_tray
    SYSTEMCTL_MODE=fail-restart
    run_cli restart-service-error restart
    assert_nonzero "$CLI_RC" "B26 restart service failure"
    assert_error_category service-error "B26 restart service failure"
    assert_service_mutation_sequence "B26 failed restart" restart
    assert_no_requests "B26 failed restart"
    assert_nc_count 0 "B26 failed restart"
    assert_tray_stop_count 0 "B26 failed restart"
    assert_tray_start_count 0 "B26 failed restart"
    assert_tray_state active "B26 failed restart"
    assert_no_pid_or_undeclared_calls "B26 failed restart"

    reset_case
    printf 'active\n' >"$tmp_root/service.state"
    seed_active_tray
    SYSTEMCTL_MODE=hang-restart
    run_cli restart-service-timeout restart
    assert_nonzero "$CLI_RC" "B26 hung restart"
    assert_error_category service-timeout "B26 hung restart"
    assert_service_mutation_sequence "B26 hung restart" restart
    assert_no_requests "B26 hung restart"
    assert_nc_count 0 "B26 hung restart"
    assert_tray_stop_count 0 "B26 hung restart"
    assert_tray_start_count 0 "B26 hung restart"
    assert_tray_state active "B26 hung restart"
    assert_no_pid_or_undeclared_calls "B26 hung restart"
    assert_bounded "$CLI_RC" "$CLI_DURATION_MS" 900 "B26 command timeout bounds hung restart"

    run_restart_transport_failure unavailable none valid unavailable
    run_restart_transport_failure denied denied valid denied
    run_restart_transport_failure timeout stall valid timeout
    run_restart_transport_failure protocol normal protocol protocol-error
    run_restart_transport_failure daemon normal daemon daemon-error

    reset_case
    printf 'active\n' >"$tmp_root/service.state"
    seed_active_tray
    TRAY_MODE=fail
    start_fixture normal || return
    run_cli restart-tray-error restart
    assert_zero "$CLI_RC" "B26 optional tray failure preserves restart success"
    assert_output_exact_line 'WARN tray-unavailable' \
        "B26 restart tray failure is explicit"
    assert_service_mutation_sequence "B26 restart tray failure" restart
    assert_exact_stats_requests "B26 restart tray failure readiness"
    assert_tray_stop_count 1 "B26 restart tray failure"
    assert_tray_start_count 1 "B26 restart tray failure"
    assert_timeline_before 'IPC STATS' \
        'SYSTEMCTL --user daemon-reload' \
        "B26 restart tray replacement starts only after readiness"
    assert_service_state active "B26 restart tray failure"
    assert_tray_state active "B26 restart tray failure preserves manager state"
    assert_no_pid_or_undeclared_calls "B26 restart tray failure"
}

run_stop_case() {
    local label=$1 systemctl_mode=$2 expected_category=$3 expected_service_state=$4
    reset_case
    printf 'active\n' >"$tmp_root/service.state"
    seed_active_tray
    SYSTEMCTL_MODE=$systemctl_mode
    run_cli "stop-$label" stop
    if [[ $expected_category == none ]]; then
        assert_zero "$CLI_RC" "B26 stop $label"
    else
        assert_nonzero "$CLI_RC" "B26 stop $label"
        assert_error_category "$expected_category" "B26 stop $label"
    fi
    assert_tray_stop_count 1 "B26 stop $label"
    assert_tray_start_count 0 "B26 stop $label"
    assert_tray_state inactive "B26 stop $label"
    assert_timeline_before \
        'SYSTEMCTL --user stop -- punto-tray.service' \
        "SYSTEMCTL stop $SERVICE" \
        "B26 stop $label asks the user supervisor before stopping the backend"
    assert_service_state "$expected_service_state" "B26 stop $label"
    assert_no_pid_or_undeclared_calls "B26 stop $label"
}

run_stop_matrix() {
    local fake_clock
    reset_case
    run_cli stop-idempotent-first stop
    assert_zero "$CLI_RC" "B26 stop on inactive service and tray succeeds"
    assert_service_mutation_sequence "B26 inactive stop"
    assert_tray_stop_count 1 "B26 inactive stop"
    assert_tray_start_count 0 "B26 inactive stop"
    assert_service_state inactive "B26 inactive stop"
    assert_tray_state inactive "B26 inactive stop"
    assert_no_pid_or_undeclared_calls "B26 inactive stop"
    clear_observations
    run_cli stop-idempotent-second stop
    assert_zero "$CLI_RC" "B26 repeated stop remains idempotent"
    assert_service_mutation_sequence "B26 repeated inactive stop"
    assert_tray_stop_count 1 "B26 repeated inactive stop"
    assert_service_state inactive "B26 repeated stop"
    assert_no_pid_or_undeclared_calls "B26 repeated stop"

    reset_case
    seed_active_tray
    run_cli stop-tray-only stop
    assert_zero "$CLI_RC" "B26 stop handles an active tray with inactive backend"
    assert_service_mutation_sequence "B26 tray-only stop"
    assert_tray_stop_count 1 "B26 tray-only stop"
    assert_tray_state inactive "B26 tray-only stop"
    assert_no_pid_or_undeclared_calls "B26 tray-only stop"

    reset_case
    printf 'active\n' >"$tmp_root/service.state"
    TRAY_PATH="$tmp_root/bin/not-installed-tray"
    run_cli stop-daemon-only stop
    assert_zero "$CLI_RC" "B26 daemon-only stop succeeds"
    assert_service_mutation_sequence "B26 daemon-only stop" stop
    assert_tray_stop_count 0 "B26 daemon-only stop"
    assert_service_state inactive "B26 daemon-only stop"
    assert_no_pid_or_undeclared_calls "B26 daemon-only stop"

    run_stop_case success ok none inactive
    run_stop_case service-error fail-stop service-error active
    run_stop_case service-timeout hang-stop service-timeout active

    reset_case
    printf 'active\n' >"$tmp_root/service.state"
    seed_active_tray
    SYSTEMCTL_MODE=sticky-stop
    run_cli stop-term-success stop
    assert_zero "$CLI_RC" "B26 TERM fallback stops sticky service"
    assert_tray_stop_count 1 "B26 TERM fallback"
    assert_service_mutation_sequence "B26 TERM fallback" stop term
    assert_timeline_before "SYSTEMCTL stop $SERVICE" \
        "SYSTEMCTL kill --kill-who=all --signal=TERM $SERVICE" \
        "B26 TERM fallback follows bounded graceful wait"
    assert_service_state inactive "B26 TERM fallback"
    fake_clock=$(<"$tmp_root/clock.ms")
    if [[ $fake_clock -ge $STOP_TIMEOUT_MS && \
          $fake_clock -le $((STOP_TIMEOUT_MS + POLL_INTERVAL_MS * 2)) ]]; then
        pass "B26 stop deadline uses deterministic fake clock"
    else
        fail "B26 stop fake clock is $fake_clock, expected near $STOP_TIMEOUT_MS"
    fi
    assert_no_pid_or_undeclared_calls "B26 TERM fallback"

    reset_case
    printf 'active\n' >"$tmp_root/service.state"
    seed_active_tray
    SYSTEMCTL_MODE=sticky-all
    run_cli stop-still-active stop
    assert_nonzero "$CLI_RC" "B26 service remaining active after TERM fails"
    assert_error_category service-error "B26 service remaining active after TERM"
    assert_tray_stop_count 1 "B26 sticky service failure"
    assert_service_mutation_sequence "B26 sticky service failure" stop term
    assert_service_state active "B26 sticky service failure"
    assert_tray_state inactive "B26 sticky service failure"
    assert_no_pid_or_undeclared_calls "B26 sticky service failure"
    assert_bounded "$CLI_RC" "$CLI_DURATION_MS" 1000 "B26 sticky service failure is bounded"

    for systemctl_mode in sticky-stop,fail-kill sticky-stop,hang-kill; do
        reset_case
        printf 'active\n' >"$tmp_root/service.state"
        seed_active_tray
        SYSTEMCTL_MODE=$systemctl_mode
        run_cli "stop-term-${systemctl_mode//,/_}" stop
        assert_nonzero "$CLI_RC" "B26 TERM failure $systemctl_mode"
        if [[ $systemctl_mode == *hang-kill ]]; then
            assert_error_category service-timeout "B26 TERM failure $systemctl_mode"
        else
            assert_error_category service-error "B26 TERM failure $systemctl_mode"
        fi
        assert_tray_stop_count 1 "B26 TERM failure $systemctl_mode"
        assert_service_mutation_sequence "B26 TERM failure $systemctl_mode" stop term
        assert_service_state active "B26 TERM failure $systemctl_mode"
        assert_no_pid_or_undeclared_calls "B26 TERM failure $systemctl_mode"
        assert_bounded "$CLI_RC" "$CLI_DURATION_MS" 1000 \
            "B26 TERM failure $systemctl_mode is bounded"
    done

    reset_case
    printf 'active\n' >"$tmp_root/service.state"
    seed_active_tray
    TRAY_MODE=fail
    run_cli stop-tray-manager-unavailable stop
    assert_zero "$CLI_RC" \
        "B26 unavailable tray manager does not block backend stop"
    assert_output_exact_line 'WARN tray-unavailable' \
        "B26 unavailable tray manager is explicit"
    assert_service_mutation_sequence \
        "B26 unavailable tray manager still stops backend" stop
    assert_service_state inactive \
        "B26 unavailable tray manager leaves backend stopped"
    assert_tray_state active \
        "B26 unavailable tray manager cannot claim tray termination"
    assert_no_pid_or_undeclared_calls \
        "B26 unavailable tray manager has no PID fallback"
}

set_timeout_value() {
    local name=$1 value=$2
    case $name in
        PUNTO_IPC_TIMEOUT_MS) IPC_TIMEOUT_MS=$value ;;
        PUNTO_COMMAND_TIMEOUT_MS) COMMAND_TIMEOUT_MS=$value ;;
        PUNTO_START_TIMEOUT_MS) START_TIMEOUT_MS=$value ;;
        PUNTO_STOP_TIMEOUT_MS) STOP_TIMEOUT_MS=$value ;;
        PUNTO_POLL_INTERVAL_MS) POLL_INTERVAL_MS=$value ;;
    esac
}

run_timeout_validation_matrix() {
    local name value sentinel="$tmp_root/timeout-injection-sentinel" fake_clock
    local -a names=(
        PUNTO_IPC_TIMEOUT_MS PUNTO_COMMAND_TIMEOUT_MS PUNTO_START_TIMEOUT_MS
        PUNTO_STOP_TIMEOUT_MS PUNTO_POLL_INTERVAL_MS
    )
    local -a invalid_values=(abc 0 -1 60001 1.5 10ms ' 10')

    for name in "${names[@]}"; do
        for value in "${invalid_values[@]}"; do
            reset_case
            set_timeout_value "$name" "$value"
            run_cli "invalid-${name}-${value//[^a-zA-Z0-9]/_}" status
            assert_nonzero "$CLI_RC" "B26 invalid $name='$value'"
            assert_error_category invalid-configuration "B26 invalid $name='$value'"
            assert_no_requests "B26 invalid $name='$value'"
            assert_no_control_calls "B26 invalid $name='$value'"
            assert_no_pid_or_undeclared_calls "B26 invalid $name='$value'"
            assert_bounded "$CLI_RC" "$CLI_DURATION_MS" 500 "B26 invalid $name='$value' is bounded"
        done
    done

    reset_case
    IPC_TIMEOUT_MS="10;touch $sentinel"
    run_cli timeout-injection status
    assert_error_category invalid-configuration "B26 hostile timeout value"
    if [[ ! -e $sentinel ]]; then
        pass "B26 timeout environment cannot execute shell syntax"
    else
        fail "B26 timeout environment executed shell syntax"
    fi
    assert_no_control_calls "B26 hostile timeout value"

    reset_case
    IPC_TIMEOUT_MS=10
    COMMAND_TIMEOUT_MS=10
    START_TIMEOUT_MS=10
    STOP_TIMEOUT_MS=10
    POLL_INTERVAL_MS=1
    run_cli timeout-lower-bound --version
    assert_zero "$CLI_RC" "B26 documented timeout lower bounds are accepted"
    assert_output_exact_line "punto $EXPECTED_VERSION" \
        "B26 lower-bound configuration preserves local command behavior"
    assert_no_requests "B26 documented timeout lower bounds"
    assert_nc_count 0 "B26 documented timeout lower bounds"
    assert_no_control_calls "B26 documented timeout lower bounds"
    assert_no_pid_or_undeclared_calls "B26 documented timeout lower bounds"
    assert_bounded "$CLI_RC" "$CLI_DURATION_MS" 800 \
        "B26 documented timeout lower bounds stay bounded"

    reset_case
    IPC_TIMEOUT_MS=60000
    COMMAND_TIMEOUT_MS=60000
    START_TIMEOUT_MS=60000
    STOP_TIMEOUT_MS=60000
    POLL_INTERVAL_MS=60000
    start_fixture normal || return
    run_cli timeout-upper-bound status
    assert_status_success "$VALID_STATS" "B26 documented timeout upper bounds"

    reset_case
    START_TIMEOUT_MS=28
    POLL_INTERVAL_MS=7
    # Keep the IPC process-spawn budget independent from the startup-deadline
    # boundary under load.  This case exercises the 28 ms socket wait; the
    # dedicated transport matrix covers the 10 ms IPC timeout itself.
    IPC_TIMEOUT_MS=100
    TRAY_PATH="$tmp_root/bin/not-installed-tray"
    run_cli timeout-start-mutation start
    assert_nonzero "$CLI_RC" "B26 mutated start timeout reaches terminal failure"
    assert_error_category unavailable "B26 mutated start timeout"
    fake_clock=$(<"$tmp_root/clock.ms")
    if [[ $fake_clock -ge 28 && $fake_clock -le 35 ]]; then
        pass "B26 PUNTO_START_TIMEOUT_MS=28 is honored by fake clock"
    else
        fail "B26 start timeout fake clock is $fake_clock"
    fi
    assert_contains "$(<"$tmp_root/calls.log")" 'SLEEP 0.007' \
        "B26 PUNTO_POLL_INTERVAL_MS=7 controls polling cadence"
    assert_service_state inactive "B26 mutated start timeout rollback"
    assert_service_mutation_sequence "B26 mutated start timeout" start stop
    assert_no_pid_or_undeclared_calls "B26 mutated start timeout"

    reset_case
    printf 'active\n' >"$tmp_root/service.state"
    seed_active_tray
    STOP_TIMEOUT_MS=20
    POLL_INTERVAL_MS=10
    SYSTEMCTL_MODE=sticky-stop
    run_cli timeout-stop-mutation stop
    assert_zero "$CLI_RC" "B26 mutated stop timeout reaches TERM fallback"
    fake_clock=$(<"$tmp_root/clock.ms")
    if [[ $fake_clock -ge 20 && $fake_clock -le 40 ]]; then
        pass "B26 PUNTO_STOP_TIMEOUT_MS=20 is honored by fake clock"
    else
        fail "B26 stop timeout fake clock is $fake_clock"
    fi
    assert_call_count "SUDO -n systemctl kill --kill-who=all --signal=TERM $SERVICE" 1 \
        "B26 mutated stop timeout triggers one TERM"
    assert_service_mutation_sequence "B26 mutated stop timeout" stop term
    assert_no_pid_or_undeclared_calls "B26 mutated stop timeout"

    reset_case
    COMMAND_TIMEOUT_MS=80
    SYSTEMCTL_MODE=hang-start
    run_cli timeout-command-mutation start
    assert_nonzero "$CLI_RC" "B26 mutated command timeout terminates hung systemctl"
    assert_error_category service-timeout "B26 mutated command timeout"
    assert_service_mutation_sequence "B26 mutated command timeout" start
    assert_bounded "$CLI_RC" "$CLI_DURATION_MS" 700 "B26 PUNTO_COMMAND_TIMEOUT_MS=80 bounds systemctl"
    assert_no_pid_or_undeclared_calls "B26 mutated command timeout"

    reset_case
    IPC_TIMEOUT_MS=25
    start_fixture stall || return
    run_cli timeout-ipc-mutation status
    assert_nonzero "$CLI_RC" "B26 mutated IPC timeout terminates stalled peer"
    assert_error_category timeout "B26 mutated IPC timeout"
    assert_bounded "$CLI_RC" "$CLI_DURATION_MS" 600 "B26 PUNTO_IPC_TIMEOUT_MS=25 bounds AF_UNIX read"
    assert_no_pid_or_undeclared_calls "B26 mutated IPC timeout"
}

run_static_safety_gate() {
    local source pid_hits raw_kill_verdict raw_kill_rc
    source=$(sed 's/[[:space:]]*#.*$//' "$CLI_SCRIPT")
    # shellcheck disable=SC2016 # These are literal source-code assertions.
    assert_contains "$source" 'TRAY=${PUNTO_TRAY:-/usr/bin/punto-tray}' \
        "B26 CLI default tray path matches the package manifest"
    # shellcheck disable=SC2016 # These are literal source-code assertions.
    assert_contains "$source" 'SOCKET=${PUNTO_SOCKET:-/var/run/punto.sock}' \
        "B26 CLI default socket path matches the daemon control endpoint"
    # shellcheck disable=SC2016 # These are literal source-code assertions.
    assert_contains "$source" \
        'VERSION_FILE=${PUNTO_VERSION_FILE:-/usr/share/punto-switcher/VERSION}' \
        "B26 CLI default VERSION path matches the package manifest"
    pid_hits=$(grep -En "(^|[^[:alnum:]_])(pgrep|pkill|pidof|killall|ps)([^[:alnum:]_]|$)|/proc/[^/]+/(stat|cmdline|status)([^[:alnum:]_]|$)|systemctl[[:space:]]+show.*(MainPID|ControlPID)|(^|[^[:alnum:]_])(MainPID|ControlPID)([^[:alnum:]_]|$)" <<<"$source" || true)
    if [[ -z $pid_hits ]]; then
        pass "B26 CLI source contains no PID/process heuristic"
    else
        fail "B26 CLI source contains PID/process heuristics: ${pid_hits//$'\n'/; }"
    fi
    set +e
    raw_kill_verdict=$(/usr/bin/python3 - "$CLI_SCRIPT" <<'PY'
import re
import sys

path = sys.argv[1]
logical_lines = []
pending = ""
heredoc = None
for number, physical in enumerate(open(path, encoding="utf-8"), 1):
    line = physical.rstrip("\n")
    if heredoc is not None:
        if line.strip() == heredoc:
            heredoc = None
        continue
    marker = re.search(r"<<-?\s*['\"]?([A-Za-z_][A-Za-z0-9_]*)['\"]?", line)
    if marker:
        heredoc = marker.group(1)
    line = re.sub(r"\s+#.*$", "", line)
    pending += line
    if pending.endswith("\\"):
        pending = pending[:-1] + " "
        continue
    logical_lines.append((number, pending))
    pending = ""

violations = []
command_pattern = re.compile(
    r"(?:^|[;&|({])\s*(?:(?:if|while|until|!)\s+)?(?:(?:command|builtin)\s+)?"
    r"(?:(?:/usr)?/bin/)?kill\s+([^;&|}]*)"
)
target_pattern = re.compile(
    r"(?:^|\s)(?:--\s+)?(?:['\"]?-?\$(?:\{([A-Za-z_][A-Za-z0-9_]*)\}|"
    r"([A-Za-z_][A-Za-z0-9_]*))['\"]?|['\"]-\$(?:\{([A-Za-z_][A-Za-z0-9_]*)\}|"
    r"([A-Za-z_][A-Za-z0-9_]*))['\"])\s*$"
)
assignment_pattern = re.compile(
    r"(?:^|[;\s])(?:(?:local|declare|typeset|export|readonly)\s+)?"
    r"([A-Za-z_][A-Za-z0-9_]*)\s*="
)
declaration_pattern = re.compile(
    r"(?:^|[;\s])(?:local|declare|typeset|export|readonly)\s+"
    r"([A-Za-z_][A-Za-z0-9_]*)(?=\s|;|$)"
)
read_pattern = re.compile(
    r"(?:^|[;&|]\s*)read(?:\s+-[A-Za-z]+(?:\s+[^\s;]+)?)*\s+"
    r"([A-Za-z_][A-Za-z0-9_]*)"
)
printf_pattern = re.compile(r"(?:^|[;&|]\s*)printf\s+-v\s+([A-Za-z_][A-Za-z0-9_]*)")
arithmetic_pattern = re.compile(
    r"(?:\(\(|\blet\s+)(?:[^;)]*?)([A-Za-z_][A-Za-z0-9_]*)\s*(?:\+\+|--|[-+*/%]?=)"
)
parameter_assignment_pattern = re.compile(
    r"\$\{([A-Za-z_][A-Za-z0-9_]*)\s*:?[-+?=]"
)

scope = "global"
proven = {}
spawned_names = set()
for _, line in logical_lines:
    for assignment in assignment_pattern.finditer(line):
        statement = line[assignment.end():].split(";", 1)[0].strip()
        if statement.startswith("$!") or re.fullmatch(
            r"\$\{?PUNTO_COMMAND_PID\}?", statement
        ):
            spawned_names.add(assignment.group(1))
for number, line in logical_lines:
    function = re.match(
        r"\s*(?:(?:function\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*\(\)|"
        r"function\s+([A-Za-z_][A-Za-z0-9_]*))\s*\{",
        line,
    )
    if function:
        scope = f"{function.group(1) or function.group(2)}@{number}"
        proven = {}

    events = []
    for assignment in assignment_pattern.finditer(line):
        statement = line[assignment.end():].split(";", 1)[0]
        safe_load = (
            "PUNTO_TRAY_PID_FILE" in statement
            and statement.lstrip().startswith("$(")
        )
        spawned = statement.strip().startswith("$!") or re.fullmatch(
            r"\$\{?PUNTO_COMMAND_PID\}?", statement.strip()
        )
        provenance = "pidfile" if safe_load else "spawned" if spawned else None
        events.append((assignment.start(1), "assign", assignment.group(1), provenance))
    for declaration in declaration_pattern.finditer(line):
        if not re.match(r"\s*=", line[declaration.end():]):
            events.append((declaration.start(1), "assign", declaration.group(1), None))
    for read in read_pattern.finditer(line):
        statement = line[read.start():].split(";", 1)[0]
        safe_load = "PUNTO_TRAY_PID_FILE" in statement and "<" in statement
        events.append((read.start(1), "assign", read.group(1),
                       "pidfile" if safe_load else None))
    for printf in printf_pattern.finditer(line):
        events.append((printf.start(1), "assign", printf.group(1), None))
    for arithmetic in arithmetic_pattern.finditer(line):
        events.append((arithmetic.start(1), "assign", arithmetic.group(1), None))
    for parameter in parameter_assignment_pattern.finditer(line):
        events.append((parameter.start(1), "assign", parameter.group(1), None))
    for command in command_pattern.finditer(line):
        events.append((command.start(), "kill", command, False))

    for _, kind, value, provenance in sorted(events, key=lambda event: event[0]):
        if kind == "assign":
            proven.pop(value, None)
            if provenance is not None:
                proven[value] = provenance
            continue
        arguments = value.group(1).strip()
        target = target_pattern.search(arguments)
        variable = next((group for group in target.groups() if group), None) if target else None
        prefix = arguments[:target.start()].strip() if target else arguments
        signal_match = re.fullmatch(
            r"(?:(?:-0|-(?:TERM|SIGTERM|KILL|SIGKILL)|"
            r"--signal=(?:TERM|SIGTERM|KILL|SIGKILL)|"
            r"-s\s+(?:TERM|SIGTERM|KILL|SIGKILL)|"
            r"--signal\s+(?:TERM|SIGTERM|KILL|SIGKILL))\s*)*",
            prefix,
        )
        uses_sigkill = re.search(r"(?:^|\s)(?:-(?:KILL|SIGKILL)|"
                                 r"--signal[=\s]+(?:KILL|SIGKILL)|"
                                 r"-s\s+(?:KILL|SIGKILL))(?:\s|$)", prefix)
        owned = proven.get(variable)
        if owned is None and variable in spawned_names:
            owned = "spawned"
        unsafe = (
            variable is None
            or owned is None
            or signal_match is None
            or (uses_sigkill and owned != "spawned")
        )
        if unsafe:
            violations.append(
                f"line {number} ({scope}): kill target/signal lacks current ownership provenance: {arguments}"
            )

    if re.fullmatch(r"\s*}\s*", line):
        scope = "global"
        proven = {}

print("\n".join(violations))
PY
    )
    raw_kill_rc=$?
    set +e
    if [[ $raw_kill_rc -ne 0 ]]; then
        fail "B26 raw-kill provenance parser failed closed (rc=$raw_kill_rc)"
    elif [[ -z $raw_kill_verdict ]]; then
        pass "B26 raw shell kill targets only current PID-file or just-spawned child provenance; SIGKILL is child-only"
    else
        fail "B26 raw shell kill lacks strict owned-PID provenance: ${raw_kill_verdict//$'\n'/; }"
    fi
}

run_execve_inventory_gate() {
    local trace_count bad_cases="" case_dir result parser_rc unknown providers
    local provider metadata essential priority
    trace_count=$(find "$tmp_root/execve" -mindepth 1 -maxdepth 1 -type d | wc -l)
    while IFS= read -r case_dir; do
        [[ -n $case_dir ]] || continue
        if [[ ! -f $case_dir/cli.rc || -L $case_dir/cli.rc ]] || \
           ! find "$case_dir" -maxdepth 1 -type f -name 'trace.*' -print -quit | grep -q .; then
            bad_cases+="${case_dir##*/} "
        fi
    done < <(find "$tmp_root/execve" -mindepth 1 -maxdepth 1 -type d | sort)
    if [[ $trace_count -eq $CLI_RUN_SERIAL && $CLI_RUN_SERIAL -gt 0 && -z $bad_cases ]]; then
        pass "B26 every CLI scenario has an independent execve trace"
    else
        fail "B26 execve evidence has $trace_count case dirs for $CLI_RUN_SERIAL scenarios; incomplete=${bad_cases:-none}"
    fi

    set +e
    result=$(/usr/bin/python3 - "$tmp_root/execve" "$tmp_root/bin" "$tmp_root/trace-wrapper" <<'PY'
import pathlib
import re
import stat
import sys

trace_root = pathlib.Path(sys.argv[1])
controlled_root = pathlib.Path(sys.argv[2])
wrapper = pathlib.Path(sys.argv[3])
host_providers = {
    "bash": "bash",
    "basename": "coreutils",
    "cat": "coreutils",
    "chmod": "coreutils",
    "cut": "coreutils",
    "date": "coreutils",
    "dirname": "coreutils",
    "head": "coreutils",
    "id": "coreutils",
    "install": "coreutils",
    "mkdir": "coreutils",
    "mkfifo": "coreutils",
    "mktemp": "coreutils",
    "mv": "coreutils",
    "readlink": "coreutils",
    "realpath": "coreutils",
    "rm": "coreutils",
    "rmdir": "coreutils",
    "sha256sum": "coreutils",
    "sleep": "coreutils",
    "sort": "coreutils",
    "stat": "coreutils",
    "tr": "coreutils",
    "wc": "coreutils",
    "grep": "grep",
    "sed": "sed",
    "awk": "mawk",
    "flock": "util-linux",
    "getent": "libc-bin",
    "systemctl": "systemd",
    "sudo": "sudo-runtime",
    "nc": "netcat-openbsd-runtime",
    "nc.openbsd": "netcat-openbsd-runtime",
    "nc-real": "netcat-openbsd-runtime",
    "env": "coreutils",
    "timeout": "coreutils",
    "nohup": "coreutils",
    "nohup-real": "coreutils",
    "setsid": "util-linux",
    "setsid-real": "util-linux",
    "sg": "login",
    "newgrp": "login",
    "pgrep": "procps",
    "pkill": "procps",
    "pidof": "sysvinit-utils",
    "killall": "psmisc",
    "ps": "procps",
    "kill": "procps",
    "journalctl": "systemd",
}
controlled_providers = {
    "systemctl": "systemd",
    "sudo": "sudo-runtime",
    "nc": "netcat-openbsd-runtime",
    "nc-real": "netcat-openbsd-runtime",
    "timeout": "coreutils",
    "nohup": "coreutils",
    "nohup-real": "coreutils",
    "setsid": "util-linux",
    "setsid-real": "util-linux",
    "sg": "login",
    "newgrp": "login",
    "pgrep": "procps",
    "pkill": "procps",
    "pidof": "sysvinit-utils",
    "killall": "psmisc",
    "ps": "procps",
    "kill": "procps",
    "id": "coreutils",
    "sleep": "coreutils",
    "date": "coreutils",
    "journalctl": "systemd",
    "punto-daemon-fixture": "controlled-fixture",
    "punto-tray-fixture": "controlled-fixture",
}
host_directories = {pathlib.Path("/bin"), pathlib.Path("/usr/bin"), pathlib.Path("/usr/sbin")}
observed = set()
unknown = []
start = re.compile(r'^execve\("([^"]+)"')
result = re.compile(r"= (-?[0-9]+)(?:\s|$)")
trace_files = sorted(trace_root.glob("*/trace.*"))
if not trace_files:
    raise SystemExit("no execve trace files")


def record(trace, raw_path, invocation):
    path = pathlib.Path(raw_path)
    name = path.name
    if not path.is_absolute():
        unknown.append(f"{trace.name}: non-absolute executable {path}")
        return
    if path == wrapper:
        observed.add("controlled-wrapper")
        return
    if path.parent == controlled_root:
        provider = controlled_providers.get(name)
    elif path.parent in host_directories:
        provider = host_providers.get(name)
        if name == "python3" and (
            "import signal,time; signal.signal(signal.SIGTERM, signal.SIG_IGN);"
            in invocation
        ):
            provider = "python3-test-hang-fixture"
    else:
        provider = None
    if provider is None:
        unknown.append(f"{trace.parent.name}/{trace.name}: {path}")
        return
    observed.add(provider)


for trace in trace_files:
    metadata = trace.lstat()
    if not stat.S_ISREG(metadata.st_mode):
        raise SystemExit(f"non-regular execve trace: {trace}")
    pending = None
    for line in trace.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("execve("):
            match = start.search(line)
            if match is None:
                unknown.append(f"{trace.parent.name}/{trace.name}: malformed execve: {line}")
                continue
            path = match.group(1)
            if line.endswith("<unfinished ...>"):
                if pending is not None:
                    unknown.append(
                        f"{trace.parent.name}/{trace.name}: overlapping unfinished execve"
                    )
                pending = (path, line)
                continue
            status = result.search(line)
            if status is None:
                unknown.append(f"{trace.parent.name}/{trace.name}: unparsed execve result: {line}")
                continue
            if status.group(1) == "0":
                record(trace, path, line)
            continue
        if line.startswith("<... execve resumed>"):
            if pending is None:
                unknown.append(f"{trace.parent.name}/{trace.name}: orphan execve resume")
                continue
            status = result.search(line)
            if status is None:
                unknown.append(f"{trace.parent.name}/{trace.name}: unparsed execve resume: {line}")
            elif status.group(1) == "0":
                record(trace, pending[0], pending[1])
            pending = None
            continue
    if pending is not None:
        unknown.append(f"{trace.parent.name}/{trace.name}: unresolved execve for {pending[0]}")
if "bash" not in observed:
    raise SystemExit("no successful CLI shell execve observed")
for item in unknown:
    print(f"UNKNOWN {item}")
for provider in sorted(observed):
    print(f"PROVIDER {provider}")
PY
    )
    parser_rc=$?
    set +e
    if [[ $parser_rc -ne 0 ]]; then
        fail "B26 execve inventory parser failed closed (rc=$parser_rc, output=${result//$'\n'/; })"
        return
    fi
    unknown=$(grep '^UNKNOWN ' <<<"$result" || true)
    if [[ -z $unknown ]]; then
        pass "B26 exercised CLI paths execute only the closed external-command inventory"
    else
        fail "B26 exercised CLI path ran undeclared executable(s): ${unknown//$'\n'/; }"
    fi

    providers=$(sed -n 's/^PROVIDER //p' <<<"$result")
    for provider in bash coreutils grep sed mawk systemd util-linux libc-bin login procps \
        sysvinit-utils psmisc; do
        grep -Fxq -- "$provider" <<<"$providers" || continue
        metadata=$(/usr/bin/dpkg-query -W -f='${Essential}|${Priority}' "$provider" \
            2>/dev/null || true)
        essential=${metadata%%|*}
        priority=${metadata#*|}
        if [[ $essential == yes || $priority == required || $priority == important ]]; then
            pass "B26 observed $provider command is justified by Debian Essential/base ownership"
        else
            fail "B26 observed $provider command lacks Essential/base justification ($metadata)"
        fi
    done
    for provider in sudo-runtime netcat-openbsd-runtime; do
        if grep -Fxq -- "$provider" <<<"$providers"; then
            pass "B26 observed $provider command is covered by the exact runtime dependency contract"
        fi
    done
    if grep -Fxq -- python3-test-hang-fixture <<<"$providers"; then
        pass "B26 python3 execve matches only the exact uncooperative systemctl test fixture"
    fi
}

restore_contract_version() {
    if [[ $VERSION_VALID -eq 1 ]]; then
        cp -- "$REPO_ROOT/VERSION" "$tmp_root/VERSION"
        chmod 0644 "$tmp_root/VERSION"
    else
        rm -f -- "$tmp_root/VERSION"
    fi
}

run_basic_commands() {
    local command
    reset_case
    run_cli version --version
    if [[ $VERSION_VALID -eq 1 ]]; then
        assert_zero "$CLI_RC" "B26 --version"
        if [[ $(plain_output <<<"$CLI_OUTPUT") == "punto $EXPECTED_VERSION" ]] && \
           cli_output_is_exact "punto $EXPECTED_VERSION"; then
            pass "B26 --version is the exact canonical line"
        else
            fail "B26 --version output is '$CLI_OUTPUT'"
        fi
    else
        assert_nonzero "$CLI_RC" "B26 --version fails when repository VERSION is missing"
        assert_error_category invalid-configuration "B26 missing VERSION"
    fi
    assert_no_control_calls "B26 --version"
    assert_privilege_dropped "B26 --version"
    assert_bounded "$CLI_RC" "$CLI_DURATION_MS" 500 "B26 --version is bounded"

    reset_case
    rm -f -- "$tmp_root/VERSION"
    run_cli version-file-missing --version
    assert_nonzero "$CLI_RC" "B26 --version rejects a missing VERSION file"
    assert_error_category invalid-configuration "B26 --version missing VERSION"
    assert_no_control_calls "B26 --version missing VERSION"

    reset_case
    printf 'not a version\nsecond line\n' >"$tmp_root/VERSION"
    run_cli version-file-malformed --version
    assert_nonzero "$CLI_RC" "B26 --version rejects malformed multi-line VERSION"
    assert_error_category invalid-configuration "B26 --version malformed VERSION"
    assert_not_contains "$CLI_OUTPUT" 'not a version' \
        "B26 malformed VERSION content is not reflected"
    assert_no_control_calls "B26 --version malformed VERSION"

    reset_case
    /usr/bin/python3 - "$tmp_root/VERSION" <<'PY'
import pathlib
import sys

pathlib.Path(sys.argv[1]).write_bytes(b"2.8\0.5\n")
PY
    run_cli version-file-nul --version
    assert_nonzero "$CLI_RC" "B26 --version rejects a VERSION file containing NUL"
    assert_error_category invalid-configuration "B26 --version NUL VERSION"
    assert_no_control_calls "B26 --version NUL VERSION"
    restore_contract_version

    reset_case
    run_cli help --help
    assert_zero "$CLI_RC" "B26 --help"
    for command in start stop restart status; do
        assert_matches "$CLI_OUTPUT" "(^|[[:space:]])$command([[:space:]]|$)" "B26 help documents $command"
    done
    assert_contains "$CLI_OUTPUT" '--version' "B26 help documents --version"
    assert_contains "$CLI_OUTPUT" 'text mutations are disabled' \
        "B26 help reports the 2.8.6 safety-mode capability"
    assert_no_control_calls "B26 --help"
    assert_bounded "$CLI_RC" "$CLI_DURATION_MS" 500 "B26 --help is bounded"

    reset_case
    run_cli usage-error definitely-unknown
    assert_nonzero "$CLI_RC" "B26 unknown command"
    assert_error_category usage-error "B26 unknown command"
    assert_no_control_calls "B26 unknown command"
    assert_bounded "$CLI_RC" "$CLI_DURATION_MS" 500 "B26 unknown command is bounded"

    reset_case
    run_cli usage-no-args
    assert_nonzero "$CLI_RC" "B26 missing command"
    assert_error_category usage-error "B26 missing command"
    assert_no_requests "B26 missing command"
    assert_no_control_calls "B26 missing command"
    assert_no_pid_or_undeclared_calls "B26 missing command"
    assert_bounded "$CLI_RC" "$CLI_DURATION_MS" 500 "B26 missing command is bounded"

    for command in start stop restart status help --help --version; do
        reset_case
        run_cli "usage-extra-${command//[^a-zA-Z0-9]/_}" "$command" unexpected-extra
        assert_nonzero "$CLI_RC" "B26 $command rejects extra arguments"
        assert_error_category usage-error "B26 $command extra argument"
        assert_no_requests "B26 $command extra argument"
        assert_no_control_calls "B26 $command extra argument"
        assert_no_pid_or_undeclared_calls "B26 $command extra argument"
        assert_bounded "$CLI_RC" "$CLI_DURATION_MS" 500 \
            "B26 $command extra-argument rejection is bounded"
    done
}

verify_fixture_permissions
run_static_safety_gate
run_basic_commands
run_stats_grammar_matrix
run_transport_error_matrix
run_start_success_and_idempotency
run_start_failure_matrix
run_restart_matrix
run_stop_matrix
run_timeout_validation_matrix
run_execve_inventory_gate

printf '\nCLI contract: %d checks, %d failure(s)\n' "$checks" "$failures"
[[ $failures -eq 0 ]]
