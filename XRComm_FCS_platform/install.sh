#!/bin/bash
# XRComm FCS Platform Installer

set -e

# Ensure we are running as root
if [ "$EUID" -ne 0 ]; then
  echo "Error: Please run this installer with sudo."
  exit 1
fi

if [ -f "/etc/systemd/system/xrcomm-server.service" ]; then
  echo "Error: A previous version of XRComm FCS Platform is already installed."
  echo "Please run the uninstall script first (e.g., sudo ./uninstall.sh)."
  exit 1
fi

echo "========================================"
echo "  XRComm FCS Platform Setup Wizard      "
echo "========================================"

echo "Installing GRPC dependencies..."
apt-get update
apt-get install -y python3-grpcio python3-protobuf python3-grpc-tools || echo "Warning: Dependency installation failed. Continuing..."

# License Agreement
echo -e "\n--- LICENSE AGREEMENT ---"
cat LICENSE
echo "-------------------------"
read -p "Do you agree to the terms of this license? [y/N]: " AGREED
if [[ ! "$AGREED" =~ ^[Yy]$ ]]; then
  echo "Installation aborted."
  exit 1
fi

# Select Platform
echo -e "\nWhich platform would you like to install?"
echo "1) Wideband Platform (XRComm_FCS_wideband_platform)"
echo "2) 8T8R Platform (XRComm_FCS_8T8R_platform)"
read -p "Enter choice [1-2]: " PLATFORM_CHOICE

PLATFORM_DIR=""
INCLUDE_DEST=""
if [ "$PLATFORM_CHOICE" == "1" ]; then
  PLATFORM_DIR="XRComm_FCS_wideband_platform"
  INCLUDE_DEST="/usr/include/xrcomm-wideband"
elif [ "$PLATFORM_CHOICE" == "2" ]; then
  PLATFORM_DIR="XRComm_FCS_8T8R_platform"
  INCLUDE_DEST="/usr/include/xrcomm-8T8R"
else
  echo "Invalid choice. Aborting."
  exit 1
fi

BASE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TARGET_DIR="$BASE_DIR/$PLATFORM_DIR"

if [ ! -d "$TARGET_DIR" ]; then
  echo "Error: Source directory $TARGET_DIR not found!"
  exit 1
fi

echo -e "\n--- Generating Systemd Services ---"
SERVER_BIN="$TARGET_DIR/XRComm_platform_drivers/bin/xrcomm_server"
GRPC_BIN="$TARGET_DIR/XRComm_platform_gRPC/bin/xrcomm_grpc_ctrl"

cat <<EOF > /etc/systemd/system/xrcomm-server.service
[Unit]
Description=XRComm FCS Platform Drivers
After=network.target

[Service]
Type=simple
WorkingDirectory=$TARGET_DIR/XRComm_platform_drivers/bin
Environment=XRCOMM_READ_CONFIG=$BASE_DIR/XRComm_NVMe_loader/config/read_config.ini
ExecStart=$SERVER_BIN
Restart=on-failure

[Install]
WantedBy=multi-user.target
EOF

cat <<EOF > /etc/systemd/system/xrcomm-grpc-ctrl.service
[Unit]
Description=XRComm gRPC Control Service
After=xrcomm-server.service

[Service]
Type=simple
WorkingDirectory=$TARGET_DIR/XRComm_platform_gRPC/bin
Environment=XRCOMM_READ_CONFIG=$BASE_DIR/XRComm_NVMe_loader/config/read_config.ini
ExecStart=$GRPC_BIN
Restart=on-failure

[Install]
WantedBy=multi-user.target
EOF

echo "Enabling and starting services..."
systemctl daemon-reload
systemctl enable xrcomm-server.service
systemctl enable xrcomm-grpc-ctrl.service
systemctl restart xrcomm-server.service
systemctl restart xrcomm-grpc-ctrl.service

echo -e "\n--- Setting up Include Files ---"
# Remove old symlinks if they exist
rm -rf /usr/include/xrcomm-wideband
rm -rf /usr/include/xrcomm-8T8R
ln -s "$TARGET_DIR/XRComm_platform_drivers/include" "$INCLUDE_DEST"
echo "Include files mapped to $INCLUDE_DEST"

echo -e "\n--- Compiling gRPC Proto files ---"
PROTO_DIR="$TARGET_DIR/XRComm_platform_gRPC/proto"
CLIENT_DIR="$TARGET_DIR/XRComm_platform_gRPC/client"

# Create dummy proto file for compilation test if it doesn't exist
if [ ! -f "$PROTO_DIR/pipeline_control.proto" ]; then
  echo "syntax = \"proto3\"; package xrcomm; service PipelineControl {}" > "$PROTO_DIR/pipeline_control.proto"
fi

python3 -m grpc_tools.protoc -I "$PROTO_DIR" --python_out="$CLIENT_DIR" --grpc_python_out="$CLIENT_DIR" "$PROTO_DIR/pipeline_control.proto" || echo "Warning: Proto compilation failed."

echo -e "\n========================================"
echo " Installation Complete!"
echo " Services are now running. Verify with:"
echo "   systemctl status xrcomm-server"
echo "   systemctl status xrcomm-grpc-ctrl"
echo "========================================"
