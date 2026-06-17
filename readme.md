# XRComm FCS Platform — Customer Setup Guide

Welcome to the XRComm FCS Platform. This guide explains how to install the required drivers and gRPC services, and how to test the system once installed.

## Prerequisites

The installer requires `sudo` privileges. It will automatically attempt to install the following dependencies:
- `python3-grpcio`
- `python3-protobuf`
- `python3-grpc-tools`

## Installation

To install the XRComm FCS platform, navigate to the `XRComm_FCS_platform` directory and execute the installer:

```bash
cd XRComm_FCS_platform
chmod +x install.sh
sudo ./install.sh
```

During installation, you will be prompted to:
1. Agree to the software LICENSE.
2. Select which platform variant you wish to install (Wideband or 8T8R).

The installer will then:
- Generate and install systemd service files for the server and gRPC control loop.
- Automatically compile the gRPC protobuf files.
- Map the public header files to `/usr/include/xrcomm-wideband` (or 8T8R) so your customer applications can find them easily.

### Checking Service Status

Once the installer finishes, the services will be running in the background. You can check their status using:

```bash
systemctl status xrcomm-server
systemctl status xrcomm-grpc-ctrl
```

## Compiling Your Secondaries

After installation, your C/C++ applications can include the XRComm headers using the mapped system paths. Depending on the platform you installed:

```c
// For Wideband:
#include <xrcomm-wideband/xrcomm_ip_client.h>

// For 8T8R:
#include <xrcomm-8T8R/xrcomm_ip_client.h>
```

When writing your `CMakeLists.txt` or Makefile, simply link against the path installed by the script.

## gRPC Control (Python Client)

The installer automatically compiles the python proto dependencies. You can interact with the running platform using the example python client located in the `XRComm_platform_gRPC/client` directory.

### Example Commands:

```bash
cd XRComm_FCS_wideband_platform/XRComm_platform_gRPC/client

# Retrieve current pipeline statistics and telemetry
python3 xrcomm_grpc_client.py get-stats

# Enable DSP mode remotely
python3 xrcomm_grpc_client.py set-dsp-mode

# Stop the platform gracefully
python3 xrcomm_grpc_client.py shutdown
```

## Uninstallation

To completely remove the services, stop the systemd daemons, and remove the system include mappings, run the uninstaller:

```bash
cd XRComm_FCS_platform
chmod +x uninstall.sh
sudo ./uninstall.sh
```

## Tarring

```bash
tar --exclude='.git' -czvf XRComm_FCS_platform.tar.gz XRComm_FCS_platform/
```