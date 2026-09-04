#!/usr/bin/env bash

set -u -o pipefail

TRAY=${PUNTO_TRAY:-/usr/bin/punto-tray}
TRAY_UNIT=${PUNTO_TRAY_UNIT:-punto-tray.service}
UDEVMON_SERVICE=${PUNTO_UDEVMON_SERVICE:-udevmon}
SOCKET=${PUNTO_SOCKET:-/var/run/punto.sock}
VERSION_FILE=${PUNTO_VERSION_FILE:-/usr/share/punto-switcher/VERSION}

IPC_TIMEOUT_MS=${PUNTO_IPC_TIMEOUT_MS-1000}
COMMAND_TIMEOUT_MS=${PUNTO_COMMAND_TIMEOUT_MS-15000}
START_TIMEOUT_MS=${PUNTO_START_TIMEOUT_MS-12000}
STOP_TIMEOUT_MS=${PUNTO_STOP_TIMEOUT_MS-5000}
POLL_INTERVAL_MS=${PUNTO_POLL_INTERVAL_MS-100}
TIMEOUT_KILL_AFTER=0.200s
MUTATION_CLEANUP_GRACE=0.400s
COMMAND_WATCHDOG_MARGIN_MS=2000

IPC_DURATION=""
COMMAND_DURATION=""
COMMAND_WAIT=""
POLL_DURATION=""
IPC_RESULT=""
IPC_ERROR=""
COMMAND_ERROR=""
BACKEND_ERROR=""
TRAY_ERROR=""

emit_error() {
    printf 'ERROR %s\n' "$1"
}

valid_milliseconds() {
    local value=$1 minimum=$2
    [[ $value =~ ^[0-9]+$ ]] || return 1
    [[ ${#value} -le 5 ]] || return 1
    ((10#$value >= minimum && 10#$value <= 60000))
}

format_seconds() {
    local value=$1 destination=$2
    printf -v "$destination" '%d.%03ds' \
        "$((10#$value / 1000))" "$((10#$value % 1000))"
}

validate_configuration() {
    [[ $UDEVMON_SERVICE == udevmon ||
       $UDEVMON_SERVICE == udevmon.service ]] || return 1
    [[ $TRAY_UNIT == punto-tray.service ]] || return 1
    valid_milliseconds "$IPC_TIMEOUT_MS" 10 || return 1
    valid_milliseconds "$COMMAND_TIMEOUT_MS" 10 || return 1
    valid_milliseconds "$START_TIMEOUT_MS" 10 || return 1
    valid_milliseconds "$STOP_TIMEOUT_MS" 10 || return 1
    valid_milliseconds "$POLL_INTERVAL_MS" 1 || return 1

    format_seconds "$IPC_TIMEOUT_MS" IPC_DURATION
    format_seconds "$COMMAND_TIMEOUT_MS" COMMAND_DURATION
    # Leave room for timeout's TERM-to-KILL grace and worker result delivery.
    format_seconds \
        "$((10#$COMMAND_TIMEOUT_MS + COMMAND_WATCHDOG_MARGIN_MS))" COMMAND_WAIT
    COMMAND_WAIT=${COMMAND_WAIT%s}
    format_seconds "$POLL_INTERVAL_MS" POLL_DURATION
    POLL_DURATION=${POLL_DURATION%s}
}

load_version() {
    local byte_count line_count payload

    [[ -f $VERSION_FILE && ! -L $VERSION_FILE ]] || return 1
    byte_count=$(wc -c <"$VERSION_FILE" 2>/dev/null) || return 1
    line_count=$(wc -l <"$VERSION_FILE" 2>/dev/null) || return 1
    [[ $byte_count =~ ^[0-9]+$ && $line_count == 1 ]] || return 1
    ((byte_count >= 2 && byte_count <= 128)) || return 1
    if LC_ALL=C grep -a -q '[^0-9.]' "$VERSION_FILE"; then
        return 1
    fi
    payload=$(<"$VERSION_FILE")
    ((${#payload} + 1 == byte_count)) || return 1
    [[ $payload =~ ^[0-9]+([.][0-9]+){2}$ ]] || return 1
    printf '%s\n' "$payload"
}

valid_uint64() {
    local value=$1 maximum=18446744073709551615
    [[ $value == 0 || $value =~ ^[1-9][0-9]*$ ]] || return 1
    ((${#value} < ${#maximum})) && return 0
    ((${#value} == ${#maximum})) || return 1
    # Compare as text to avoid signed arithmetic overflow.
    # shellcheck disable=SC2071
    [[ $value == "$maximum" || $value < "$maximum" ]]
}

validate_stats_line() {
    local line=$1 index name value
    local -a parts names

    names=(
        x11_health analysis_health input_health x11_last_progress_ms
        analysis_last_progress_ms input_last_progress_ms analysis_outstanding
        input_in_flight log_dropped text_mutation enabled configured_enabled
        config_pending config_generation config_result analyzed need_switch corrections pending_words
        ready_results worker_threads daemon_peers analysis_mode control_plane
        queued_tasks avg_queue_us avg_analysis_us avg_macro_us avg_tail_len
    )
    read -r -a parts <<<"$line"
    [[ ${#parts[@]} -eq 30 && ${parts[0]} == OK ]] || return 1
    [[ $line == "${parts[*]}" ]] || return 1

    for ((index = 0; index < ${#names[@]}; ++index)); do
        name=${parts[$((index + 1))]%%=*}
        value=${parts[$((index + 1))]#*=}
        [[ $name == "${names[$index]}" && $value != "${parts[$((index + 1))]}" ]] || return 1
        case $name in
            x11_health|analysis_health|input_health)
                [[ $value == ready || $value == degraded || $value == failed ]] || return 1
                ;;
            input_in_flight|configured_enabled|config_pending)
                [[ $value == 0 || $value == 1 ]] || return 1
                ;;
            text_mutation)
                [[ $value == disabled ]] || return 1
                ;;
            enabled)
                [[ $value == 0 ]] || return 1
                ;;
            config_result)
                [[ $value == none || $value == ok || $value == error ]] || return 1
                ;;
            analysis_mode)
                [[ $value == auto || $value == fixed ]] || return 1
                ;;
            control_plane)
                [[ $value == primary || $value == secondary ]] || return 1
                ;;
            *)
                valid_uint64 "$value" || return 1
                ;;
        esac
    done
}

query_status() {
    local capture_dir response_fifo error_fifo response_file error_file
    local response_reader error_reader response_reader_rc error_reader_rc
    local rc byte_count line_count payload diagnostic

    IPC_RESULT=""
    IPC_ERROR=""
    capture_dir=$(mktemp -d "${TMPDIR:-/tmp}/punto-ipc.XXXXXX" 2>/dev/null) || {
        IPC_ERROR=unavailable
        return 1
    }
    response_fifo=$capture_dir/response.fifo
    error_fifo=$capture_dir/error.fifo
    response_file=$capture_dir/response
    error_file=$capture_dir/error
    if ! mkfifo -m 0600 -- "$response_fifo" "$error_fifo"; then
        rm -f -- "$response_fifo" "$error_fifo" "$response_file" "$error_file"
        rmdir -- "$capture_dir" 2>/dev/null || true
        IPC_ERROR=unavailable
        return 1
    fi

    head -c 4097 <"$response_fifo" >"$response_file" &
    response_reader=$!
    head -c 4097 <"$error_fifo" >"$error_file" &
    error_reader=$!

    printf 'STATS\n' | timeout --signal=TERM \
        --kill-after="$TIMEOUT_KILL_AFTER" "$IPC_DURATION" nc -U "$SOCKET" \
        >"$response_fifo" 2>"$error_fifo"
    rc=${PIPESTATUS[1]}
    wait "$response_reader"
    response_reader_rc=$?
    wait "$error_reader"
    error_reader_rc=$?
    rm -f -- "$response_fifo" "$error_fifo"

    byte_count=$(wc -c <"$response_file" 2>/dev/null) || byte_count=""
    if [[ ! $byte_count =~ ^[0-9]+$ ]] || ((byte_count > 4096)); then
        rm -f -- "$response_file" "$error_file"
        rmdir -- "$capture_dir" 2>/dev/null || true
        IPC_ERROR=protocol-error
        return 1
    fi
    if ((response_reader_rc != 0 || error_reader_rc != 0)); then
        rm -f -- "$response_file" "$error_file"
        rmdir -- "$capture_dir" 2>/dev/null || true
        IPC_ERROR=unavailable
        return 1
    fi
    if ((rc != 0)); then
        diagnostic=$(<"$error_file")
        rm -f -- "$response_file" "$error_file"
        rmdir -- "$capture_dir" 2>/dev/null || true
        if ((rc == 124 || rc == 137)); then
            IPC_ERROR=timeout
        elif [[ $diagnostic == *'Permission denied'* ]]; then
            IPC_ERROR=denied
        else
            IPC_ERROR=unavailable
        fi
        return 1
    fi
    rm -f -- "$error_file"

    line_count=$(wc -l <"$response_file" 2>/dev/null) || {
        rm -f -- "$response_file"
        rmdir -- "$capture_dir" 2>/dev/null || true
        IPC_ERROR=protocol-error
        return 1
    }
    if [[ ! $byte_count =~ ^[0-9]+$ || $line_count != 1 ]] || \
       ((byte_count < 2 || byte_count > 4096)) || \
       LC_ALL=C grep -a -q '[^ -~]' "$response_file"; then
        rm -f -- "$response_file"
        rmdir -- "$capture_dir" 2>/dev/null || true
        IPC_ERROR=protocol-error
        return 1
    fi
    payload=$(<"$response_file")
    rm -f -- "$response_file"
    rmdir -- "$capture_dir" 2>/dev/null || true
    if ((${#payload} + 1 != byte_count)); then
        IPC_ERROR=protocol-error
        return 1
    fi
    if [[ $payload == ERROR\ * ]]; then
        IPC_ERROR=daemon-error
        return 1
    fi
    if ! validate_stats_line "$payload"; then
        IPC_ERROR=protocol-error
        return 1
    fi
    IPC_RESULT=$payload
}

backend_active() {
    local rc
    BACKEND_ERROR=""
    timeout --signal=TERM --kill-after="$TIMEOUT_KILL_AFTER" \
        "$IPC_DURATION" systemctl is-active --quiet -- "$UDEVMON_SERVICE" \
        >/dev/null 2>&1
    rc=$?
    case $rc in
        0) return 0 ;;
        3) return 1 ;;
        124|137) BACKEND_ERROR=service-timeout ;;
        *) BACKEND_ERROR=service-error ;;
    esac
    return 2
}

run_mutation_worker() {
    local action=$1 command_pid="" rc
    shift

    # shellcheck disable=SC2317 # Invoked indirectly by the signal trap below.
    terminate_worker() {
        trap - TERM HUP INT
        if [[ $command_pid =~ ^[1-9][0-9]*$ ]]; then
            { kill -TERM -- "-$command_pid" || true; } 2>/dev/null
            sleep "$TIMEOUT_KILL_AFTER"
            { kill -KILL -- "-$command_pid" || true; } 2>/dev/null
            wait "$command_pid" 2>/dev/null || true
        fi
        exit 124
    }
    trap terminate_worker TERM HUP INT

    setsid timeout --signal=TERM --kill-after="$TIMEOUT_KILL_AFTER" \
        "$COMMAND_DURATION" sudo -n systemctl "$action" "$@" \
        >/dev/null 2>&1 &
    command_pid=$!
    wait "$command_pid" 2>/dev/null
    rc=$?
    trap - TERM HUP INT
    printf '%s\n' "$rc"
}

run_service_command() {
    local action=$1 mutation_rc result_fd mutation_job
    shift
    COMMAND_ERROR=""

    coproc PUNTO_COMMAND { run_mutation_worker "$action" "$@"; }
    result_fd=${PUNTO_COMMAND[0]}
    mutation_job=$PUNTO_COMMAND_PID
    if IFS= read -r -t "$COMMAND_WAIT" mutation_rc <&"$result_fd"; then
        wait "$mutation_job" 2>/dev/null || true
        if [[ $mutation_rc == 0 ]]; then
            return 0
        fi
        if [[ $mutation_rc == 124 || $mutation_rc == 137 ]]; then
            COMMAND_ERROR=service-timeout
        else
            COMMAND_ERROR=service-error
        fi
        return 1
    fi
    { kill -TERM "$mutation_job" || true; } 2>/dev/null
    sleep "$MUTATION_CLEANUP_GRACE"
    { kill -KILL "$mutation_job" || true; } 2>/dev/null
    wait "$mutation_job" 2>/dev/null || true
    COMMAND_ERROR=service-timeout
    return 1
}

wait_for_socket() {
    local started now deadline
    started=$(date +%s%3N) || return 1
    deadline=$((10#$started + 10#$START_TIMEOUT_MS))
    while [[ ! -S $SOCKET ]]; do
        now=$(date +%s%3N) || return 1
        ((10#$now >= deadline)) && break
        sleep "$POLL_DURATION"
    done
}

wait_for_service_inactive() {
    local started now deadline rc
    started=$(date +%s%3N) || return 1
    deadline=$((10#$started + 10#$STOP_TIMEOUT_MS))
    while :; do
        backend_active
        rc=$?
        ((rc == 1)) && return 0
        ((rc == 2)) && return 2
        now=$(date +%s%3N) || return 1
        ((10#$now >= deadline)) && return 1
        sleep "$POLL_DURATION"
    done
}

run_tray_manager_command() {
    local action=$1 rc
    TRAY_ERROR=""
    timeout --signal=TERM --kill-after="$TIMEOUT_KILL_AFTER" \
        "$COMMAND_DURATION" systemctl --user "$action" -- "$TRAY_UNIT" \
        >/dev/null 2>&1
    rc=$?
    case $rc in
        0) return 0 ;;
        124|137) TRAY_ERROR=tray-timeout ;;
        *) TRAY_ERROR=tray-unavailable ;;
    esac
    return 1
}

tray_is_active() {
    local rc
    timeout --signal=TERM --kill-after="$TIMEOUT_KILL_AFTER" \
        "$IPC_DURATION" systemctl --user is-active --quiet -- "$TRAY_UNIT" \
        >/dev/null 2>&1
    rc=$?
    case $rc in
        0) return 0 ;;
        3) return 1 ;;
        124|137) TRAY_ERROR=tray-timeout ;;
        *) TRAY_ERROR=tray-unavailable ;;
    esac
    return 2
}

wait_for_tray_active() {
    local started now deadline rc consecutive=0
    started=$(date +%s%3N) || return 1
    deadline=$((10#$started + 10#$START_TIMEOUT_MS))
    while :; do
        tray_is_active
        rc=$?
        if ((rc == 0)); then
            consecutive=$((consecutive + 1))
            ((consecutive >= 3)) && return 0
        elif ((rc == 2)); then
            return 1
        else
            consecutive=0
        fi
        now=$(date +%s%3N) || return 1
        if ((10#$now >= deadline)); then
            TRAY_ERROR=tray-unavailable
            return 1
        fi
        sleep "$POLL_DURATION"
    done
}

wait_for_tray_inactive() {
    local started now deadline rc
    started=$(date +%s%3N) || return 1
    deadline=$((10#$started + 10#$STOP_TIMEOUT_MS))
    while :; do
        tray_is_active
        rc=$?
        ((rc == 1)) && return 0
        ((rc == 2)) && return 1
        now=$(date +%s%3N) || return 1
        if ((10#$now >= deadline)); then
            TRAY_ERROR=tray-timeout
            return 1
        fi
        sleep "$POLL_DURATION"
    done
}

reload_tray_manager() {
    local rc
    TRAY_ERROR=""
    timeout --signal=TERM --kill-after="$TIMEOUT_KILL_AFTER" \
        "$COMMAND_DURATION" systemctl --user daemon-reload >/dev/null 2>&1
    rc=$?
    case $rc in
        0) return 0 ;;
        124|137) TRAY_ERROR=tray-timeout ;;
        *) TRAY_ERROR=tray-unavailable ;;
    esac
    return 1
}

ensure_tray_started() {
    [[ -x $TRAY ]] || return 0
    [[ -n ${DISPLAY:-} || -n ${WAYLAND_DISPLAY:-} ]] || return 0
    reload_tray_manager && run_tray_manager_command start &&
        wait_for_tray_active
}

ensure_tray_restarted() {
    [[ -x $TRAY ]] || return 0
    [[ -n ${DISPLAY:-} || -n ${WAYLAND_DISPLAY:-} ]] || return 0
    reload_tray_manager && run_tray_manager_command restart &&
        wait_for_tray_active
}

ensure_tray_stopped() {
    [[ -x $TRAY ]] || return 0
    run_tray_manager_command stop && wait_for_tray_inactive
}

report_tray_warning() {
    printf 'WARN %s\n' "${TRAY_ERROR:-tray-unavailable}"
}

report_start_failure() {
    local category=$1 rollback=$2 warning="" rc
    if [[ $rollback == yes ]]; then
        run_service_command stop "$UDEVMON_SERVICE" || true
        backend_active
        rc=$?
        ((rc == 0)) && warning='WARN rollback-incomplete: service-active'
        ((rc == 2)) && warning='WARN rollback-incomplete: service-state-unknown'
    fi
    emit_error "$category"
    [[ -z $warning ]] || printf '%s\n' "$warning"
    return 1
}

start_service() {
    local started_here=no backend_rc

    backend_active
    backend_rc=$?
    if ((backend_rc == 0)); then
        if [[ ! -S $SOCKET ]]; then
            emit_error unavailable
            return 1
        fi
        if ! query_status; then
            emit_error "$IPC_ERROR"
            return 1
        fi
        ensure_tray_started || report_tray_warning
        return 0
    elif ((backend_rc == 1)); then
        if ! run_service_command start "$UDEVMON_SERVICE"; then
            emit_error "$COMMAND_ERROR"
            return 1
        fi
        started_here=yes
    else
        emit_error "$BACKEND_ERROR"
        return 1
    fi

    wait_for_socket || true
    if ! query_status; then
        report_start_failure "$IPC_ERROR" "$started_here"
        return 1
    fi
    ensure_tray_started || report_tray_warning
}

restart_service() {
    if ! run_service_command restart "$UDEVMON_SERVICE"; then
        emit_error "$COMMAND_ERROR"
        return 1
    fi
    wait_for_socket || true
    if ! query_status; then
        emit_error "$IPC_ERROR"
        return 1
    fi
    ensure_tray_restarted || report_tray_warning
}

stop_service() {
    local backend_rc inactive_rc
    ensure_tray_stopped || report_tray_warning
    backend_active
    backend_rc=$?
    ((backend_rc == 1)) && return 0
    if ((backend_rc == 2)); then
        emit_error "$BACKEND_ERROR"
        return 1
    fi

    if ! run_service_command stop "$UDEVMON_SERVICE"; then
        emit_error "$COMMAND_ERROR"
        return 1
    fi
    wait_for_service_inactive
    inactive_rc=$?
    ((inactive_rc == 0)) && return 0
    if ((inactive_rc == 2)); then
        emit_error "$BACKEND_ERROR"
        return 1
    fi
    if ! run_service_command kill --kill-who=all --signal=TERM "$UDEVMON_SERVICE"; then
        emit_error "$COMMAND_ERROR"
        return 1
    fi
    backend_active
    backend_rc=$?
    if ((backend_rc == 0)); then
        emit_error service-error
        return 1
    fi
    if ((backend_rc == 2)); then
        emit_error "$BACKEND_ERROR"
        return 1
    fi
}

show_help() {
    local version=$1
    printf '%s\n' \
        'Usage: punto <command>' \
        "Version: $version" \
        '' \
        'Commands:' \
        '  start      Start the backend and tray' \
        '  stop       Stop the tray and backend' \
        '  restart    Restart the backend and tray' \
        '  status     Print the daemon STATS line' \
        '  help       Show this help' \
        '  --version  Print the installed version' \
        '' \
        "Safety mode $version: analysis and passthrough are active; automatic" \
        'and manual text mutations are disabled before key dispatch.'
}

main() {
    local command=${1:-} version
    if ! validate_configuration; then
        emit_error invalid-configuration
        return 1
    fi
    if (($# != 1)); then
        emit_error usage-error
        return 1
    fi

    case $command in
        start) start_service ;;
        stop) stop_service ;;
        restart) restart_service ;;
        status)
            if query_status; then
                printf '%s\n' "$IPC_RESULT"
            else
                emit_error "$IPC_ERROR"
                return 1
            fi
            ;;
        help|--help|-h)
            version=$(load_version) || {
                emit_error invalid-configuration
                return 1
            }
            show_help "$version"
            ;;
        --version)
            version=$(load_version) || {
                emit_error invalid-configuration
                return 1
            }
            printf 'punto %s\n' "$version"
            ;;
        *)
            emit_error usage-error
            return 1
            ;;
    esac
}

main "$@"
