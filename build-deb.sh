#!/bin/bash
# =============================================================================
# Сборка deb-пакета Punto Switcher v2.0 (C++20)
# =============================================================================
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

VERSION="2.0.0"
PACKAGE_NAME="punto-switcher"
BUILD_DIR="build-deb"
CPP_BUILD_DIR="cpp/build"

echo "=== Сборка Punto Switcher v${VERSION} ==="

# -----------------------------------------------------------------------------
# Этап 1: Сборка C++ бинарника
# -----------------------------------------------------------------------------
echo "[1/4] Сборка C++ бинарника..."

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

echo "   Бинарник собран: $(file cpp/build/punto | cut -d: -f2)"

# -----------------------------------------------------------------------------
# Этап 2: Подготовка структуры пакета
# -----------------------------------------------------------------------------
echo "[2/4] Подготовка структуры пакета..."

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR/DEBIAN"
mkdir -p "$BUILD_DIR/usr/local/bin"
mkdir -p "$BUILD_DIR/etc/punto"

# Копируем бинарник
cp cpp/build/punto "$BUILD_DIR/usr/local/bin/"
chmod 755 "$BUILD_DIR/usr/local/bin/punto"

# Копируем конфигурацию
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
# Этап 3: Сборка пакета
# -----------------------------------------------------------------------------
echo "[3/4] Сборка deb-пакета..."

OUTPUT_DEB="${PACKAGE_NAME}_${VERSION}_amd64.deb"
dpkg-deb --build --root-owner-group "$BUILD_DIR" "$OUTPUT_DEB"

# -----------------------------------------------------------------------------
# Этап 4: Информация о пакете
# -----------------------------------------------------------------------------
echo "[4/4] Информация о пакете:"
echo ""
dpkg-deb -I "$OUTPUT_DEB"
echo ""
echo "Содержимое:"
dpkg-deb -c "$OUTPUT_DEB"
echo ""
echo "=== Готово! ==="
echo "Пакет: $OUTPUT_DEB"
echo "Размер: $(du -h "$OUTPUT_DEB" | cut -f1)"
echo ""
echo "Установка: sudo dpkg -i $OUTPUT_DEB"
echo "Удаление:  sudo dpkg -r $PACKAGE_NAME"

# Очистка
rm -rf "$BUILD_DIR"
