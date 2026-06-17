#!/usr/bin/env python3
# ============================================================================
#  playback_grpc_client.py  —  XRComm FCS wideband platform
#
#  Example gRPC client for the PLAYBACK control surface (Sections 8.3 & 8.4).
#  Exercises every playback RPC: the read-only snapshot, a get + set for each
#  of the eight config.ini fields, start/stop, and one-shot + streaming
#  diagnostics.
#
#  Generate the stubs first (they are NOT shipped):
#     python -m grpc_tools.protoc -I../proto \
#         --python_out=. --grpc_python_out=. ../proto/pipeline_control.proto
#
#  Then, with the platform gRPC server running:
#     python playback_grpc_client.py get-config
#     python playback_grpc_client.py get sample_rate
#     python playback_grpc_client.py set sample_rate 1474560000
#     python playback_grpc_client.py set binary_filename tone_512.bin
#     python playback_grpc_client.py start
#     python playback_grpc_client.py diag
#     python playback_grpc_client.py watch
#     python playback_grpc_client.py stop
# ============================================================================

import argparse
import sys

import grpc
import pipeline_control_pb2 as pb
import pipeline_control_pb2_grpc as pb_grpc

DEFAULT_ENDPOINT = "localhost:50051"

# field name -> (Get RPC, Set RPC, value message ctor, python-value parser)
FIELDS = {
    "core_list":          ("GetCoreList",         "SetCoreList",         pb.CoreListValue,         str),
    "source_mode":        ("GetSourceMode",       "SetSourceMode",       pb.SourceModeValue,       int),
    "binary_filename":    ("GetBinaryFilename",   "SetBinaryFilename",   pb.BinaryFilenameValue,   str),
    "sample_rate":        ("GetSampleRate",       "SetSampleRate",       pb.SampleRateValue,       int),
    "tx_port_id":         ("GetTxPortId",         "SetTxPortId",         pb.TxPortIdValue,         int),
    "dest_mac":           ("GetDestMac",          "SetDestMac",          pb.DestMacValue,          str),
    "loop_count":         ("GetLoopCount",        "SetLoopCount",        pb.LoopCountValue,        int),
    "trigger_burst_size": ("GetTriggerBurstSize", "SetTriggerBurstSize", pb.TriggerBurstSizeValue, int),
}


def print_config(cfg):
    print("PlaybackConfig:")
    print(f"  core_list             = {cfg.core_list}")
    print(f"  source_mode           = {cfg.source_mode}")
    print(f"  binary_filename       = {cfg.binary_filename}")
    print(f"  target_sample_rate_hz = {cfg.target_sample_rate_hz}")
    print(f"  tx_port_id            = {cfg.tx_port_id}")
    print(f"  dest_mac              = {cfg.dest_mac}")
    print(f"  loop_count            = {cfg.loop_count}")
    print(f"  trigger_burst_size    = {cfg.trigger_burst_size}")


def print_diag(d):
    print("PlaybackDiagnostics:")
    print(f"  playback_running          = {d.playback_running}")
    print(f"  running_time_s            = {d.running_time_s:.4f}")
    print(f"  loops_completed           = {d.loops_completed}")
    print(f"  loop_count_target         = {d.loop_count_target}")
    print(f"  trigger_state             = {d.trigger_state}")
    print(f"  trigger_burst_size        = {d.trigger_burst_size}")
    print(f"  total_triggered_pkts      = {d.total_triggered_pkts}")
    print(f"  tuning_rate_hz            = {d.tuning_rate_hz}")
    print(f"  tuning_offset             = {d.tuning_offset}")
    print(f"  read_input_gbps_instant   = {d.read_input_gbps_instant:.4f}")
    print(f"  read_input_gbps_average   = {d.read_input_gbps_average:.4f}")
    print(f"  wire_tx_gbps_instant      = {d.wire_tx_gbps_instant:.4f}")
    print(f"  wire_tx_gbps_average      = {d.wire_tx_gbps_average:.4f}")


def main():
    ap = argparse.ArgumentParser(description="XRComm wideband playback gRPC client")
    ap.add_argument("--endpoint", default=DEFAULT_ENDPOINT)
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("get-config", help="read all eight values (snapshot)")

    p_get = sub.add_parser("get", help="read one field")
    p_get.add_argument("field", choices=FIELDS.keys())

    p_set = sub.add_parser("set", help="write one field")
    p_set.add_argument("field", choices=FIELDS.keys())
    p_set.add_argument("value")

    sub.add_parser("start", help="StartPlayback")
    sub.add_parser("stop", help="StopPlayback")
    sub.add_parser("diag", help="GetPlaybackDiagnostics (one snapshot)")
    sub.add_parser("watch", help="WatchPlaybackDiagnostics (stream, Ctrl+C to stop)")

    args = ap.parse_args()

    channel = grpc.insecure_channel(args.endpoint)
    stub = pb_grpc.PipelineControlStub(channel)

    try:
        if args.cmd == "get-config":
            print_config(stub.GetPlaybackConfig(pb.Empty()))

        elif args.cmd == "get":
            get_rpc = FIELDS[args.field][0]
            resp = getattr(stub, get_rpc)(pb.Empty())
            print(f"{args.field} = {resp.value}")

        elif args.cmd == "set":
            _, set_rpc, ctor, parse = FIELDS[args.field]
            msg = ctor(value=parse(args.value))
            # capture trailing metadata (e.g. "takes effect at next StartPlayback")
            call = getattr(stub, set_rpc).with_call(msg)
            cfg, rendezvous = call
            for k, v in rendezvous.trailing_metadata() or []:
                if k == "note":
                    print(f"[note] {v}")
            print_config(cfg)

        elif args.cmd == "start":
            r = stub.StartPlayback(pb.Empty())
            print(f"start: ok={r.ok} {r.message}")

        elif args.cmd == "stop":
            r = stub.StopPlayback(pb.Empty())
            print(f"stop: ok={r.ok} {r.message}")

        elif args.cmd == "diag":
            print_diag(stub.GetPlaybackDiagnostics(pb.Empty()))

        elif args.cmd == "watch":
            print("Streaming diagnostics (Ctrl+C to stop)...")
            for d in stub.WatchPlaybackDiagnostics(pb.Empty()):
                print_diag(d)
                print("-" * 40)

    except grpc.RpcError as e:
        print(f"[gRPC error] {e.code().name}: {e.details()}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()