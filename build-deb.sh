#!/bin/bash
# Скрипт сборки deb-пакета Punto Switcher
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
VERSION="1.0.3"
PACKAGE_NAME="punto-switcher"
BUILD_DIR="${SCRIPT_DIR}/build-deb"
DEB_DIR="${BUILD_DIR}/${PACKAGE_NAME}_${VERSION}"

echo "=== Сборка deb-пакета Punto Switcher v${VERSION} ==="

# Очистка предыдущей сборки
rm -rf "${BUILD_DIR}"
mkdir -p "${DEB_DIR}"

# Сборка бинарника
echo "Компиляция..."
make -C "${SCRIPT_DIR}" clean
make -C "${SCRIPT_DIR}"

# Создание структуры пакета
echo "Создание структуры пакета..."

# DEBIAN
mkdir -p "${DEB_DIR}/DEBIAN"
cp "${SCRIPT_DIR}/DEBIAN/control" "${DEB_DIR}/DEBIAN/"
cp "${SCRIPT_DIR}/DEBIAN/postinst" "${DEB_DIR}/DEBIAN/"
cp "${SCRIPT_DIR}/DEBIAN/prerm" "${DEB_DIR}/DEBIAN/"
chmod 755 "${DEB_DIR}/DEBIAN/postinst"
chmod 755 "${DEB_DIR}/DEBIAN/prerm"

# Бинарники -> /usr/local/bin
mkdir -p "${DEB_DIR}/usr/local/bin"
cp "${SCRIPT_DIR}/punto" "${DEB_DIR}/usr/local/bin/"
cp "${SCRIPT_DIR}/punto-switch" "${DEB_DIR}/usr/local/bin/"
cp "${SCRIPT_DIR}/punto-invert" "${DEB_DIR}/usr/local/bin/"
cp "${SCRIPT_DIR}/punto-case-invert" "${DEB_DIR}/usr/local/bin/"
cp "${SCRIPT_DIR}/punto-translit" "${DEB_DIR}/usr/local/bin/"
chmod 755 "${DEB_DIR}/usr/local/bin/"*

# Конфигурации -> /usr/share/punto-switcher
mkdir -p "${DEB_DIR}/usr/share/punto-switcher"
cp "${SCRIPT_DIR}/config.yaml" "${DEB_DIR}/usr/share/punto-switcher/"
cp "${SCRIPT_DIR}/udevmon.yaml" "${DEB_DIR}/usr/share/punto-switcher/"

# Документация -> /usr/share/doc
mkdir -p "${DEB_DIR}/usr/share/doc/${PACKAGE_NAME}"
cp "${SCRIPT_DIR}/README.md" "${DEB_DIR}/usr/share/doc/${PACKAGE_NAME}/"

# Сборка пакета
echo "Сборка deb-пакета..."
dpkg-deb --build "${DEB_DIR}"

# Перемещение в корень проекта
mv "${BUILD_DIR}/${PACKAGE_NAME}_${VERSION}.deb" "${SCRIPT_DIR}/punto-ubuntu.deb"

# Очистка
rm -rf "${BUILD_DIR}"

echo ""
echo "=== Готово! ==="
echo "Пакет создан: ${SCRIPT_DIR}/punto-ubuntu.deb"
echo ""
echo "Для установки выполните:"
echo "  sudo apt install ./punto-ubuntu.deb"
echo ""
echo "Для удаления:"
echo "  sudo apt remove punto-switcher"
