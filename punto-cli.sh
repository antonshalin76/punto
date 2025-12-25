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

function print_status() {
    echo -e "${GREEN}━━━ Punto Switcher Status ━━━${NC}"
    
    # Backend (punto-daemon через udevmon)
    if systemctl is-active --quiet "${UDEVMON_SERVICE}"; then
        echo -e "Backend (udevmon):  ${GREEN}✓ running${NC}"
        DAEMON_COUNT=$(ps aux | grep -c "[p]unto-daemon" || true)
        echo -e "  Daemon processes: ${DAEMON_COUNT}"
    else
        echo -e "Backend (udevmon):  ${RED}✗ stopped${NC}"
    fi
    
    # Frontend (punto-tray)
    if pgrep -f "punto-tray" > /dev/null; then
        echo -e "Frontend (tray):    ${GREEN}✓ running${NC}"
        TRAY_PID=$(pgrep -f "punto-tray" | head -1)
        echo -e "  PID: ${TRAY_PID}"
    else
        echo -e "Frontend (tray):    ${RED}✗ stopped${NC}"
    fi
    
    echo -e "${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
}

function start_service() {
    echo "Starting Punto Switcher..."
    
    # Запуск backend через systemd
    if ! systemctl is-active --quiet "${UDEVMON_SERVICE}"; then
        echo "  → Starting backend (udevmon)..."
        sudo systemctl start "${UDEVMON_SERVICE}"
        sleep 2
    else
        echo "  → Backend already running"
    fi
    
    # Запуск frontend
    if ! pgrep -f "punto-tray" > /dev/null; then
        echo "  → Starting frontend (tray)..."
        nohup "${TRAY}" > /dev/null 2>&1 &
        sleep 1
    else
        echo "  → Frontend already running"
    fi
    
    echo -e "${GREEN}✓ Punto Switcher started${NC}"
    print_status
}

function stop_service() {
    echo "Stopping Punto Switcher..."
    
    # Остановка frontend
    if pgrep -f "punto-tray" > /dev/null; then
        echo "  → Stopping frontend (tray)..."
        pkill -f "punto-tray" || true
        sleep 1
    fi
    
    # Остановка backend
    if systemctl is-active --quiet "${UDEVMON_SERVICE}"; then
        echo "  → Stopping backend (udevmon)..."
        sudo systemctl stop "${UDEVMON_SERVICE}"
        sleep 1
    fi
    
    # Убиваем оставшиеся процессы punto-daemon если есть
    if pgrep -f "punto-daemon" > /dev/null; then
        echo "  → Killing remaining daemon processes..."
        sudo pkill -9 -f "punto-daemon" || true
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
Punto Switcher CLI v2.7.3

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
