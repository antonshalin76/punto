#!/bin/bash
set -e

VERSION="0.1.0"
PKG_NAME="punto_ubuntu"
BUILD_DIR="build/${PKG_NAME}"

echo "Building package ${PKG_NAME} version ${VERSION}..."

# Clean
rm -rf build
mkdir -p build

# Create structure
mkdir -p "${BUILD_DIR}/DEBIAN"
mkdir -p "${BUILD_DIR}/opt/punto/app/punto"
mkdir -p "${BUILD_DIR}/usr/lib/systemd/user"
mkdir -p "${BUILD_DIR}/usr/bin"

# Copy Control and Scripts
cp packaging/DEBIAN/* "${BUILD_DIR}/DEBIAN/"
chmod 755 "${BUILD_DIR}/DEBIAN/postinst"

# Copy Source Code
# Exclude __pycache__ etc
rsync -av --exclude='__pycache__' --exclude='*.pyc' punto/ "${BUILD_DIR}/opt/punto/app/punto/"
# Assets
mkdir -p "${BUILD_DIR}/opt/punto/app/punto/assets"
rsync -av punto/assets/ "${BUILD_DIR}/opt/punto/app/punto/assets/"

# Copy Systemd Unit
cp packaging/punto.service "${BUILD_DIR}/usr/lib/systemd/user/"

# Create launcher wrapper
cat > "${BUILD_DIR}/usr/bin/punto-daemon" <<EOF
#!/bin/bash
export PYTHONPATH=/opt/punto/app
exec /opt/punto/venv/bin/python -m punto.daemon.main "\$@"
EOF
chmod 755 "${BUILD_DIR}/usr/bin/punto-daemon"

# Create GUI launcher
cat > "${BUILD_DIR}/usr/bin/punto-tray" <<EOF
#!/bin/bash
export PYTHONPATH=/opt/punto/app
exec /opt/punto/venv/bin/python -m punto.gui.tray "\$@"
EOF
chmod 755 "${BUILD_DIR}/usr/bin/punto-tray"

# Autostart Desktop Entry for Tray
mkdir -p "${BUILD_DIR}/etc/xdg/autostart"
cat > "${BUILD_DIR}/etc/xdg/autostart/punto-tray.desktop" <<EOF
[Desktop Entry]
Type=Application
Exec=/usr/bin/punto-tray
Hidden=false
NoDisplay=false
X-GNOME-Autostart-enabled=true
Name[en_US]=Punto Tray
Name=Punto Tray
Comment=Punto Switcher Tray Icon
EOF

# Build
dpkg-deb --build "${BUILD_DIR}" "${PKG_NAME}.deb"
echo "Package built successfully: ${PKG_NAME}.deb"
