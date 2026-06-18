# XRComm 8T8R — Playback gRPC server (PlaybackControl)

Customer entrypoint for the playback control service. This script is shipped
with the package and is intended to be run directly.

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
* `GetPlaybackDiagnostics` / `WatchPlaybackDiagnostics` return the steady-state
  playback metrics. For 8T8R the read/transmit figures are the combined
  both-port totals.

## Prerequisites

```bash
pip install grpcio grpcio-tools
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
