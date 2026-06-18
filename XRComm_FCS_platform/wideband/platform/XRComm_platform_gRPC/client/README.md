# XRComm Platform — Remote gRPC Control Client (Python)

A standalone client for controlling the XRComm FCS wideband platform over gRPC.
It is **separate from the platform gRPC server** (`xrcomm_grpc_ctrl`) and is
meant to run on a **remote host** — it connects over the network and holds no
shared memory.

## 1. Install dependencies

```bash
pip install -r requirements.txt
```

## 2. Generate the stubs (one time, and after any .proto change)

```bash
python -m grpc_tools.protoc -I. --python_out=. --grpc_python_out=. \
       pipeline_control.proto
```

This produces `pipeline_control_pb2.py` and `pipeline_control_pb2_grpc.py`
next to the client. (These generated files are not shipped — regenerate them
from the `.proto`, which is the interface contract.)

## 3. Run

The platform gRPC server listens on `0.0.0.0:50051` by default, so point the
client at the platform host:

```bash
./xrcomm_grpc_client.py --host <platform-ip>:50051 <command> [on|off]
```

## Commands

| Command | RPC | Purpose |
|---|---|---|
| `get-flags` | `GetFlags` | Read current dsp / ip / logging / rt-loop flags |
| `set-dsp on\|off` | `SetDspMode` | Enable/disable FULL_PACKET (DSP) dispatch |
| `set-ip on\|off` | `SetIpMode` | Enable/disable IQ dispatch to applications |
| `set-logging on\|off` | `SetLogging` | Enable/disable NVMe (LOG_BLOCK) capture. Turning logging **off** via this command triggers the automatic NVMe read-back (below) if a capture occurred. |
| `set-read-capture <seconds> [--start-offset <seconds>]` | `SetReadCapture` | Configure the automatic read-back: how many seconds to read, and the offset **from the start of the capture** (not a wall-clock time) at which to begin. Overwrites `platform/config/read_config.ini`. |
| `set-read-rate <hz>` | `SetReadCaptureRate` | Set the receiver sample rate (Hz) used to size the read-back. Overwrites `platform/config/read_config.ini`. |
| `get-stats` | `GetStats` | Pipeline + DSP + spectrum + **platform telemetry** |
| `get-mode-stats` | `GetModeStats` | Per-API-mode + per-application Rx/processed/drops |
| `list-ips` | `ListSecondaryIPs` | Registered customer applications |
| `watch-status` | `WatchStatus` | Live status stream (~1 Hz) incl. platform telemetry and read-back progress |
| `shutdown` | `Shutdown` | Stop the platform pipeline cleanly |

### NVMe automatic read-back (`set-read-capture`, `set-read-rate`)

When a capture is stopped with `set-logging off` (and **only** then — not on
shutdown or failure, and only if a capture actually happened), the platform
automatically copies a configurable window of the just-finished capture back
out of NVMe to host files. Two values control this window, both stored in
`platform/config/read_config.ini`:

| Key | Set by | Meaning |
|---|---|---|
| `start_offset_seconds` | `set-read-capture --start-offset` | Offset **from the start of the capture** at which to begin reading. `0.0` (default) = the first record. This is **not** a wall-clock time. |
| `read_duration_seconds` | `set-read-capture <seconds>` | How long to read, starting at `start_offset_seconds`. |
| `sample_rate_hz` | `set-read-rate <hz>` | Receiver sample rate, used to convert both of the above into whole NVMe records (rounded **down** to the nearest full record). |

If `start_offset_seconds` falls at or beyond the capture's length, the
platform reads from the start of the capture instead and logs a warning. If
`start_offset_seconds + read_duration_seconds` exceeds the capture, the read
is capped to whatever remains and a warning is logged — the read-back never
fails because of these conditions.

```bash
# Read 1 second starting 0.5 s into every capture, at an 800 MHz receiver rate
./xrcomm_grpc_client.py --host 192.168.1.10:50051 set-read-rate 800e6
./xrcomm_grpc_client.py --host 192.168.1.10:50051 set-read-capture 1.0 --start-offset 0.5
```

**Both the control client and the platform driver must resolve
`platform/config/read_config.ini` to the same file.** By default this is a path
relative to each process's own working directory; if the platform driver and
`xrcomm_grpc_ctrl` are started from different directories, export
`XRCOMM_READ_CONFIG` to the same absolute path in both terminals before
starting them:
```bash
export XRCOMM_READ_CONFIG="/absolute/path/to/XRComm_FCS_platform/wideband/platform/config/read_config.ini"
```

### Platform telemetry fields (`get-stats`, `watch-status`)

Per the packaging guide §8.2 (wideband, one set for the whole platform):

| Field | Meaning |
|---|---|
| `hsp_port` | HSP port in use — **NIC port id + 1** (NIC port 0 → hsp 1, port 1 → hsp 2) |
| `samples_received` | Total IQ samples received |
| `samples_logged` | Samples written to NVMe (0 unless logging active) |
| `samples_processed_secondary` | Samples consumed by customer application(s) |
| `packets_dropped` | **Total** loss across every stage below (monotonic) |
| `drops_nic_rx_overflow` | NIC hardware RX-ring overflow (`imissed`) — packets dropped before they ever entered the pipeline |
| `drops_no_mem` | Processing-stage drops: mbuf clone / SharedBlock pool exhaustion (typically the NVMe logging stage falling behind) |
| `drops_ring_full` | RX→processing or processing→logging ring full (a downstream stage isn't draining fast enough) |
| `drops_dsp` | FULL_PACKET dispatch-ring (`DSP_MBUF_RING`) full |
| `drops_app` | Application-ring-full, summed across connected applications |

`packets_dropped` is the sum of the five breakdown fields. If you observe
drops while logging is enabled, check `drops_no_mem` and `drops_ring_full`
first — they indicate the NVMe logging path is the bottleneck rather than the
network interface itself.

## Examples

```bash
./xrcomm_grpc_client.py --host 192.168.1.10:50051 set-dsp on
./xrcomm_grpc_client.py --host 192.168.1.10:50051 set-ip on
./xrcomm_grpc_client.py --host 192.168.1.10:50051 set-read-rate 800e6
./xrcomm_grpc_client.py --host 192.168.1.10:50051 set-read-capture 1.0 --start-offset 0.5
./xrcomm_grpc_client.py --host 192.168.1.10:50051 set-logging on
./xrcomm_grpc_client.py --host 192.168.1.10:50051 set-logging off   # triggers read-back
./xrcomm_grpc_client.py --host 192.168.1.10:50051 get-stats
./xrcomm_grpc_client.py --host 192.168.1.10:50051 watch-status
./xrcomm_grpc_client.py --host 192.168.1.10:50051 shutdown
```

## Mode rules

The three mode flags are independent — DSP, IP, and logging may be enabled in
any combination and run in parallel. Flag changes take effect within ~1 second.

## Interface version

The client's `.proto` is the interface contract with the platform driver and
the gRPC server. After pulling an updated `.proto` (new RPCs or fields),
**rebuild the platform driver and the gRPC server, and regenerate the Python
stubs (step 2 above) together** — a client built against a newer `.proto`
than the server understands will fail RPC calls for the new fields/methods,
and a server built against a newer shared-memory layout than a running
secondary application expects will refuse to attach until that secondary is
rebuilt too.
