# XRComm Wideband Playback — gRPC Client

Example gRPC client for the **playback** control surface of the XRComm FCS
wideband platform (Guide Sections 8.3 & 8.4). It connects to the platform gRPC
endpoint (default `localhost:50051`) and exercises every playback RPC.

## How it works

The playback executable reads **only** `inputs/config.ini`. It does not receive
gRPC calls. The gRPC **server** is the single writer of that file:

```
gRPC client  →  gRPC server  →  inputs/config.ini  →  playback executable
```

Each per-field `Set` RPC validates one value and rewrites **only its own key**
inside `config.ini`; the other seven keys are left byte-for-byte untouched. The
playback executable picks the new values up at its next `StartPlayback`.

Steady-state diagnostics are published once per second by the playback
executable to a status file at the playback tree root; the server reads that
file to answer `GetPlaybackDiagnostics` / `WatchPlaybackDiagnostics`.

## Generate the stubs

The `.proto` ships; the generated stubs do not. Generate them into this folder:

```bash
pip install grpcio grpcio-tools
python -m grpc_tools.protoc -I../proto \
    --python_out=. --grpc_python_out=. ../proto/pipeline_control.proto
```

This produces `pipeline_control_pb2.py` and `pipeline_control_pb2_grpc.py`.

## Run

With the platform gRPC server running:

```bash
# Read all eight current values
python playback_grpc_client.py get-config

# Read / write a single field (one parameter per call — no bulk setter)
python playback_grpc_client.py get sample_rate
python playback_grpc_client.py set sample_rate 1474560000
python playback_grpc_client.py set binary_filename tone_512.bin
python playback_grpc_client.py set loop_count 0
python playback_grpc_client.py set trigger_burst_size 4

# Start / stop the playback executable
python playback_grpc_client.py start
python playback_grpc_client.py stop

# Diagnostics: one snapshot, or a 1 Hz stream
python playback_grpc_client.py diag
python playback_grpc_client.py watch

# Point at a non-default endpoint
python playback_grpc_client.py --endpoint 192.168.1.50:50051 get-config
```

If a setter is called while playback is running, the response carries a trailing
metadata `note` stating the value takes effect at the next `StartPlayback`; the
client prints it.

## Playback RPC reference

| RPC | Request | Response | Purpose |
|---|---|---|---|
| GetPlaybackConfig | Empty | PlaybackConfig | Read all eight current values (read-only snapshot) |
| GetCoreList / SetCoreList | Empty / CoreListValue | CoreListValue / PlaybackConfig | Read / write `core_list` |
| GetSourceMode / SetSourceMode | Empty / SourceModeValue | SourceModeValue / PlaybackConfig | Read / write `source_mode` |
| GetBinaryFilename / SetBinaryFilename | Empty / BinaryFilenameValue | BinaryFilenameValue / PlaybackConfig | Read / write `binary_filename` |
| GetSampleRate / SetSampleRate | Empty / SampleRateValue | SampleRateValue / PlaybackConfig | Read / write `target_sample_rate_hz` |
| GetTxPortId / SetTxPortId | Empty / TxPortIdValue | TxPortIdValue / PlaybackConfig | Read / write `tx_port_id` |
| GetDestMac / SetDestMac | Empty / DestMacValue | DestMacValue / PlaybackConfig | Read / write `dest_mac` |
| GetLoopCount / SetLoopCount | Empty / LoopCountValue | LoopCountValue / PlaybackConfig | Read / write `loop_count` |
| GetTriggerBurstSize / SetTriggerBurstSize | Empty / TriggerBurstSizeValue | TriggerBurstSizeValue / PlaybackConfig | Read / write `trigger_burst_size` |
| StartPlayback | Empty | StatusReply | Launch playback using current config.ini; error if already running |
| StopPlayback | Empty | StatusReply | Stop playback cleanly; error if not running |
| GetPlaybackDiagnostics | Empty | PlaybackDiagnostics | One snapshot of the steady-state diagnostics |
| WatchPlaybackDiagnostics | Empty | stream PlaybackDiagnostics | Diagnostics stream, one update per second |

## Per-field validation (server side)

| Field | Validation before writing config.ini |
|---|---|
| core_list | Comma-separated integer list, all cores valid on the host |
| source_mode | Must be 0 or 1 |
| binary_filename | File must exist in `inputs/playback_waveforms/` (error names the path) |
| target_sample_rate_hz | Must be > 0 |
| tx_port_id | Must be a valid port id |
| dest_mac | Must parse as `XX:XX:XX:XX:XX:XX` |
| loop_count | Any uint32 (0 = infinite) |
| trigger_burst_size | Must be ≥ 1 |

An invalid value returns a gRPC error naming the field, and `config.ini` is not
modified.