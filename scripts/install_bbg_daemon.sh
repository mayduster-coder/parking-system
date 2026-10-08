#!/bin/bash
# =============================================================
# install_bbg_daemon.sh - build process1/process2 on the BeagleBone and
#                         install them as systemd daemons that start
#                         automatically at boot.
#
# Run ON THE BEAGLEBONE from the repository root:
#     sudo ./scripts/install_bbg_daemon.sh
#
# Useful commands afterwards:
#     systemctl status parking-i2c parking-eth
#     journalctl -u parking-eth -f          (live output of process 1)
#     sudo systemctl restart parking-eth
#     sudo systemctl disable --now parking-i2c parking-eth   (uninstall)
# =============================================================

set -e
INSTALL_DIR=/opt/parking
REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"

if [ "$(id -u)" -ne 0 ]; then
    echo "Please run with sudo"; exit 1
fi

echo "[install] Building..."
make -C "$REPO_DIR/bbg"

echo "[install] Stopping old instances..."
systemctl stop parking-eth parking-i2c 2>/dev/null || true
pkill -x process1 2>/dev/null || true
pkill -x process2 2>/dev/null || true

echo "[install] Copying files to $INSTALL_DIR..."
mkdir -p "$INSTALL_DIR"
cp "$REPO_DIR/bbg/process1" "$REPO_DIR/bbg/process2" "$INSTALL_DIR/"
# keep an existing config (it may have been edited on the board)
[ -f "$INSTALL_DIR/process1.conf" ] || cp "$REPO_DIR/bbg/process1.conf" "$INSTALL_DIR/"

echo "[install] Installing systemd services..."
cp "$REPO_DIR/bbg/parking-i2c.service" "$REPO_DIR/bbg/parking-eth.service" /etc/systemd/system/
systemctl daemon-reload
systemctl enable --now parking-i2c.service parking-eth.service

echo "[install] Done."
systemctl --no-pager --lines=0 status parking-i2c parking-eth || true
