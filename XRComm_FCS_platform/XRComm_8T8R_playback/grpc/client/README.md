# XRComm 8T8R — Playback gRPC client

Example client for the `PlaybackControl` service. It exercises every playback
RPC: read the whole config, get/set each field, start/stop the playback
executable, and read or stream steady-state diagnostics.

This is the **playback** control surface only (guide Sections 8.3 + 8.4). The
platform RPCs (8.1 flags, 8.2 telemetry) live in the full contract and are not
part of this playback-scoped deliverable.

## 1. Prerequisites

```bash
pip install grpcio grpcio-tools
```

## 2. Shipped stubs

The Python gRPC stubs are shipped in this directory and in
`XRComm_8T8R_playback/bin/` for the server. Regenerate them from
`grpc/proto/pipeline_control.proto` only if XRComm provides an updated proto.

## 3. Run

Default endpoint is `localhost:50051` (override with `--target host:port`).

```bash
python playback_client.py get-config
python playback_client.py get core_list
python playback_client.py set core_list 6,7,8,9,10
python playback_client.py get-channel 3
python playback_client.py set-channel 3 tone_512.bin   # "" to disable a channel
python playback_client.py get-txport 0
python playback_client.py set-txport 0 1
python playback_client.py get-destmac 1
python playback_client.py set-destmac 1 <receiver-nic-mac>
python playback_client.py set sample_rate 1966080000
python playback_client.py set loop_count 0
python playback_client.py set trigger_burst_size 1
python playback_client.py start
python playback_client.py diagnostics
python playback_client.py watch          # streams once per second; Ctrl-C to stop
python playback_client.py stop
```

A `set*` succeeds, writes exactly one key into `inputs/config.ini`, and prints
the full updated `PlaybackConfig`. An invalid value returns a gRPC
`INVALID_ARGUMENT` naming the field; `config.ini` is left unchanged.

## 4. RPC reference

| RPC | Request | Response | Purpose |
| --- | --- | --- | --- |
| GetPlaybackConfig | Empty | PlaybackConfig | All 16 current `config.ini` values (read-only snapshot) |
| GetCoreList / SetCoreList | Empty / CoreListValue | …Value / PlaybackConfig | `core_list` (comma-separated host cores) |
| GetSourceMode / SetSourceMode | Empty / SourceModeValue | …Value / PlaybackConfig | `source_mode` (0 NVMe, 1 file) |
| GetSampleRate / SetSampleRate | Empty / SampleRateValue | …Value / PlaybackConfig | `target_sample_rate_hz` (>0) |
| GetLoopCount / SetLoopCount | Empty / LoopCountValue | …Value / PlaybackConfig | `loop_count` (0 = infinite) |
| GetTriggerBurstSize / SetTriggerBurstSize | Empty / TriggerBurstSizeValue | …Value / PlaybackConfig | `trigger_burst_size` (≥1; placeholder in 8T8R) |
| GetChannelFilename / SetChannelFilename | ChannelRef / ChannelFilenameValue | …Value / PlaybackConfig | One channel's `ch<N>_filename` (N=0..7; "" disables; each channel may use a different waveform) |
| GetTxPortId / SetTxPortId | PortRef / TxPortIdValue | …Value / PlaybackConfig | One port's `port<N>_tx_port_id` (N=0..1) |
| GetDestMac / SetDestMac | PortRef / DestMacValue | …Value / PlaybackConfig | One port's `port<N>_dest_mac` (N=0..1) |
| StartPlayback | Empty | StatusReply | Launch playback from current `config.ini`; error if already running |
| StopPlayback | Empty | StatusReply | Clean stop (SIGINT); error if not running |
| GetPlaybackDiagnostics | Empty | PlaybackDiagnostics | One steady-state snapshot (13 fields) |
| WatchPlaybackDiagnostics | Empty | stream PlaybackDiagnostics | Diagnostics, one update per second |

### Validation (per field)

| Field | Rule |
| --- | --- |
| core_list | comma-separated integers, each valid on the host |
| source_mode | 0 or 1 |
| ch<N>_filename | "" (disable) or a file present in `inputs/playback_waveforms/`; error names the path checked |
| target_sample_rate_hz | > 0 |
| port<N>_tx_port_id | DPDK port id 0..65535 |
| port<N>_dest_mac | parses as `XX:XX:XX:XX:XX:XX` |
| loop_count | any uint32 (0 = infinite) |
| trigger_burst_size | ≥ 1 |

## 5. How it fits together

```
gRPC client  ->  gRPC server  ->  inputs/config.ini  ->  playback executable
```

The playback executable reads **only** `inputs/config.ini` (no CLI args, no
console prompts). Setters write into that file; the executable picks the values
up at its next `StartPlayback`. If playback is already running when a setter
succeeds, the change applies at the next start.

Before starting playback, set each `port<N>_dest_mac` to the MAC address of
the FlexServer receive NIC connected to that playback port. Use
`ip -br link`, `ethtool -P <interface-name>`, or `dpdk-devbind.py --status`
on the FlexServer before binding the NIC to identify the correct MAC address.
