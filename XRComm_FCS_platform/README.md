# XRComm FCS Platform

This directory contains the core binaries, gRPC servers, and example applications for the XRComm FCS Platform.

## Directory Structure Overview

- **`install.sh` / `uninstall.sh`**: Scripts to setup and teardown the platform as systemd services.
- **`XRComm_FCS_wideband_platform/`**: Contains the compiled driver binaries and public headers for the Wideband platform.
- **`XRComm_FCS_8T8R_platform/`**: Reserved for the upcoming 8T8R platform binaries.
- **`XRComm_wideband_hello_world/`**: Customer example applications showing how to connect to the Wideband pipeline.
- **`XRComm_wideband_playback/`**: Remote playback utility for pre-recorded IQ waveforms.
- **`QUICK_START.pdf`**: Customer guide for initial bring-up and testing.

## Getting Started

To install the platform, execute the installer as root:
```bash
sudo ./install.sh
```
*(Note: When prompted, accept the license and select Option `1` for the Wideband Platform).*

## Verifying the Installation

Once the installer finishes, verify that both the server and the gRPC control loop are active:
```bash
# Check service status
systemctl status xrcomm-server.service
systemctl status xrcomm-grpc-ctrl.service

# To view the live logs for the server
journalctl -u xrcomm-server.service -f
```

## Testing the gRPC Control Loop

The installer automatically compiles the protobuf files for you. You can immediately test the pipeline telemetry:
```bash
cd XRComm_FCS_wideband_platform/XRComm_platform_gRPC/client

# Retrieve the real-time pipeline stats
python3 xrcomm_grpc_client.py get-stats
```

## Compiling the "Hello World" Examples

We have provided example C applications to demonstrate how to connect to the data plane using the installed system headers. 
```bash
# Navigate to the example directory from the platform root
cd XRComm_wideband_hello_world

# Compile the examples
mkdir build && cd build
cmake ..
make

# Run the standard IQ consumer
../bin/hello_world
```

**Testing the Data Flow:** 
If you leave `hello_world` running in one terminal, you can open a second terminal and use the gRPC client to turn on the IP data dispatch:
```bash
cd XRComm_FCS_wideband_platform/XRComm_platform_gRPC/client
python3 xrcomm_grpc_client.py set-ip on
```
You should immediately see the `hello_world` application wake up and begin printing power levels (dBFS) for the incoming batches.
