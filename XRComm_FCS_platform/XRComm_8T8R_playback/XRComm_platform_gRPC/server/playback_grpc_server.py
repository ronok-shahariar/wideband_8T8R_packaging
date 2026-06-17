#!/usr/bin/env python3
"""
XRComm FCS 8T8R — Playback gRPC server (PlaybackControl service).

Implements Sections 8.3 and 8.4 of the Customer Package Preparation Guide for
the 8T8R playback tree:

  * Per-field Get/Set of every inputs/config.ini key. Each Set VALIDATES one
    value, rewrites ONLY its own key inside inputs/config.ini (all other keys
    stay byte-for-byte untouched), and returns the full PlaybackConfig snapshot.
  * StartPlayback / StopPlayback drive the playback executable as a child
    process (working directory = playback tree root, so the relative inputs/
    path resolves). Each returns a gRPC error if already in the requested state.
  * GetPlaybackDiagnostics / WatchPlaybackDiagnostics read the once-per-second
    JSON status file the playback executable publishes (playback_status.json in
    the playback tree root).

The gRPC server is the ONLY writer of inputs/config.ini. The playback
executable only reads that file, at its next start. Chain:
    gRPC client -> gRPC server -> inputs/config.ini -> playback executable

NOTE: in the binary-only customer bundle this server is shipped as the
xrcomm_grpc_ctrl binary; this Python implementation is the engineering source.
"""

import argparse
import json
import os
import re
import signal
import subprocess
import sys
import time
from concurrent import futures

import grpc

# Generated stubs (run generate_stubs.sh first).
import pipeline_control_pb2 as pb
import pipeline_control_pb2_grpc as pb_grpc

N_CHANNELS = 8
N_PORTS = 2

# --- config.ini defaults (mirror the C loader so a snapshot is always complete)
DEFAULTS = {
    "core_list": "6,7,8,9,10",
    "source_mode": 1,
    "target_sample_rate_hz": 1966080000,
    "loop_count": 0,
    "trigger_burst_size": 1,
    "tx_port_id": [1, 0],
    "dest_mac": ["14:FE:B5:DD:9A:82", "14:FE:B5:DD:9A:82"],
}

_MAC_RE = re.compile(r"^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$")


# ─────────────────────────────────────────────────────────────────────────────
# config.ini read / surgical write
# ─────────────────────────────────────────────────────────────────────────────
def _parse_config(path):
    """Return {key: value_str} from config.ini (comments stripped, last wins)."""
    d = {}
    if not os.path.exists(path):
        print(f"[gRPC][WARN] config file not found: {path} — returning DEFAULTS "
              f"only. Set --playback-root (or XRCOMM_PLAYBACK_ROOT) to the tree "
              f"that contains inputs/config.ini.", file=sys.stderr)
        return d
    with open(path, "r") as f:
        for line in f:
            code = line.split("#", 1)[0]
            if "=" not in code:
                continue
            k, v = code.split("=", 1)
            k, v = k.strip(), v.strip()
            if k:
                d[k] = v
    return d


def _rewrite_key(path, key, value):
    """Rewrite ONLY `key`'s line in config.ini; every other byte is preserved.

    If the key is absent it is appended. An inline comment on the edited line
    is preserved.
    """
    value = str(value)
    with open(path, "r") as f:
        lines = f.readlines()

    pat = re.compile(
        r"^(?P<indent>\s*)" + re.escape(key) +
        r"(?P<sp>\s*)=(?P<sp2>\s*)(?P<val>[^#\n]*)(?P<comment>#.*)?(?P<nl>\r?\n?)$"
    )
    found = False
    for i, line in enumerate(lines):
        m = pat.match(line)
        if not m:
            continue
        nl = m.group("nl") or "\n"
        comment = m.group("comment") or ""
        sep = "  " + comment if comment else ""
        lines[i] = f"{m.group('indent')}{key} = {value}{sep}{nl}"
        found = True
        break

    if not found:
        if lines and not lines[-1].endswith("\n"):
            lines[-1] += "\n"
        lines.append(f"{key} = {value}\n")

    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        f.writelines(lines)
    os.replace(tmp, path)


# ─────────────────────────────────────────────────────────────────────────────
# snapshot builder
# ─────────────────────────────────────────────────────────────────────────────
def _snapshot(cfg_path):
    raw = _parse_config(cfg_path)

    def s(key, default):
        return raw.get(key, default)

    def i(key, default):
        try:
            return int(raw[key])
        except (KeyError, ValueError):
            return default

    ch = [raw.get(f"ch{n}_filename", "") for n in range(N_CHANNELS)]
    tx = [
        int(raw[f"port{n}_tx_port_id"]) if f"port{n}_tx_port_id" in raw
        else DEFAULTS["tx_port_id"][n]
        for n in range(N_PORTS)
    ]
    mac = [raw.get(f"port{n}_dest_mac", DEFAULTS["dest_mac"][n]) for n in range(N_PORTS)]

    return pb.PlaybackConfig(
        core_list=s("core_list", DEFAULTS["core_list"]),
        source_mode=i("source_mode", DEFAULTS["source_mode"]),
        ch_filename=ch,
        target_sample_rate_hz=int(s("target_sample_rate_hz",
                                    DEFAULTS["target_sample_rate_hz"])),
        tx_port_id=tx,
        dest_mac=mac,
        loop_count=i("loop_count", DEFAULTS["loop_count"]),
        trigger_burst_size=i("trigger_burst_size", DEFAULTS["trigger_burst_size"]),
    )


# ─────────────────────────────────────────────────────────────────────────────
# service implementation
# ─────────────────────────────────────────────────────────────────────────────
class PlaybackControl(pb_grpc.PlaybackControlServicer):
    def __init__(self, root):
        self.root = os.path.abspath(root)
        self.cfg_path = os.path.join(self.root, "inputs", "config.ini")
        self.wave_dir = os.path.join(self.root, "inputs", "playback_waveforms")
        self.binary = os.path.join(self.root, "bin", "xrcomm_playback")
        self.status_path = os.path.join(self.root, "playback_status.json")
        self.proc = None

    # -- helpers --------------------------------------------------------------
    def _running(self):
        return self.proc is not None and self.proc.poll() is None

    def _set_and_snapshot(self, context, key, value):
        if not os.path.exists(self.cfg_path):
            context.abort(grpc.StatusCode.FAILED_PRECONDITION,
                          f"config file not found: {self.cfg_path}")
        _rewrite_key(self.cfg_path, key, value)
        snap = _snapshot(self.cfg_path)
        return snap

    def _bad(self, context, field, msg):
        context.abort(grpc.StatusCode.INVALID_ARGUMENT, f"{field}: {msg}")

    # -- whole snapshot -------------------------------------------------------
    def GetPlaybackConfig(self, request, context):
        return _snapshot(self.cfg_path)

    # -- core_list ------------------------------------------------------------
    def GetCoreList(self, request, context):
        return pb.CoreListValue(value=_snapshot(self.cfg_path).core_list)

    def SetCoreList(self, request, context):
        v = request.value.strip()
        parts = [p.strip() for p in v.split(",") if p.strip() != ""]
        if not parts:
            self._bad(context, "core_list", "comma-separated integer core list expected")
        ncpu = os.cpu_count() or 0
        for p in parts:
            if not p.isdigit():
                self._bad(context, "core_list", f"'{p}' is not an integer core id")
            if ncpu and int(p) >= ncpu:
                self._bad(context, "core_list",
                          f"core {p} not valid on host (0..{ncpu - 1})")
        return self._set_and_snapshot(context, "core_list", ",".join(parts))

    # -- source_mode ----------------------------------------------------------
    def GetSourceMode(self, request, context):
        return pb.SourceModeValue(value=_snapshot(self.cfg_path).source_mode)

    def SetSourceMode(self, request, context):
        if request.value not in (0, 1):
            self._bad(context, "source_mode", "must be 0 (NVMe) or 1 (file playback)")
        return self._set_and_snapshot(context, "source_mode", request.value)

    # -- target_sample_rate_hz ------------------------------------------------
    def GetSampleRate(self, request, context):
        return pb.SampleRateValue(
            value=_snapshot(self.cfg_path).target_sample_rate_hz)

    def SetSampleRate(self, request, context):
        if request.value == 0:
            self._bad(context, "target_sample_rate_hz", "must be > 0 (Hz)")
        return self._set_and_snapshot(context, "target_sample_rate_hz", request.value)

    # -- loop_count -----------------------------------------------------------
    def GetLoopCount(self, request, context):
        return pb.LoopCountValue(value=_snapshot(self.cfg_path).loop_count)

    def SetLoopCount(self, request, context):
        # any uint32; 0 = infinite (proto already constrains to uint32)
        return self._set_and_snapshot(context, "loop_count", request.value)

    # -- trigger_burst_size ---------------------------------------------------
    def GetTriggerBurstSize(self, request, context):
        return pb.TriggerBurstSizeValue(
            value=_snapshot(self.cfg_path).trigger_burst_size)

    def SetTriggerBurstSize(self, request, context):
        if request.value < 1:
            self._bad(context, "trigger_burst_size", "must be >= 1")
        return self._set_and_snapshot(context, "trigger_burst_size", request.value)

    # -- per-channel filename -------------------------------------------------
    def GetChannelFilename(self, request, context):
        ch = request.channel
        if not (0 <= ch < N_CHANNELS):
            self._bad(context, "channel", f"must be 0..{N_CHANNELS - 1}")
        snap = _snapshot(self.cfg_path)
        return pb.ChannelFilenameValue(channel=ch, value=snap.ch_filename[ch])

    def SetChannelFilename(self, request, context):
        ch = request.channel
        if not (0 <= ch < N_CHANNELS):
            self._bad(context, "channel", f"must be 0..{N_CHANNELS - 1}")
        val = request.value.strip()
        if val != "":
            checked = val if val.startswith("/") else os.path.join(self.wave_dir, val)
            if not os.path.exists(checked):
                self._bad(context, f"ch{ch}_filename",
                          f"file not found: {checked}")
        return self._set_and_snapshot(context, f"ch{ch}_filename", val)

    # -- per-port tx_port_id --------------------------------------------------
    def GetTxPortId(self, request, context):
        p = request.port
        if not (0 <= p < N_PORTS):
            self._bad(context, "port", f"must be 0..{N_PORTS - 1}")
        return pb.TxPortIdValue(port=p, value=_snapshot(self.cfg_path).tx_port_id[p])

    def SetTxPortId(self, request, context):
        p = request.port
        if not (0 <= p < N_PORTS):
            self._bad(context, "port", f"must be 0..{N_PORTS - 1}")
        if request.value > 65535:
            self._bad(context, f"port{p}_tx_port_id", "must be a DPDK port id (0..65535)")
        return self._set_and_snapshot(context, f"port{p}_tx_port_id", request.value)

    # -- per-port dest_mac ----------------------------------------------------
    def GetDestMac(self, request, context):
        p = request.port
        if not (0 <= p < N_PORTS):
            self._bad(context, "port", f"must be 0..{N_PORTS - 1}")
        return pb.DestMacValue(port=p, value=_snapshot(self.cfg_path).dest_mac[p])

    def SetDestMac(self, request, context):
        p = request.port
        if not (0 <= p < N_PORTS):
            self._bad(context, "port", f"must be 0..{N_PORTS - 1}")
        if not _MAC_RE.match(request.value.strip()):
            self._bad(context, f"port{p}_dest_mac", "must parse as XX:XX:XX:XX:XX:XX")
        return self._set_and_snapshot(context, f"port{p}_dest_mac",
                                      request.value.strip())

    # -- process control ------------------------------------------------------
    def StartPlayback(self, request, context):
        if self._running():
            context.abort(grpc.StatusCode.FAILED_PRECONDITION,
                          "playback already running")
        if not os.path.exists(self.binary):
            context.abort(grpc.StatusCode.FAILED_PRECONDITION,
                          f"playback binary not found: {self.binary}")
        # Working directory = playback tree root so 'inputs/...' resolves.
        self.proc = subprocess.Popen([self.binary], cwd=self.root)
        return pb.StatusReply(ok=True,
                              message=f"playback started (pid {self.proc.pid})")

    def StopPlayback(self, request, context):
        if not self._running():
            context.abort(grpc.StatusCode.FAILED_PRECONDITION,
                          "playback not running")
        self.proc.send_signal(signal.SIGINT)   # clean shutdown -> prints summary
        try:
            self.proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait(timeout=5)
        pid = self.proc.pid
        self.proc = None
        return pb.StatusReply(ok=True, message=f"playback stopped (pid {pid})")

    # -- diagnostics ----------------------------------------------------------
    def _read_diag(self):
        d = {}
        try:
            with open(self.status_path, "r") as f:
                d = json.load(f)
        except (OSError, ValueError):
            d = {}
        running = bool(d.get("playback_running", False)) and self._running()
        return pb.PlaybackDiagnostics(
            playback_running=running,
            running_time_s=float(d.get("running_time_s", 0.0)),
            loops_completed=int(d.get("loops_completed", 0)),
            loop_count_target=int(d.get("loop_count_target", 0)),
            trigger_state=str(d.get("trigger_state", "IDLE")),
            trigger_burst_size=int(d.get("trigger_burst_size", 1)),
            total_triggered_pkts=int(d.get("total_triggered_pkts", 0)),
            tuning_rate_hz=int(d.get("tuning_rate_hz", 0)),
            tuning_offset=int(d.get("tuning_offset", 0)),
            read_input_gbps_instant=float(d.get("read_input_gbps_instant", 0.0)),
            read_input_gbps_average=float(d.get("read_input_gbps_average", 0.0)),
            wire_tx_gbps_instant=float(d.get("wire_tx_gbps_instant", 0.0)),
            wire_tx_gbps_average=float(d.get("wire_tx_gbps_average", 0.0)),
        )

    def GetPlaybackDiagnostics(self, request, context):
        return self._read_diag()

    def WatchPlaybackDiagnostics(self, request, context):
        while context.is_active():
            yield self._read_diag()
            time.sleep(1.0)


def serve():
    ap = argparse.ArgumentParser(
        description="XRComm 8T8R playback gRPC server",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Default playback root is computed relative to this script's location:
  <this file>/../../../XRComm_8T8R_playback
i.e. the sibling of XRComm_FCS_8T8R_platform/ at the bundle root.
No --playback-root flag is needed when running from the standard bundle layout.
""")
    # Compute relative default once, anchored to THIS file's location so it
    # works regardless of the shell's current working directory.
    #
    # Bundle layout:
    #   <bundle_root>/
    #     XRComm_8T8R_playback/          <- playback tree
    #     XRComm_FCS_8T8R_platform/
    #       XRComm_platform_gRPC/
    #         server/
    #           playback_grpc_server.py  <- THIS FILE
    #
    # Three levels up from this file lands on <bundle_root>.
    _server_dir = os.path.dirname(os.path.abspath(__file__))
    _default_root = os.path.normpath(
        os.path.join(_server_dir, "..", "..", "..", "XRComm_8T8R_playback")
    )
    ap.add_argument(
        "--playback-root",
        default=_default_root,
        help=f"Path to XRComm_8T8R_playback/ (default: {_default_root})",
    )
    ap.add_argument("--listen", default="[::]:50051",
                    help="gRPC listen address (default [::]:50051).")
    args = ap.parse_args()

    server = grpc.server(futures.ThreadPoolExecutor(max_workers=8))
    pb_grpc.add_PlaybackControlServicer_to_server(
        PlaybackControl(args.playback_root), server)
    server.add_insecure_port(args.listen)
    server.start()
    print(f"[gRPC] PlaybackControl listening on {args.listen}")
    print(f"[gRPC] playback root: {os.path.abspath(args.playback_root)}")
    _cfg = os.path.join(os.path.abspath(args.playback_root), "inputs", "config.ini")
    if os.path.exists(_cfg):
        print(f"[gRPC] config file : {_cfg}  (found)")
    else:
        print(f"[gRPC] config file : {_cfg}  *** NOT FOUND — get-config will show "
              f"defaults only. Fix --playback-root. ***")
    try:
        server.wait_for_termination()
    except KeyboardInterrupt:
        print("\n[gRPC] shutting down")
        server.stop(2).wait()


if __name__ == "__main__":
    serve()