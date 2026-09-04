#!/usr/bin/env python3
"""Gather one quick dual-node bench validation capture for UA30/UA31."""

from __future__ import annotations

import argparse
import datetime
import json
import sys
import time
from pathlib import Path
from typing import Optional

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "probes" / "Issue47"))
import _tracer_client


def _next_results_dir(root: Path) -> Path:
    """Return a timestamped output directory that does not collide."""
    today = datetime.datetime.now().strftime("%Y%m%d")
    base = root / f"results.{today}.bench_validation"
    candidate = base
    suffix = 1
    while candidate.exists():
        candidate = Path(str(base) + f".{suffix}")
        suffix += 1
    return candidate


def _display(ser, line: int, message: str) -> None:
    """Write one OLED annotation line through tracer display verb."""
    _tracer_client.send_command(ser, f"display {line} {message}")


def _await_end_capture(ser, secs: int, timeout_slop_s: float = 5.0) -> bool:
    """Wait for END CAPTURE from TracerHost."""
    deadline = time.time() + secs + timeout_slop_s
    while time.time() < deadline:
        line = ser.readline().decode("utf-8", errors="replace").strip()
        if line.startswith("END CAPTURE"):
            print(f"Seen run-window marker: {line}")
            return True
    return False


def _collect_dump_lines(ser, timeout_s: float = 15.0) -> list[str]:
    """Collect ring dump payload lines from BEGIN DUMP to END DUMP."""
    ser.write(b"dump\n")
    deadline = time.time() + timeout_s
    in_dump = False
    dump_lines: list[str] = []
    while time.time() < deadline:
        line = ser.readline().decode("utf-8", errors="replace").strip()
        if not line:
            continue
        if line.startswith("BEGIN DUMP"):
            in_dump = True
            continue
        if line == "END DUMP":
            return dump_lines
        if in_dump:
            dump_lines.append(line)
    return dump_lines


def _read_status_snapshot(ser, timeout_s: float = 8.0) -> Optional[dict]:
    """Read the host-scope status bundle via the shared tracer client."""
    return _tracer_client.read_host_status_snapshot(ser, timeout_s=timeout_s)


def _read_node_status(ser, ua: int, timeout_s: float = 3.0) -> Optional[dict]:
    """Read one per-node status JSON payload from the tracer shell."""
    return _tracer_client.read_node_status(ser, ua, timeout_s=timeout_s)


def _configure_dual_node_traffic(
    ser,
    ua_a: int,
    ua_b: int,
    walker_byte_a: int,
    walker_byte_b: int,
    walker_period_ms: int,
    loopback_byte_a: int,
    loopback_bit_a: int,
    loopback_byte_b: int,
    loopback_bit_b: int,
) -> None:
    """Enable slowwalker and write(read()) loopback on both nodes."""
    for ua, walker_byte, loopback_byte, loopback_bit in (
        (ua_a, walker_byte_a, loopback_byte_a, loopback_bit_a),
        (ua_b, walker_byte_b, loopback_byte_b, loopback_bit_b),
    ):
        _tracer_client.send_generator_command(
            ser,
            "configure",
            "slowwalker",
            ua=ua,
            extra_args=f"byte {walker_byte} period {walker_period_ms}",
        )
        _tracer_client.send_generator_command(
            ser,
            "enable",
            "slowwalker",
            ua=ua,
        )
        _tracer_client.configure_loopback_write_read(
            ser,
            ua=ua,
            src_byte=loopback_byte,
            src_bit=loopback_bit,
            dst_byte=loopback_byte,
            dst_bit=loopback_bit,
        )
        _tracer_client.send_generator_command(
            ser,
            "enable",
            "toggleoutfrominput",
            ua=ua,
        )


def main() -> int:
    """Run one quick dual-node validation capture and write artifacts."""
    parser = argparse.ArgumentParser(description="Dual-node bench validation gather")
    parser.add_argument("--port", default=None, help="Host device; default resolves from bench.json")
    parser.add_argument("--secs", type=int, default=45, help="Capture run time in seconds")
    parser.add_argument("--ua-a", type=int, default=30)
    parser.add_argument("--ua-b", type=int, default=31)
    parser.add_argument("--ua-a-in", type=int, default=7)
    parser.add_argument("--ua-a-out", type=int, default=7)
    parser.add_argument("--ua-b-in", type=int, default=3)
    parser.add_argument("--ua-b-out", type=int, default=3)
    # Walker bytes per docs/testbed-physical-notes.md jumper map:
    # UA30 byte 5 (cross-byte jumper OUT(5,1)->IN(6,1));
    # UA31 byte 2 (same-byte loopback). Loopback defaults follow the
    # physical jumpers. NOTE: a loopback and a walker on the same node
    # both write outputs — if they share a byte the walker pattern is
    # visually confusing (two bits change per step, not one). That is a
    # readability concern, not a defect; the combination is a valid
    # stimulus. See the dual_node README.
    parser.add_argument("--ua-a-walker-byte", type=int, default=5)
    parser.add_argument("--ua-b-walker-byte", type=int, default=2)
    parser.add_argument("--walker-period-ms", type=int, default=1000)
    parser.add_argument("--ua-a-loopback-byte", type=int, default=3)
    parser.add_argument("--ua-a-loopback-bit", type=int, default=1)
    parser.add_argument("--ua-b-loopback-byte", type=int, default=2)
    parser.add_argument("--ua-b-loopback-bit", type=int, default=1)
    parser.add_argument("--tag", default="bench_validation")
    parser.add_argument("--out", default="auto")
    args = parser.parse_args()
    args.port = args.port or _tracer_client.host_port()

    root = Path(__file__).resolve().parent / "data"
    out_dir = _next_results_dir(root) if args.out == "auto" else Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    log_path = out_dir / f"{args.tag}.log"
    manifest_path = out_dir / "manifest.json"

    ser = _tracer_client.reboot_and_reconnect(args.port)
    cleanup_ok = True
    status_snapshot: Optional[dict] = None
    node_statuses: dict[str, Optional[dict]] = {}

    try:
        if not _tracer_client.sync_and_validate_boot(ser):
            print("ERROR: boot validation failed", file=sys.stderr)
            return 1

        print("Configuring runtime topology...")
        _display(ser, 1, "dual-node validate")
        _display(ser, 2, "cfg nodes")
        _tracer_client.send_command(ser, f"node add {args.ua_a} C {args.ua_a_in} {args.ua_a_out}")
        _tracer_client.send_command(ser, f"node add {args.ua_b} C {args.ua_b_in} {args.ua_b_out}")
        _tracer_client.send_command(ser, f"node enable {args.ua_a}")
        _tracer_client.send_command(ser, f"node enable {args.ua_b}")
        # Disable the compiled-in phantom node (UA32) so its backoff
        # ladder doesn't inject ~260ms pauses into the capture.
        _tracer_client.send_command(ser, "node disable 32")
        _tracer_client.flush_lines(ser)

        print("Configuring dual-node traffic (slowwalker + write(read()) loopback)...")
        _display(ser, 1, f"u{args.ua_a}s{args.ua_a_walker_byte} u{args.ua_b}s{args.ua_b_walker_byte}")
        _display(
            ser,
            2,
            (
                f"lb u{args.ua_a}:{args.ua_a_loopback_byte}.{args.ua_a_loopback_bit} "
                f"u{args.ua_b}:{args.ua_b_loopback_byte}.{args.ua_b_loopback_bit}"
            ),
        )
        _configure_dual_node_traffic(
            ser,
            ua_a=args.ua_a,
            ua_b=args.ua_b,
            walker_byte_a=args.ua_a_walker_byte,
            walker_byte_b=args.ua_b_walker_byte,
            walker_period_ms=args.walker_period_ms,
            loopback_byte_a=args.ua_a_loopback_byte,
            loopback_bit_a=args.ua_a_loopback_bit,
            loopback_byte_b=args.ua_b_loopback_byte,
            loopback_bit_b=args.ua_b_loopback_bit,
        )
        _display(ser, 1, "slow+loopback on")
        _display(ser, 2, "behaviors enabled")
        _tracer_client.flush_lines(ser)

        print(f"Starting capture for {args.secs}s...")
        _display(ser, 2, f"run {args.secs}s")
        _tracer_client.send_command(ser, f"run {args.secs}")
        if not _await_end_capture(ser, args.secs):
            _display(ser, 2, "run timeout")
            print("ERROR_TIMEOUT: END CAPTURE not seen", file=sys.stderr)
            return 1

        print("Quiescing traffic before dump...")
        _display(ser, 2, "quiescing")
        quiet_ok = _tracer_client.quiesce_traffic_preserving_ring(
            ser,
            node_addresses=(args.ua_a, args.ua_b),
        )
        if not quiet_ok:
            _display(ser, 2, "quiet timeout")
            print("ERROR: bus did not become quiet before dump", file=sys.stderr)

        print("Dumping ring...")
        _display(ser, 2, "dumping")
        dump_lines = _collect_dump_lines(ser)
        if not dump_lines:
            _display(ser, 2, "dump failed")
            print("ERROR: no dump lines captured", file=sys.stderr)
            return 1

        status_snapshot = _read_status_snapshot(ser)
        node_statuses = {
            str(args.ua_a): _read_node_status(ser, args.ua_a),
            str(args.ua_b): _read_node_status(ser, args.ua_b),
        }

        with log_path.open("w", encoding="utf-8") as handle:
            handle.write("# CMRI Dual Node Bench Validation\n")
            handle.write(f"# ua_a: {args.ua_a}\n")
            handle.write(f"# ua_b: {args.ua_b}\n")
            handle.write(f"# ua_a_walker_byte: {args.ua_a_walker_byte}\n")
            handle.write(f"# ua_b_walker_byte: {args.ua_b_walker_byte}\n")
            handle.write(f"# walker_period_ms: {args.walker_period_ms}\n")
            handle.write(f"# ua_a_loopback_byte: {args.ua_a_loopback_byte}\n")
            handle.write(f"# ua_a_loopback_bit: {args.ua_a_loopback_bit}\n")
            handle.write(f"# ua_b_loopback_byte: {args.ua_b_loopback_byte}\n")
            handle.write(f"# ua_b_loopback_bit: {args.ua_b_loopback_bit}\n")
            handle.write(f"# secs: {args.secs}\n")
            handle.write(f"# timestamp: {datetime.datetime.now().isoformat()}\n")
            for line in dump_lines:
                handle.write(line + "\n")
        print(f"Wrote capture: {log_path}")

        manifest = {
            "scenario": "dual_node_bench_validation",
            "tag": args.tag,
            "port": args.port,
            "secs": args.secs,
            "ua_a": args.ua_a,
            "ua_b": args.ua_b,
            "ua_a_in": args.ua_a_in,
            "ua_a_out": args.ua_a_out,
            "ua_b_in": args.ua_b_in,
            "ua_b_out": args.ua_b_out,
            "ua_a_walker_byte": args.ua_a_walker_byte,
            "ua_b_walker_byte": args.ua_b_walker_byte,
            "walker_period_ms": args.walker_period_ms,
            "ua_a_loopback_byte": args.ua_a_loopback_byte,
            "ua_a_loopback_bit": args.ua_a_loopback_bit,
            "ua_b_loopback_byte": args.ua_b_loopback_byte,
            "ua_b_loopback_bit": args.ua_b_loopback_bit,
            "capture_file": log_path.name,
            "status_snapshot": status_snapshot,
            "node_statuses": node_statuses,
            "timestamp": datetime.datetime.now().isoformat(),
        }
        manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        print(f"Wrote manifest: {manifest_path}")
        _display(ser, 2, "done")

    finally:
        cleanup_ok = _tracer_client.shutdown_and_verify_quiet(
            ser,
            node_addresses=(args.ua_a, args.ua_b),
        )
        try:
            ser.close()
        except Exception:
            pass

    return 0 if cleanup_ok else 1


if __name__ == "__main__":
    sys.exit(main())
