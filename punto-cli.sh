#!/bin/bash
# Punto Switcher CLI - управление сервисом

set -e

DAEMON="/usr/local/bin/punto-daemon"
TRAY="/usr/local/bin/punto-tray"
UDEVMON_SERVICE="udevmon"

# Цвета для вывода
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

function daemon_count() {
    pgrep -x -c "punto-daemon" 2>/dev/null || true
}

function tray_pid() {
    pgrep -x "punto-tray" 2>/dev/null | head -1 || true
}

function current_login_user() {
    id -un 2>/dev/null || echo "${USER:-unknown}"
}

function current_user_has_punto_group() {
    id -nG 2>/dev/null | tr ' ' '\n' | grep -qx "punto"
}

function account_is_in_punto_group() {
    id -nG "$(current_login_user)" 2>/dev/null | tr ' ' '\n' | grep -qx "punto"
}

function backend_unit_active() {
    systemctl is-active --quiet "${UDEVMON_SERVICE}"
}

function backend_is_healthy() {
    local count
    count="$(daemon_count)"
    backend_unit_active && [[ "${count}" -gt 0 ]]
}

function wait_for_backend_healthy() {
    local timeout_seconds="${1:-10}"
    local attempt

    for ((attempt = 0; attempt < timeout_seconds; ++attempt)); do
        if backend_is_healthy; then
            return 0
        fi
        sleep 1
    done

    backend_is_healthy
}

function show_backend_diagnostics() {
    echo -e "${YELLOW}  → systemd status:${NC}"
    systemctl status "${UDEVMON_SERVICE}" --no-pager --lines=20 || true
    echo -e "${YELLOW}  → recent journal:${NC}"
    journalctl -u "${UDEVMON_SERVICE}" -n 30 --no-pager || true
}

function wait_for_tray_started() {
    local timeout_seconds="${1:-5}"
    local attempt

    for ((attempt = 0; attempt < timeout_seconds; ++attempt)); do
        if [[ -n "$(tray_pid)" ]]; then
            return 0
        fi
        sleep 1
    done

    [[ -n "$(tray_pid)" ]]
}

function start_tray() {
    if [[ ! -x "${TRAY}" ]]; then
        echo -e "${YELLOW}  → Frontend binary not installed, skipping tray start${NC}"
        return 0
    fi

    if [[ -z "${DISPLAY:-}" && -z "${WAYLAND_DISPLAY:-}" ]]; then
        echo -e "${YELLOW}  → GUI session not detected, skipping tray start${NC}"
        return 0
    fi

    if [[ -n "$(tray_pid)" ]]; then
        echo "  → Frontend already running"
        return 0
    fi

    if current_user_has_punto_group; then
        echo "  → Starting frontend (tray)..."
        nohup "${TRAY}" > /dev/null 2>&1 &
    elif account_is_in_punto_group; then
        echo "  → Starting frontend (tray) via sg punto..."
        sg punto -c "setsid ${TRAY} >/dev/null 2>&1 </dev/null &"
    else
        local login_user
        login_user="$(current_login_user)"
        echo -e "${YELLOW}  → User '${login_user}' is not in group punto; tray IPC will be unavailable.${NC}"
        echo -e "${YELLOW}    Run: sudo usermod -aG punto ${login_user} && log out/in${NC}"
        return 0
    fi

    if ! wait_for_tray_started 5; then
        echo -e "${YELLOW}  → Tray process did not stay up. Check desktop session and appindicator support.${NC}"
    fi
}

function print_status() {
    local count
    local tray_process

    count="$(daemon_count)"
    tray_process="$(tray_pid)"

    echo -e "${GREEN}━━━ Punto Switcher Status ━━━${NC}"

    # Backend (punto-daemon через udevmon)
    if backend_unit_active && [[ "${count}" -gt 0 ]]; then
        echo -e "Backend (udevmon):  ${GREEN}✓ running${NC}"
        echo -e "  Daemon processes: ${count}"
    elif backend_unit_active; then
        echo -e "Backend (udevmon):  ${YELLOW}! degraded${NC}"
        echo -e "  Daemon processes: ${count}"
    else
        echo -e "Backend (udevmon):  ${RED}✗ stopped${NC}"
    fi

    # Frontend (punto-tray)
    if [[ -n "${tray_process}" ]]; then
        echo -e "Frontend (tray):    ${GREEN}✓ running${NC}"
        echo -e "  PID: ${tray_process}"
    else
        echo -e "Frontend (tray):    ${RED}✗ stopped${NC}"
    fi

    echo -e "${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
}

function start_service() {
    echo "Starting Punto Switcher..."

    # Запуск backend через systemd
    if backend_is_healthy; then
        echo "  → Backend already running"
    elif backend_unit_active; then
        echo "  → Backend active but unhealthy, restarting udevmon..."
        sudo systemctl restart "${UDEVMON_SERVICE}"
    else
        echo "  → Starting backend (udevmon)..."
        sudo systemctl start "${UDEVMON_SERVICE}"
    fi

    if ! wait_for_backend_healthy 12; then
        echo -e "${RED}✗ Backend failed to start cleanly${NC}"
        show_backend_diagnostics
        exit 1
    fi

    # Запуск frontend
    start_tray
    
    echo -e "${GREEN}✓ Punto Switcher started${NC}"
    print_status
}

function stop_service() {
    echo "Stopping Punto Switcher..."

    # Остановка frontend
    if [[ -n "$(tray_pid)" ]]; then
        echo "  → Stopping frontend (tray)..."
        pkill -x "punto-tray" || true
        sleep 1
    fi

    # Остановка backend
    if backend_unit_active; then
        echo "  → Stopping backend (udevmon)..."
        sudo systemctl stop "${UDEVMON_SERVICE}"
        sleep 1
    fi

    # Best-effort: просим оставшиеся процессы завершиться без SIGKILL.
    if [[ "$(daemon_count)" -gt 0 ]]; then
        echo "  → Sending TERM to remaining daemon processes..."
        sudo pkill -TERM -x "punto-daemon" || true
        sleep 2
    fi

    if [[ "$(daemon_count)" -gt 0 ]]; then
        echo -e "${YELLOW}  → Внимание: часть punto-daemon всё ещё работает. Проверьте journalctl и состояние udevmon вручную.${NC}"
    fi

    echo -e "${GREEN}✓ Punto Switcher stopped${NC}"
}

function restart_service() {
    echo "Restarting Punto Switcher..."
    stop_service
    sleep 2
    start_service
}

function show_help() {
    cat << EOF
Punto Switcher CLI v2.8.4

Usage: punto <command>

Commands:
  start     Start Punto Switcher (backend + frontend)
  stop      Stop Punto Switcher
  restart   Restart Punto Switcher
  status    Show service status
  help      Show this help message

Examples:
  punto start     # Start the service
  punto status    # Check if running
  punto restart   # Reload configuration

EOF
}

# Main
case "${1:-}" in
    start)
        start_service
        ;;
    stop)
        stop_service
        ;;
    restart)
        restart_service
        ;;
    status)
        print_status
        ;;
    help|--help|-h)
        show_help
        ;;
    *)
        echo -e "${RED}Error: Unknown command '${1:-}'${NC}"
        echo ""
        show_help
        exit 1
        ;;
esac
