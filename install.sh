#!/bin/bash
# Punto Switcher Installation Script

set -e

echo "=== Building Punto Switcher ==="
make clean
make

echo ""
echo "=== Installing binary ==="
sudo make install

echo ""
echo "=== Installing udevmon config ==="
sudo cp udevmon.yaml /etc/udevmon.yaml

echo ""
echo "=== Restarting udevmon service ==="
sudo systemctl restart udevmon
sudo systemctl enable udevmon

echo ""
echo "=== Done! ==="
echo "Punto Switcher is now active."
echo ""
echo "To check status: sudo systemctl status udevmon"
echo "To view logs: sudo journalctl -u udevmon -f"
echo "To disable: sudo systemctl stop udevmon"
