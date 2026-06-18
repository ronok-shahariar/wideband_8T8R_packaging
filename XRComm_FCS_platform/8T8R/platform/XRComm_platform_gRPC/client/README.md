# XRComm 8T8R Platform gRPC Client

Example Python client for the XRComm 8T8R Platform gRPC control interface.

## Prerequisites

```bash
pip install grpcio grpcio-tools
```

## Generate stubs from the proto contract

```bash
cd /path/to/XRComm_FCS_platform/8T8R/platform/XRComm_platform_gRPC/client/
python3 -m grpc_tools.protoc -I../proto \
    --python_out=. --grpc_python_out=. \
    ../proto/pipeline_control.proto
```

This produces `pipeline_control_pb2.py` and `pipeline_control_pb2_grpc.py` in the
current directory. Generated stubs are not shipped — regenerate from the `.proto` file.

## Run the client

The default endpoint is `127.0.0.1:50051`. Use `--host` and `--port` to override.

```bash
# Read current mode flags
python3 xrcomm_grpc_client.py get-flags

# Enable DSP mode (FULL_PACKET_MODE) — independent, does NOT affect other modes
python3 xrcomm_grpc_client.py set-dsp on

# Enable NVMe logging — independent, can run at same time as DSP and IP
python3 xrcomm_grpc_client.py set-logging on

# Enable IP mode (IQ_MODE) — independent
python3 xrcomm_grpc_client.py set-ip on

# All three modes simultaneously (no mutual exclusion)
python3 xrcomm_grpc_client.py set-dsp on
python3 xrcomm_grpc_client.py set-logging on
python3 xrcomm_grpc_client.py set-ip on

# Per-channel 8T8R telemetry + registered IQ applications
python3 xrcomm_grpc_client.py get-stats

# List registered secondary IQ applications (port + channel shown)
python3 xrcomm_grpc_client.py list-ips

# Stream 20 live status updates at 1 Hz
python3 xrcomm_grpc_client.py watch-status 20

# Stop the platform pipeline
python3 xrcomm_grpc_client.py shutdown
```

## RPC Reference

| RPC | Request | Response | Purpose |
|-----|---------|----------|---------|
| `GetFlags` | Empty | PipelineFlags | Read current mode flags |
| `SetDspMode` | BoolRequest | PipelineFlags | Enable/disable DSP (FULL_PACKET_MODE) |
| `SetLogging` | BoolRequest | PipelineFlags | Enable/disable NVMe IQ capture |
| `SetIpMode` | BoolRequest | PipelineFlags | Enable/disable IQ dispatch (IQ_MODE) |
| `GetStats` | Empty | CombinedStats | Pipeline stats + 8T8R per-channel telemetry |
| `ListSecondaryIPs` | Empty | SecondaryIPList | Registered secondary IQ applications |
| `WatchStatus` | Empty | stream PipelineStatus | 1 Hz live status stream |
| `Shutdown` | Empty | StatusReply | Stop the platform pipeline |

## Mode independence

All three modes operate **independently** and may be active simultaneously:

- `enable_dsp_mode` — FULL_PACKET_MODE: raw packet dispatch to DSP secondary
- `enable_logging` — NVMe raw IQ capture (trigger-gated)
- `enable_ip_mode` — IQ_MODE: per-channel IQ dispatch to registered secondaries

Enabling one mode does **not** disable any other mode.

## Per-channel 8T8R telemetry

`GetStats` and `WatchStatus` both return `repeated ChannelTelemetry` with one entry
per logical channel (8 total: port 0 channels 0–3, port 1 channels 4–7).

Each entry carries:

| Field | Description |
|-------|-------------|
| `hsp_port` | HSP port (0 or 1) |
| `channel` | Channel index (0–7) |
| `samples_received` | IQ samples received on this channel |
| `samples_logged` | Samples written to NVMe (0 when logging off) |
| `samples_processed_secondary` | Samples consumed by registered IQ applications |
| `packets_dropped` | Monotonic drop counter: RX overflow + ring-full combined |

## Secondary IQ application registration

When a secondary registers via `xrcomm_ip_client_init_port_channel()`, the platform
creates a dedicated SHM ring for it. `ListSecondaryIPs` shows all registered slots
with the `hsp_port` and `channel` the application requested, and the `rx_mode`
string (`IQ_MODE`, `FULL_PACKET_MODE`, or `LOG_BLOCK_MODE`).
