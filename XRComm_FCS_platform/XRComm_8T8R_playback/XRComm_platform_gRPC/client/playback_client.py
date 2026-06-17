#!/usr/bin/env python3
"""
XRComm FCS 8T8R — Playback gRPC example client.

Exercises every RPC of the PlaybackControl service: read the whole config,
get/set every field, start/stop playback, and read or stream the steady-state
diagnostics. Run `generate_stubs.sh` first so the *_pb2 modules exist.

Examples:
    python playback_client.py get-config
    python playback_client.py get core_list
    python playback_client.py set core_list 6,7,8,9,10
    python playback_client.py get-channel 3
    python playback_client.py set-channel 3 tone_512.bin
    python playback_client.py get-txport 0
    python playback_client.py set-txport 0 1
    python playback_client.py get-destmac 1
    python playback_client.py set-destmac 1 14:FE:B5:DD:9A:82
    python playback_client.py set sample_rate 1966080000
    python playback_client.py set loop_count 0
    python playback_client.py set trigger_burst_size 1
    python playback_client.py start
    python playback_client.py diagnostics
    python playback_client.py watch
    python playback_client.py stop
"""

import argparse
import sys

import grpc
import pipeline_control_pb2 as pb
import pipeline_control_pb2_grpc as pb_grpc

# scalar field name -> (Get RPC, Set RPC, value message, python cast)
SCALAR = {
    "core_list":          ("GetCoreList", "SetCoreList", pb.CoreListValue, str),
    "source_mode":        ("GetSourceMode", "SetSourceMode", pb.SourceModeValue, int),
    "sample_rate":        ("GetSampleRate", "SetSampleRate", pb.SampleRateValue, int),
    "loop_count":         ("GetLoopCount", "SetLoopCount", pb.LoopCountValue, int),
    "trigger_burst_size": ("GetTriggerBurstSize", "SetTriggerBurstSize",
                           pb.TriggerBurstSizeValue, int),
}


def print_config(c):
    print("PlaybackConfig:")
    print(f"  core_list             = {c.core_list}")
    print(f"  source_mode           = {c.source_mode}")
    for i, f in enumerate(c.ch_filename):
        print(f"  ch{i}_filename         = {f!r}")
    print(f"  target_sample_rate_hz = {c.target_sample_rate_hz}")
    for i, v in enumerate(c.tx_port_id):
        print(f"  port{i}_tx_port_id      = {v}")
    for i, v in enumerate(c.dest_mac):
        print(f"  port{i}_dest_mac        = {v}")
    print(f"  loop_count            = {c.loop_count}")
    print(f"  trigger_burst_size    = {c.trigger_burst_size}")


def print_diag(d):
    print("PlaybackDiagnostics:")
    print(f"  playback_running        = {d.playback_running}")
    print(f"  running_time_s          = {d.running_time_s:.6f}")
    print(f"  loops_completed         = {d.loops_completed}")
    print(f"  loop_count_target       = {d.loop_count_target}")
    print(f"  trigger_state           = {d.trigger_state}")
    print(f"  trigger_burst_size      = {d.trigger_burst_size}")
    print(f"  total_triggered_pkts    = {d.total_triggered_pkts}")
    print(f"  tuning_rate_hz          = {d.tuning_rate_hz}")
    print(f"  tuning_offset           = {d.tuning_offset}")
    print(f"  read_input_gbps_instant = {d.read_input_gbps_instant:.4f}")
    print(f"  read_input_gbps_average = {d.read_input_gbps_average:.4f}")
    print(f"  wire_tx_gbps_instant    = {d.wire_tx_gbps_instant:.4f}")
    print(f"  wire_tx_gbps_average    = {d.wire_tx_gbps_average:.4f}")


def main():
    ap = argparse.ArgumentParser(description="XRComm 8T8R playback gRPC client")
    ap.add_argument("--target", default="localhost:50051",
                    help="gRPC endpoint (default localhost:50051)")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("get-config")
    g = sub.add_parser("get");  g.add_argument("field", choices=SCALAR.keys())
    s = sub.add_parser("set");  s.add_argument("field", choices=SCALAR.keys()); s.add_argument("value")
    gc = sub.add_parser("get-channel"); gc.add_argument("channel", type=int)
    sc = sub.add_parser("set-channel"); sc.add_argument("channel", type=int); sc.add_argument("filename")
    gt = sub.add_parser("get-txport"); gt.add_argument("port", type=int)
    st = sub.add_parser("set-txport"); st.add_argument("port", type=int); st.add_argument("value", type=int)
    gm = sub.add_parser("get-destmac"); gm.add_argument("port", type=int)
    sm = sub.add_parser("set-destmac"); sm.add_argument("port", type=int); sm.add_argument("mac")
    sub.add_parser("start")
    sub.add_parser("stop")
    sub.add_parser("diagnostics")
    sub.add_parser("watch")
    args = ap.parse_args()

    channel = grpc.insecure_channel(args.target)
    stub = pb_grpc.PlaybackControlStub(channel)

    try:
        if args.cmd == "get-config":
            print_config(stub.GetPlaybackConfig(pb.Empty()))

        elif args.cmd == "get":
            get_rpc, _, _, _ = SCALAR[args.field]
            r = getattr(stub, get_rpc)(pb.Empty())
            print(f"{args.field} = {r.value}")

        elif args.cmd == "set":
            _, set_rpc, msg, cast = SCALAR[args.field]
            snap = getattr(stub, set_rpc)(msg(value=cast(args.value)))
            print_config(snap)

        elif args.cmd == "get-channel":
            r = stub.GetChannelFilename(pb.ChannelRef(channel=args.channel))
            print(f"ch{r.channel}_filename = {r.value!r}")

        elif args.cmd == "set-channel":
            snap = stub.SetChannelFilename(
                pb.ChannelFilenameValue(channel=args.channel, value=args.filename))
            print_config(snap)

        elif args.cmd == "get-txport":
            r = stub.GetTxPortId(pb.PortRef(port=args.port))
            print(f"port{r.port}_tx_port_id = {r.value}")

        elif args.cmd == "set-txport":
            snap = stub.SetTxPortId(pb.TxPortIdValue(port=args.port, value=args.value))
            print_config(snap)

        elif args.cmd == "get-destmac":
            r = stub.GetDestMac(pb.PortRef(port=args.port))
            print(f"port{r.port}_dest_mac = {r.value}")

        elif args.cmd == "set-destmac":
            snap = stub.SetDestMac(pb.DestMacValue(port=args.port, value=args.mac))
            print_config(snap)

        elif args.cmd == "start":
            r = stub.StartPlayback(pb.Empty())
            print(f"ok={r.ok} {r.message}")

        elif args.cmd == "stop":
            r = stub.StopPlayback(pb.Empty())
            print(f"ok={r.ok} {r.message}")

        elif args.cmd == "diagnostics":
            print_diag(stub.GetPlaybackDiagnostics(pb.Empty()))

        elif args.cmd == "watch":
            for d in stub.WatchPlaybackDiagnostics(pb.Empty()):
                print_diag(d)
                print("-" * 40)

    except grpc.RpcError as e:
        print(f"[gRPC error] {e.code().name}: {e.details()}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
