# XRComm FCS Platform

This directory contains the core binaries, gRPC servers, playback tools, and example applications for the XRComm FCS Platform.

## Directory Structure Overview

- **`install.sh` / `uninstall.sh`**: Scripts to set up and remove the selected platform as systemd services.
- **`wideband/`**: Wideband package root, including `platform/`, `hello_world/`, `playback/`, `nvme_loader/`, and `Wideband_Quickstart.pdf`.
- **`8T8R/`**: 8T8R package root, including `platform/`, `hello_world/`, `playback/`, `nvme_loader/`, and `8T8R_Quickstart.pdf`.
- **`wideband/platform/`** and **`8T8R/platform/`**: Platform-specific driver binaries, public headers, gRPC services, and `config/read_config.ini`.
- **`wideband/nvme_loader/`** and **`8T8R/nvme_loader/`**: Optional offline NVMe reader tools.

## Getting Started

To install the platform, execute the installer as root:

```bash
sudo ./install.sh
```

When prompted, accept the license and select the platform you want to install.

## Verifying the Installation

Once the installer finishes, verify that both the server and the gRPC control loop are active:

```bash
systemctl status xrcomm-server.service
systemctl status xrcomm-grpc-ctrl.service
journalctl -u xrcomm-server.service -f
```

## Testing the gRPC Control Loop

The installer automatically compiles the protobuf files for the selected platform. For example, to test Wideband telemetry:

```bash
cd wideband/platform/XRComm_platform_gRPC/client
python3 xrcomm_grpc_client.py get-stats
```

For 8T8R, use:

```bash
cd 8T8R/platform/XRComm_platform_gRPC/client
python3 xrcomm_grpc_client.py get-stats
```

## Compiling Hello-World Examples

Each platform keeps its examples inside its own variant root:

```bash
cd wideband/hello_world
mkdir -p build && cd build
cmake ..
make
../bin/hello_world
```

```bash
cd 8T8R/hello_world
mkdir -p build && cd build
cmake ..
make
../bin/hello_world
```
