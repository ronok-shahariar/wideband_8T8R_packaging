#!/bin/bash
# XRComm FCS Platform Uninstaller

set -e

# Ensure we are running as root
if [ "$EUID" -ne 0 ]; then
  echo "Error: Please run this uninstaller with sudo."
  exit 1
fi

echo "========================================"
echo "  XRComm FCS Platform Uninstaller       "
echo "========================================"

read -p "Are you sure you want to completely remove the XRComm services? [y/N]: " CONFIRM
if [[ ! "$CONFIRM" =~ ^[Yy]$ ]]; then
  echo "Uninstallation aborted."
  exit 0
fi

echo "Stopping and disabling services..."
systemctl stop xrcomm-server.service || true
systemctl stop xrcomm-grpc-ctrl.service || true
systemctl disable xrcomm-server.service || true
systemctl disable xrcomm-grpc-ctrl.service || true

echo "Removing systemd service files..."
rm -f /etc/systemd/system/xrcomm-server.service
rm -f /etc/systemd/system/xrcomm-grpc-ctrl.service

echo "Reloading systemd daemon..."
systemctl daemon-reload

echo "Removing include files mappings..."
rm -rf /usr/include/xrcomm-wideband
rm -rf /usr/include/xrcomm-8T8R

echo -e "\n========================================"
echo " Uninstallation Complete."
echo "========================================"
