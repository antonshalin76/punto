#!/bin/bash
# =============================================================================
# Сборка deb-пакета Punto Switcher v2.1 (C++20)
# =============================================================================
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

VERSION="2.1.0"
PACKAGE_NAME="punto-switcher"
BUILD_DIR="build-deb"
CPP_BUILD_DIR="cpp/build"

# Цвета для вывода
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo "=== Сборка Punto Switcher v${VERSION} ==="

# -----------------------------------------------------------------------------
# Этап 0: Проверка и установка зависимостей
# -----------------------------------------------------------------------------
echo "[0/5] Проверка зависимостей..."

# Обязательные пакеты для сборки
BUILD_DEPS=("build-essential" "cmake" "pkg-config" "libx11-dev")

# Опциональные пакеты (словари для автопереключения)
OPTIONAL_DEPS=("hunspell-en-us" "hunspell-ru")

# Функция проверки установленного пакета
check_package() {
    dpkg -s "$1" &>/dev/null
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
    sudo apt-get update -qq
    sudo apt-get install -y "${MISSING_BUILD_DEPS[@]}"
    echo -e "${GREEN}   Зависимости для сборки установлены${NC}"
else
    echo -e "${GREEN}   Все зависимости для сборки установлены${NC}"
fi

# Предлагаем установить опциональные зависимости
if [[ ${#MISSING_OPTIONAL_DEPS[@]} -gt 0 ]]; then
    echo -e "${YELLOW}   Опциональные пакеты (словари): ${MISSING_OPTIONAL_DEPS[*]}${NC}"
    read -p "   Установить словари для автопереключения? [Y/n] " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Nn]$ ]]; then
        sudo apt-get install -y "${MISSING_OPTIONAL_DEPS[@]}"
        echo -e "${GREEN}   Словари установлены${NC}"
    else
        echo "   Пропускаем установку словарей"
    fi
fi

# -----------------------------------------------------------------------------
# Этап 1: Сборка C++ бинарника
# -----------------------------------------------------------------------------
echo "[1/5] Сборка C++ бинарника..."

mkdir -p cpp/build
cd cpp/build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
cd "$SCRIPT_DIR"

# Проверяем, что бинарник собрался
if [[ ! -f "cpp/build/punto" ]]; then
    echo "Ошибка: бинарник cpp/build/punto не найден!"
    exit 1
fi

echo -e "${GREEN}   Бинарник собран${NC}: $(file cpp/build/punto | cut -d: -f2)"

# -----------------------------------------------------------------------------
# Этап 2: Подготовка структуры пакета
# -----------------------------------------------------------------------------
echo "[2/5] Подготовка структуры пакета..."

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR/DEBIAN"
mkdir -p "$BUILD_DIR/usr/local/bin"
mkdir -p "$BUILD_DIR/etc/punto"
mkdir -p "$BUILD_DIR/usr/share/punto-switcher"

# Копируем бинарник
cp cpp/build/punto "$BUILD_DIR/usr/local/bin/"
chmod 755 "$BUILD_DIR/usr/local/bin/punto"

# Копируем конфигурацию и udevmon для postinst
cp config.yaml "$BUILD_DIR/usr/share/punto-switcher/"
cp udevmon.yaml "$BUILD_DIR/usr/share/punto-switcher/"
cp config.yaml "$BUILD_DIR/etc/punto/config.yaml.new"

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
# Этап 3: Проверка runtime-зависимостей
# -----------------------------------------------------------------------------
echo "[3/5] Проверка runtime-зависимостей..."

# Зависимости для работы программы
RUNTIME_DEPS=("interception-tools" "libx11-6" "xsel")
MISSING_RUNTIME_DEPS=()

for pkg in "${RUNTIME_DEPS[@]}"; do
    if ! check_package "$pkg"; then
        MISSING_RUNTIME_DEPS+=("$pkg")
    fi
done

if [[ ${#MISSING_RUNTIME_DEPS[@]} -gt 0 ]]; then
    echo -e "${YELLOW}   Отсутствуют runtime-пакеты: ${MISSING_RUNTIME_DEPS[*]}${NC}"
    echo "   Эти пакеты нужны для работы punto-switcher."
    read -p "   Установить сейчас? [Y/n] " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Nn]$ ]]; then
        sudo apt-get install -y "${MISSING_RUNTIME_DEPS[@]}"
        echo -e "${GREEN}   Runtime-зависимости установлены${NC}"
    else
        echo -e "${YELLOW}   Предупреждение: пакет может не работать без этих зависимостей${NC}"
    fi
else
    echo -e "${GREEN}   Все runtime-зависимости установлены${NC}"
fi

# -----------------------------------------------------------------------------
# Этап 4: Сборка пакета
# -----------------------------------------------------------------------------
echo "[4/5] Сборка deb-пакета..."

OUTPUT_DEB="${PACKAGE_NAME}_${VERSION}_amd64.deb"
dpkg-deb --build --root-owner-group "$BUILD_DIR" "$OUTPUT_DEB"

# -----------------------------------------------------------------------------
# Этап 5: Информация о пакете
# -----------------------------------------------------------------------------
echo "[5/5] Информация о пакете:"
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

# Показываем статус словарей
if [[ ${#MISSING_OPTIONAL_DEPS[@]} -gt 0 ]]; then
    echo ""
    echo -e "${YELLOW}Примечание: для автопереключения рекомендуется установить:${NC}"
    echo "  sudo apt install ${MISSING_OPTIONAL_DEPS[*]}"
fi

# Очистка
rm -rf "$BUILD_DIR"
