#!/usr/bin/env python3
"""
xrcomm_grpc_client.py — XRComm 8T8R Platform gRPC Example Client

Usage:
    python3 xrcomm_grpc_client.py [--host HOST] [--port PORT] <command> [args]

Commands:
    get-flags                   Read current mode flags
    set-dsp <on|off>            Enable/disable DSP (FULL_PACKET_MODE) — independent
    set-logging <on|off>        Enable/disable NVMe logging — independent
    set-ip <on|off>             Enable/disable IP mode (IQ_MODE) — independent
    get-stats                   Get combined stats with per-channel 8T8R telemetry
    list-ips                    List registered secondary IQ applications
    watch-status [N]            Stream live status updates (default 10 updates)
    shutdown                    Stop the platform pipeline cleanly

Notes:
    - All three modes (DSP, logging, IP) are independent and may run simultaneously.
    - Per-channel telemetry reports one entry per logical channel (0..7).
    - Channel = hsp_port * 4 + per_port_channel_index.

Generate stubs from proto:
    python3 -m grpc_tools.protoc -I../proto --python_out=. \\
        --grpc_python_out=. ../proto/pipeline_control.proto
"""

import sys
import os
import argparse
import grpc

# Stubs are generated from pipeline_control.proto — not shipped pre-compiled.
# Run: python3 -m grpc_tools.protoc -I../proto --python_out=. \\
#             --grpc_python_out=. ../proto/pipeline_control.proto
try:
    import pipeline_control_pb2 as pb2
    import pipeline_control_pb2_grpc as pb2_grpc
except ImportError:
    print("ERROR: gRPC stubs not found. Generate them with:")
    print("  python3 -m grpc_tools.protoc -I../proto \\")
    print("      --python_out=. --grpc_python_out=. ../proto/pipeline_control.proto")
    sys.exit(1)


def make_channel(host: str, port: int):
    return grpc.insecure_channel(f"{host}:{port}")


def print_flags(flags):
    """Pretty-print PipelineFlags."""
    print(f"  dsp_mode (FULL_PACKET_MODE): {'ON' if flags.enable_dsp_mode else 'off'}")
    print(f"  logging  (NVMe IQ capture):  {'ON' if flags.enable_logging  else 'off'}")
    print(f"  ip_mode  (IQ_MODE dispatch): {'ON' if flags.enable_ip_mode  else 'off'}")
    print(f"  rt_loop:                     {'ON' if flags.execute_rt_loop else 'off'}")
    print("  (all modes independent — may all be ON simultaneously)")


def cmd_get_flags(stub):
    resp = stub.GetFlags(pb2.Empty())
    print("=== Mode Flags ===")
    print_flags(resp)


def cmd_set_mode(stub, rpc_name: str, value: bool):
    req = pb2.BoolRequest(value=value)
    if rpc_name == "dsp":
        resp = stub.SetDspMode(req)
        label = "DSP (FULL_PACKET_MODE)"
    elif rpc_name == "logging":
        resp = stub.SetLogging(req)
        label = "NVMe logging"
    elif rpc_name == "ip":
        resp = stub.SetIpMode(req)
        label = "IP mode (IQ_MODE)"
    else:
        print(f"Unknown mode: {rpc_name}")
        return
    print(f"=== Set {label} {'ON' if value else 'off'} ===")
    print_flags(resp)


def cmd_get_stats(stub):
    resp = stub.GetStats(pb2.Empty())
    print("=== Combined Stats ===")

    # DSP
    d = resp.dsp
    print(f"\nDSP (FULL_PACKET_MODE):")
    print(f"  ready={d.dsp_ready}  epochs={d.epochs_processed}  "
          f"dropped={d.epochs_dropped}  peaks={d.peaks_detected}")
    print(f"  cfo={d.last_cfo_hz:.2f} Hz  rssi={d.last_rssi:.1f}")

    # Per-channel 8T8R telemetry
    print(f"\n8T8R Per-Channel Telemetry ({len(resp.channel_telemetry)} channels):")
    print(f"  {'Port':>4}  {'Ch':>4}  {'Samples_Rx':>14}  {'Logged':>12}  "
          f"{'Processed_2ndary':>18}  {'Pkts_Dropped':>14}")
    for t in resp.channel_telemetry:
        print(f"  {t.hsp_port:>4}  {t.channel:>4}  {t.samples_received:>14}  "
              f"{t.samples_logged:>12}  {t.samples_processed_secondary:>18}  "
              f"{t.packets_dropped:>14}")

    # Per-IP secondary stats
    if resp.spectrum:
        print(f"\nRegistered IQ Applications ({len(resp.spectrum)} slots):")
        for s in resp.spectrum:
            ch_str = "all" if s.channel == 255 else str(s.channel)
            print(f"  Slot[{s.slot_id}] '{s.name}' port={s.hsp_port} ch={ch_str}")
            print(f"    pushed={s.batches_pushed}  dropped={s.batches_dropped}  "
                  f"rx_self={s.batches_received}  proc={s.batches_processed}")
            print(f"    power={s.last_power_dbfs:.2f}dBFS  mean={s.mean_power_dbfs:.2f}dBFS")
    else:
        print("\nRegistered IQ Applications: (none)")


def cmd_list_ips(stub):
    resp = stub.ListSecondaryIPs(pb2.Empty())
    print("=== Registered Secondary IQ Applications ===")
    if not resp.entries:
        print("  (none registered)")
        return
    for e in resp.entries:
        ch_str = "all" if e.channel == 255 else str(e.channel)
        print(f"  slot={e.slot_id}  name={e.name:<24}  pid={e.pid:<6}  "
              f"mode={e.rx_mode:<18}  port={e.hsp_port}  ch={ch_str:<3}  "
              f"ring={'ready' if e.ring_ready else 'wait'}  "
              f"active={'yes' if e.active else 'no'}")


def cmd_watch_status(stub, n_updates: int = 10):
    print(f"=== Live Status (streaming {n_updates} updates) ===")
    count = 0
    for ps in stub.WatchStatus(pb2.Empty()):
        f = ps.flags
        uptime_s = ps.uptime_ms // 1000
        print(f"\n[{uptime_s:>6}s] running={ps.running}  "
              f"dsp={'ON' if f.enable_dsp_mode else 'off'}  "
              f"logging={'ON' if f.enable_logging else 'off'}  "
              f"ip={'ON' if f.enable_ip_mode else 'off'}")
        for t in ps.stats.channel_telemetry:
            if t.samples_received or t.packets_dropped:
                print(f"         ch[{t.hsp_port}/{t.channel}] "
                      f"rx={t.samples_received}  logged={t.samples_logged}  "
                      f"dropped={t.packets_dropped}")
        count += 1
        if count >= n_updates:
            break


def cmd_shutdown(stub):
    resp = stub.Shutdown(pb2.Empty())
    if resp.ok:
        print("Pipeline shutdown requested successfully.")
    else:
        print(f"Shutdown failed: {resp.error}")


def main():
    parser = argparse.ArgumentParser(
        description="XRComm 8T8R Platform gRPC Client")
    parser.add_argument("--host", default="127.0.0.1",
                        help="gRPC server host (default: 127.0.0.1)")
    parser.add_argument("--port", type=int, default=50051,
                        help="gRPC server port (default: 50051)")
    parser.add_argument("command", nargs="?",
                        help="Command to run (see module docstring)")
    parser.add_argument("args", nargs="*",
                        help="Command arguments")
    args = parser.parse_args()

    if not args.command:
        print(__doc__)
        sys.exit(0)

    channel = make_channel(args.host, args.port)
    stub    = pb2_grpc.PipelineControlStub(channel)

    cmd = args.command.lower()

    try:
        if cmd == "get-flags":
            cmd_get_flags(stub)
        elif cmd in ("set-dsp", "set-logging", "set-ip"):
            if not args.args:
                print(f"Usage: {cmd} <on|off>")
                sys.exit(1)
            value = args.args[0].lower() == "on"
            mode_key = cmd.replace("set-", "")
            cmd_set_mode(stub, mode_key, value)
        elif cmd == "get-stats":
            cmd_get_stats(stub)
        elif cmd == "list-ips":
            cmd_list_ips(stub)
        elif cmd == "watch-status":
            n = int(args.args[0]) if args.args else 10
            cmd_watch_status(stub, n)
        elif cmd == "shutdown":
            cmd_shutdown(stub)
        else:
            print(f"Unknown command: {cmd}")
            print(__doc__)
            sys.exit(1)
    except grpc.RpcError as e:
        print(f"gRPC error ({e.code()}): {e.details()}")
        sys.exit(1)

    channel.close()


if __name__ == "__main__":
    main()
