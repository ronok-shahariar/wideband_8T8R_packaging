# XRComm 8T8R — Playback gRPC server (PlaybackControl)

Engineering source for the playback control service. In the binary-only
customer bundle this is delivered as the `xrcomm_grpc_ctrl` binary; only the
`.proto` and the example client ship as source.

## What it does

* Reads/writes `inputs/config.ini` for the 8T8R playback tree. It is the **only**
  writer of that file. Each per-field `Set` validates one value and rewrites
  **only that key's line** — every other byte of `config.ini` (including
  comments) is preserved. Invalid values return `INVALID_ARGUMENT` and the file
  is not touched.
* `StartPlayback` launches `bin/xrcomm_playback` with the working directory set
  to the playback tree root, so the relative `inputs/...` paths resolve.
  `StopPlayback` sends SIGINT and waits for a clean exit. Each returns a gRPC
  error if playback is already in the requested state.
* `GetPlaybackDiagnostics` / `WatchPlaybackDiagnostics` read the once-per-second
  `playback_status.json` that the playback executable publishes in the tree
  root, and return the 13 Table-D fields. For 8T8R the read/transmit figures are
  the combined both-port totals; `trigger_state` is always `IDLE` and
  `total_triggered_pkts` is 0 (runtime per-channel `t<CH>,<offset>` triggers do
  not use a burst state).

## Prerequisites

```bash
pip install grpcio grpcio-tools
../generate_stubs.sh        # generates *_pb2 modules into server/ and client/
```

## Run

```bash
python playback_grpc_server.py \
    --playback-root /path/to/XRComm_FCS_platform/8T8R/playback \
    --listen [::]:50051
```

`--playback-root` defaults to the sibling `8T8R/playback/` tree (or the
`XRCOMM_PLAYBACK_ROOT` environment variable). The root must contain `inputs/`
(with `config.ini` and `playback_waveforms/`) and `bin/xrcomm_playback`.
