#!/bin/bash
# =============================================================================
# Сборка deb-пакета Punto Switcher v2.7.4 (C++20 версия)
# =============================================================================
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

VERSION="2.7.4"
PACKAGE_NAME="punto-switcher"
BUILD_DIR="build-deb"
CPP_BUILD_DIR="cpp/build"

# Цвета для вывода
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

INSTALL_AFTER_BUILD=false
for arg in "$@"; do
    if [[ "$arg" == "--install" ]]; then
        INSTALL_AFTER_BUILD=true
    fi
done

restart_tray() {
    if [[ "$BUILD_TRAY" != "true" ]]; then
        return
    fi

    # Останавливаем старый tray (если запущен)
    if pgrep -x punto-tray >/dev/null 2>&1; then
        pkill -x punto-tray 2>/dev/null || true
        sleep 0.2
    fi

    # Запускаем tray только если есть GUI окружение
    if [[ -n "${DISPLAY:-}" || -n "${WAYLAND_DISPLAY:-}" ]]; then
        /usr/local/bin/punto-tray >/dev/null 2>&1 &
    else
        echo -e "${YELLOW}   DISPLAY/WAYLAND_DISPLAY не найден — пропускаем автозапуск punto-tray${NC}"
    fi
}

echo "=== Сборка Punto Switcher v${VERSION} ==="

# -----------------------------------------------------------------------------
# Этап 1: Проверка и установка зависимостей
# -----------------------------------------------------------------------------
echo "[1/6] Проверка зависимостей..."

# Обязательные пакеты для сборки
# libhunspell-dev для поддержки словарей (падежи, склонения, время)
BUILD_DEPS=("build-essential" "cmake" "pkg-config" "libx11-dev" "libhunspell-dev")

# Пакеты для сборки tray-приложения (Ayatana для Ubuntu 22.04+)
TRAY_DEPS=("libgtk-3-dev" "libayatana-appindicator3-dev")

# Опциональные пакеты (словари для автопереключения)
# hunspell-en-us, hunspell-ru — базовые словари
# wamerican-huge — расширенный английский словарь (~300k слов)
OPTIONAL_DEPS=("hunspell" "hunspell-en-us" "hunspell-ru" "wamerican-huge")

# Функция проверки установленного пакета
check_package() {
    dpkg -s "$1" &>/dev/null
}

APT_UPDATED=0
apt_update_once() {
    if [[ $APT_UPDATED -eq 0 ]]; then
        sudo apt-get update -qq
        APT_UPDATED=1
    fi
}

apt_install() {
    if [[ $# -eq 0 ]]; then
        return 0
    fi
    apt_update_once
    sudo apt-get install -y "$@"
}

# Проверяем обязательные зависимости
MISSING_BUILD_DEPS=()
for pkg in "${BUILD_DEPS[@]}"; do
    if ! check_package "$pkg"; then
        MISSING_BUILD_DEPS+=("$pkg")
    fi
done

# Проверяем опциональные зависимости
MISSING_OPTIONAL_DEPS=()
for pkg in "${OPTIONAL_DEPS[@]}"; do
    if ! check_package "$pkg"; then
        MISSING_OPTIONAL_DEPS+=("$pkg")
    fi
done

# Устанавливаем обязательные зависимости
if [[ ${#MISSING_BUILD_DEPS[@]} -gt 0 ]]; then
    echo -e "${YELLOW}   Отсутствуют пакеты для сборки: ${MISSING_BUILD_DEPS[*]}${NC}"
    echo "   Установка..."
    apt_install "${MISSING_BUILD_DEPS[@]}"
    echo -e "${GREEN}   Зависимости для сборки установлены${NC}"
else
    echo -e "${GREEN}   Все зависимости для сборки установлены${NC}"
fi

# Проверяем зависимости для tray-приложения
MISSING_TRAY_DEPS=()
for pkg in "${TRAY_DEPS[@]}"; do
    if ! check_package "$pkg"; then
        MISSING_TRAY_DEPS+=("$pkg")
    fi
done

BUILD_TRAY=true
if [[ ${#MISSING_TRAY_DEPS[@]} -gt 0 ]]; then
    echo -e "${YELLOW}   Отсутствуют пакеты для tray: ${MISSING_TRAY_DEPS[*]}${NC}"
    echo "   Установка..."

    if apt_install "${MISSING_TRAY_DEPS[@]}"; then
        echo -e "${GREEN}   Зависимости для tray установлены${NC}"
    else
        echo -e "${YELLOW}   Предупреждение: не удалось установить зависимости для tray. punto-tray не будет собран.${NC}" >&2
        BUILD_TRAY=false
    fi
fi

# Устанавливаем опциональные зависимости (словари)
if [[ ${#MISSING_OPTIONAL_DEPS[@]} -gt 0 ]]; then
    echo -e "${YELLOW}   Опциональные пакеты (словари): ${MISSING_OPTIONAL_DEPS[*]}${NC}"
    echo "   Установка..."

    if apt_install "${MISSING_OPTIONAL_DEPS[@]}"; then
        echo -e "${GREEN}   Словари установлены${NC}"
        MISSING_OPTIONAL_DEPS=()
    else
        echo -e "${YELLOW}   Предупреждение: не удалось установить словари. Автопереключение может быть менее точным.${NC}" >&2
    fi
fi

# -----------------------------------------------------------------------------
# Этап 2: Сборка C++ бинарника
# -----------------------------------------------------------------------------
echo "[2/6] Сборка C++ бинарника..."

mkdir -p cpp/build
cd cpp/build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
cd "$SCRIPT_DIR"

# Для IDE/clangd: копируем compile_commands.json в корень репозитория
if [[ -f "cpp/build/compile_commands.json" ]]; then
    cp cpp/build/compile_commands.json "$SCRIPT_DIR/compile_commands.json"
fi

# Проверяем, что бинарник собрался
if [[ ! -f "cpp/build/punto" ]]; then
    echo "Ошибка: бинарник cpp/build/punto не найден!"
    exit 1
fi

echo -e "${GREEN}   punto собран${NC}: $(file cpp/build/punto | cut -d: -f2)"

# Проверяем tray-приложение
if [[ "$BUILD_TRAY" == "true" ]] && [[ -f "cpp/build/punto-tray" ]]; then
    echo -e "${GREEN}   punto-tray собран${NC}: $(file cpp/build/punto-tray | cut -d: -f2)"
else
    BUILD_TRAY=false
    echo -e "${YELLOW}   punto-tray не собран (отсутствуют зависимости GTK3/AppIndicator)${NC}"
fi

# -----------------------------------------------------------------------------
# Этап 3: Подготовка структуры пакета
# -----------------------------------------------------------------------------
echo "[3/6] Подготовка структуры пакета..."

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR/DEBIAN"
mkdir -p "$BUILD_DIR/usr/local/bin"
mkdir -p "$BUILD_DIR/etc/punto"
mkdir -p "$BUILD_DIR/usr/share/punto-switcher"
mkdir -p "$BUILD_DIR/usr/share/punto-switcher/sounds"
mkdir -p "$BUILD_DIR/etc/xdg/autostart"

# Копируем бинарник daemon
cp cpp/build/punto "$BUILD_DIR/usr/local/bin/punto-daemon"
chmod 755 "$BUILD_DIR/usr/local/bin/punto-daemon"

# Копируем CLI wrapper
cp punto-cli.sh "$BUILD_DIR/usr/local/bin/punto"
chmod 755 "$BUILD_DIR/usr/local/bin/punto"

# Копируем tray-приложение и desktop entry
if [[ "$BUILD_TRAY" == "true" ]]; then
    cp cpp/build/punto-tray "$BUILD_DIR/usr/local/bin/"
    chmod 755 "$BUILD_DIR/usr/local/bin/punto-tray"
    cp punto-tray.desktop "$BUILD_DIR/etc/xdg/autostart/"
    echo -e "${GREEN}   punto-tray включён в пакет${NC}"
fi

# Копируем конфигурацию и udevmon для postinst
cp config.yaml "$BUILD_DIR/usr/share/punto-switcher/"
cp udevmon.yaml "$BUILD_DIR/usr/share/punto-switcher/"
cp config.yaml "$BUILD_DIR/etc/punto/config.yaml.new"

# Копируем звуки переключения раскладки
cp cpp/src/sound/en_ru.wav "$BUILD_DIR/usr/share/punto-switcher/sounds/"
cp cpp/src/sound/ru_en.wav "$BUILD_DIR/usr/share/punto-switcher/sounds/"

# Копируем DEBIAN файлы
cp DEBIAN/control "$BUILD_DIR/DEBIAN/"
cp DEBIAN/postinst "$BUILD_DIR/DEBIAN/" 2>/dev/null || true
cp DEBIAN/prerm "$BUILD_DIR/DEBIAN/" 2>/dev/null || true

# Устанавливаем права на скрипты
chmod 755 "$BUILD_DIR/DEBIAN/"*inst 2>/dev/null || true
chmod 755 "$BUILD_DIR/DEBIAN/"*rm 2>/dev/null || true

# Обновляем версию в control
sed -i "s/^Version:.*/Version: ${VERSION}/" "$BUILD_DIR/DEBIAN/control"

# -----------------------------------------------------------------------------
# Этап 4: Проверка runtime-зависимостей
# -----------------------------------------------------------------------------
echo "[4/6] Проверка runtime-зависимостей..."

# Зависимости для работы программы
RUNTIME_DEPS=("interception-tools" "libx11-6" "xsel")

# Дополнительные runtime-зависимости для tray
if [[ "$BUILD_TRAY" == "true" ]]; then
    RUNTIME_DEPS+=("libgtk-3-0" "libayatana-appindicator3-1")
fi
MISSING_RUNTIME_DEPS=()

for pkg in "${RUNTIME_DEPS[@]}"; do
    if ! check_package "$pkg"; then
        MISSING_RUNTIME_DEPS+=("$pkg")
    fi
done

if [[ ${#MISSING_RUNTIME_DEPS[@]} -gt 0 ]]; then
    echo -e "${YELLOW}   Отсутствуют runtime-пакеты: ${MISSING_RUNTIME_DEPS[*]}${NC}"
    echo "   Эти пакеты нужны для работы punto-switcher. Установка..."
    apt_install "${MISSING_RUNTIME_DEPS[@]}"
    echo -e "${GREEN}   Runtime-зависимости установлены${NC}"
else
    echo -e "${GREEN}   Все runtime-зависимости установлены${NC}"
fi

# -----------------------------------------------------------------------------
# Этап 5: Сборка пакета
# -----------------------------------------------------------------------------
echo "[5/6] Сборка deb-пакета..."

OUTPUT_DEB="${PACKAGE_NAME}_${VERSION}_amd64.deb"
dpkg-deb --build --root-owner-group "$BUILD_DIR" "$OUTPUT_DEB"

# -----------------------------------------------------------------------------
# Этап 6: Информация о пакете
# -----------------------------------------------------------------------------
echo "[6/6] Информация о пакете:"
echo ""
dpkg-deb -I "$OUTPUT_DEB"
echo ""
echo "Содержимое:"
dpkg-deb -c "$OUTPUT_DEB"
echo ""
echo -e "${GREEN}=== Готово! ===${NC}"
echo "Пакет: $OUTPUT_DEB"
echo "Размер: $(du -h "$OUTPUT_DEB" | cut -f1)"
echo ""
echo "Установка: sudo dpkg -i $OUTPUT_DEB"
echo "Удаление:  sudo dpkg -r $PACKAGE_NAME"

# -----------------------------------------------------------------------------
# Optional: установка и перезапуск tray
# -----------------------------------------------------------------------------
if [[ "$INSTALL_AFTER_BUILD" == "true" ]]; then
    echo ""
    echo "Установка пакета..."
    sudo dpkg -i "$OUTPUT_DEB"

    echo "Перезапуск punto-tray..."
    restart_tray
else
    echo ""
    read -p "Установить пакет сейчас и перезапустить tray? [Y/n] " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Nn]$ ]]; then
        sudo dpkg -i "$OUTPUT_DEB"
        restart_tray
    fi
fi

# Показываем статус словарей
if [[ ${#MISSING_OPTIONAL_DEPS[@]} -gt 0 ]]; then
    echo ""
    echo -e "${YELLOW}Примечание: для автопереключения рекомендуется установить:${NC}"
    echo "  sudo apt install ${MISSING_OPTIONAL_DEPS[*]}"
fi

# Очистка
rm -rf "$BUILD_DIR"
