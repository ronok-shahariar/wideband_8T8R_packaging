#!/usr/bin/env python3
# =============================================================================
# xrcomm_grpc_client.py — Standalone remote control client for the XRComm
#                         FCS wideband platform gRPC server.
#
# This is a SEPARATE application from the platform gRPC server. It holds no
# shared memory and makes no local assumptions: it connects over the network to
# a running xrcomm_grpc_ctrl server (default <host>:50051) and exercises every
# RPC. Run it from any host that can reach the platform.
#
# One-time stub generation (see README.md):
#   python -m grpc_tools.protoc -I. --python_out=. --grpc_python_out=. \
#          pipeline_control.proto
#
# Examples:
#   ./xrcomm_grpc_client.py --host 192.168.1.10:50051 get-flags
#   ./xrcomm_grpc_client.py --host 192.168.1.10:50051 set-dsp on
#   ./xrcomm_grpc_client.py --host 192.168.1.10:50051 get-stats
#   ./xrcomm_grpc_client.py --host 192.168.1.10:50051 watch-status
#   ./xrcomm_grpc_client.py --host 192.168.1.10:50051 shutdown
# =============================================================================
import argparse
import sys

try:
    import grpc
    import pipeline_control_pb2 as pb
    import pipeline_control_pb2_grpc as pbg
except ImportError as e:
    sys.exit(
        f"Import failed ({e}).\n"
        "Generate the stubs first:\n"
        "  python -m grpc_tools.protoc -I. --python_out=. "
        "--grpc_python_out=. pipeline_control.proto\n"
        "and install deps:  pip install -r requirements.txt"
    )

DEFAULT_HOST = "localhost:50051"


def _on(val: str) -> bool:
    return str(val).strip().lower() in ("on", "1", "true", "yes")


def _print_flags(f):
    print(
        f"flags: dsp={'ON' if f.enable_dsp_mode else 'off'}  "
        f"ip={'ON' if f.enable_ip_mode else 'off'}  "
        f"logging={'ON' if f.enable_logging else 'off'}  "
        f"rt_loop={'ON' if f.execute_rt_loop else 'off'}"
    )


def _print_stats(cs):
    t = cs.platform_telemetry
    print("platform telemetry:")
    print(f"  hsp_port                     = {t.hsp_port}")
    print(f"  samples_received             = {t.samples_received}")
    print(f"  samples_logged               = {t.samples_logged}")
    print(f"  samples_processed_secondary  = {t.samples_processed_secondary}")
    print(f"  packets_dropped              = {t.packets_dropped}")
    d = cs.dsp
    print("dsp:")
    print(f"  ready={d.dsp_ready} epochs_proc={d.epochs_processed} "
          f"epochs_drop={d.epochs_dropped} peaks={d.peaks_detected} "
          f"cfo_hz={d.last_cfo_hz:.4f}")
    if len(cs.spectrum):
        print("spectrum applications:")
        for s in cs.spectrum:
            print(f"  slot={s.slot_id} {s.name} rx={s.batches_received} "
                  f"proc={s.batches_processed} drop={s.batches_dropped} "
                  f"power={s.last_power_dbfs:.2f} dBFS")


def cmd_get_flags(stub, _):
    _print_flags(stub.GetFlags(pb.Empty()))


def cmd_set(stub, args):
    req = pb.BoolRequest(value=_on(args.value))
    rpc = {"set-dsp": stub.SetDspMode,
           "set-ip": stub.SetIpMode,
           "set-logging": stub.SetLogging}[args.command]
    _print_flags(rpc(req))


def cmd_get_stats(stub, _):
    _print_stats(stub.GetStats(pb.Empty()))


def cmd_get_mode_stats(stub, _):
    ms = stub.GetModeStats(pb.Empty())
    for c in (ms.full_packet, ms.iq, ms.log_block):
        print(f"  {c.mode:<12} rx={c.rx} proc={c.processed} drops={c.drops}")
    for a in ms.apps:
        print(f"  app slot={a.slot_id} {a.name} mode={a.rx_mode} "
              f"rx={a.rx} proc={a.processed} drops={a.drops} "
              f"{'active' if a.active else 'inactive'}")


def cmd_list_ips(stub, _):
    r = stub.ListSecondaryIPs(pb.Empty())
    for e in r.entries:
        print(f"  slot={e.slot_id} {e.name} mode={e.rx_mode} "
              f"{'active' if e.active else 'inactive'}")


def cmd_set_read_capture(stub, args):
    try:
        seconds = float(args.value)
    except (TypeError, ValueError):
        sys.exit("usage: set-read-capture <seconds> [--start-offset <seconds>]")
    try:
        start_offset = float(args.start_offset)
    except (TypeError, ValueError):
        sys.exit("--start-offset must be a number of seconds (offset from the "
                  "start of the capture, not a wall-clock time)")
    r = stub.SetReadCapture(pb.ReadCaptureRequest(seconds=seconds,
                                                   start_offset_seconds=start_offset))
    print(f"read-back set: start_offset={start_offset} s, duration={seconds} s" if r.ok
          else f"failed: {r.error}")


def cmd_set_read_rate(stub, args):
    try:
        hz = float(args.value)
    except (TypeError, ValueError):
        sys.exit("usage: set-read-rate <hz>  (e.g. 800e6)")
    r = stub.SetReadCaptureRate(pb.ReadRateRequest(sample_rate_hz=hz))
    print(f"read-back sample rate set to {hz:.0f} Hz" if r.ok
          else f"failed: {r.error}")


_RB_STATE = {0: "idle", 1: "reading", 2: "complete", 3: "error"}


def _print_readback(rb):
    state = _RB_STATE.get(rb.state, str(rb.state))
    if rb.state == 0 and rb.records_target == 0:
        return  # nothing has run yet — keep watch output quiet
    line = f"nvme read-back: {state}"
    if rb.records_target:
        mb = rb.bytes_written / (1024 * 1024)
        line += (f"  {rb.records_done}/{rb.records_target} records"
                 f"  {mb:.1f} MB  ({rb.seconds_requested:g}s @ "
                 f"{rb.sample_rate_hz:.0f} Hz)")
    if rb.last_event:
        line += f"  event={rb.last_event}"
    print(line)
    if state == "reading":
        print("  (reading the latest capture out of NVMe — finishes shortly)")


def cmd_watch(stub, _):
    print("Streaming status (Ctrl-C to stop)...")
    try:
        for st in stub.WatchStatus(pb.Empty()):
            _print_flags(st.flags)
            _print_stats(st.stats)
            _print_readback(st.readback)
            print(f"  running={st.running} uptime_ms={st.uptime_ms}\n")
    except KeyboardInterrupt:
        print("\nstopped.")


def cmd_shutdown(stub, _):
    r = stub.Shutdown(pb.Empty())
    if r.ok:
        print("shutdown: requested")
    else:
        print(f"shutdown failed: {r.error}")


COMMANDS = {
    "get-flags": cmd_get_flags,
    "set-dsp": cmd_set,
    "set-ip": cmd_set,
    "set-logging": cmd_set,
    "set-read-capture": cmd_set_read_capture,
    "set-read-rate": cmd_set_read_rate,
    "get-stats": cmd_get_stats,
    "get-mode-stats": cmd_get_mode_stats,
    "list-ips": cmd_list_ips,
    "watch-status": cmd_watch,
    "shutdown": cmd_shutdown,
}


def main():
    p = argparse.ArgumentParser(description="XRComm platform gRPC control client")
    p.add_argument("--host", default=DEFAULT_HOST,
                   help=f"server address host:port (default {DEFAULT_HOST})")
    p.add_argument("command", choices=COMMANDS.keys())
    p.add_argument("value", nargs="?", default="",
                   help="on|off for set-*, seconds for set-read-capture, Hz for set-read-rate")
    p.add_argument("--start-offset", default="0.0",
                   help="set-read-capture only: offset (s) from the START OF THE "
                        "CAPTURE at which to begin reading, NOT a wall-clock time "
                        "(default 0.0 = from the first record)")
    args = p.parse_args()

    with grpc.insecure_channel(args.host) as channel:
        try:
            grpc.channel_ready_future(channel).result(timeout=5)
        except grpc.FutureTimeoutError:
            sys.exit(f"Could not reach server at {args.host}")
        stub = pbg.PipelineControlStub(channel)
        try:
            COMMANDS[args.command](stub, args)
        except grpc.RpcError as e:
            sys.exit(f"RPC failed: {e.code()} {e.details()}")


if __name__ == "__main__":
    main()