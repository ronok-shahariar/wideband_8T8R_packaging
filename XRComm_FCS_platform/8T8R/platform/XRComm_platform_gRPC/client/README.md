# XRComm 8T8R Platform gRPC Client

Example Python client for the XRComm 8T8R Platform gRPC control interface.

## Prerequisites

```bash
pip install grpcio grpcio-tools
```

## Generate stubs from the proto contract

```bash
cd XRComm_FCS_platform/8T8R/platform/XRComm_platform_gRPC/client/
python3 -m grpc_tools.protoc -I../proto \
    --python_out=. --grpc_python_out=. \
    ../proto/pipeline_control.proto
```

This produces `pipeline_control_pb2.py` and `pipeline_control_pb2_grpc.py` in the
current directory. The package includes generated stubs; regenerate them only after an
intentional `.proto` update.

## Run the client

The default endpoint is `127.0.0.1:50051`. Use `--host` and `--port` to override.

```bash
# Read current mode flags
python3 xrcomm_grpc_client.py get-flags

# Enable Full-Packet Mode (FULL_PACKET_MODE) — independent, does NOT affect other modes
python3 xrcomm_grpc_client.py set-fullpacket on

# Enable NVMe logging — independent, can run at same time as Full-Packet and IP
python3 xrcomm_grpc_client.py set-logging on

# Enable IP mode (IQ_MODE) — independent
python3 xrcomm_grpc_client.py set-ip on

# All three modes simultaneously (no mutual exclusion)
python3 xrcomm_grpc_client.py set-fullpacket on
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
| `SetFullPacketMode` | BoolRequest | PipelineFlags | Enable/disable Full-Packet Mode (FULL_PACKET_MODE) |
| `SetLogging` | BoolRequest | PipelineFlags | Enable/disable NVMe IQ capture |
| `SetIpMode` | BoolRequest | PipelineFlags | Enable/disable IQ dispatch (IQ_MODE) |
| `GetStats` | Empty | CombinedStats | Pipeline stats + 8T8R per-channel telemetry + auto-read status |
| `ListSecondaryIPs` | Empty | SecondaryIPList | Registered secondary IQ applications |
| `WatchStatus` | Empty | stream PipelineStatus | 1 Hz live status stream (includes auto-read progress) |
| `SetReadCapture` | ReadCaptureRequest | ReadConfig | Set per-channel auto-read `start_ts` + `duration` (seconds) |
| `SetReadCaptureRate` | ReadCaptureRateRequest | ReadConfig | Set per-channel receiver `sample_rate` (Hz) |
| `GetReadConfig` | Empty | ReadConfig | Read all 8 per-channel auto-read rows |
| `GetReadStatus` | Empty | ReadStatus | Live auto-read progress per channel |
| `Shutdown` | Empty | StatusReply | Stop the platform pipeline |

## NVMe Auto-Read

After NVMe logging is turned **off via gRPC** (`set-logging off`), the platform automatically performs **one read pass per session** over every channel that captured data. The read is **not** triggered on shutdown or on failure.

Per-channel read parameters live in `8T8R/platform/config/read_config.ini` (one row per channel: `ch_no`, `start_ts`, `duration`, `sample_rate`). The installer sets `XRCOMM_READ_CONFIG` to this file for both platform services. Set them over gRPC before turning logging off:

```bash
# Set channel 2 to read 2 seconds of data starting 0.5 s into the event:
python3 xrcomm_grpc_client.py set-read-capture 2 0.5 2.0

# Set channel 2 receiver sample rate to 1966080000 Hz:
python3 xrcomm_grpc_client.py set-read-rate 2 1966080000

# View the current per-channel read configuration:
python3 xrcomm_grpc_client.py get-read-config

# Watch live read progress (or use get-stats / watch-status):
python3 xrcomm_grpc_client.py get-read-status
```

**How the read fires:**

```bash
# 1. Enable logging and capture some triggered data:
python3 xrcomm_grpc_client.py set-logging on
#    ... playback sends trigger-marked packets ...

# 2. Turn logging off — this triggers the automatic read:
python3 xrcomm_grpc_client.py set-logging off

# 3. Watch the read happen (status shows "reading" for ~the read duration):
python3 xrcomm_grpc_client.py watch-status 10
```

**Record conversion:** `records = floor(seconds × sample_rate / 131072)`. For example, 1.0 s at 1474560000 Hz = 11250 records = 5625 MB of IQ payload. `start_ts` becomes a whole-record skip; `duration` becomes a whole-record count. Both floor to whole records.

**Edge cases (handled with warnings, never failure):**
- If `start_ts` is at or beyond the captured event length, the reader logs a warning and reads from the start of the records.
- If `start_ts + duration` exceeds the event length, the duration is capped to what remains and a warning is logged.

Read output goes to `XRComm_platform_drivers/output_logs/ch<N>/event_<id>_<timestamp>/`.

## Mode independence

All three modes operate **independently** and may be active simultaneously:

- `enable_full_packet_mode` — FULL_PACKET_MODE: raw packet dispatch to an attached secondary
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
