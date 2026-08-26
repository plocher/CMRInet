#!/usr/bin/env python3
"""Gather host-view validation captures for real-node and phantom-node scenarios."""

from __future__ import annotations

import argparse
import datetime
import json
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "probes" / "Issue47"))
import _tracer_client

EXPECT_ONLINE = "online"
EXPECT_GEOMETRY_MISMATCH = "geometry_mismatch"
EXPECT_OFFLINE = "offline"

SCENARIO_REAL_NODES_PASS = "real-nodes-pass"
SCENARIO_REAL_NODES_MISMATCH = "real-nodes-mismatch"
SCENARIO_REAL_NODES_PLUS_UA32 = "real-nodes-plus-ua32"


@dataclass
class NodePlan:
    """One UA declared into the Host for this scenario."""

    ua: int
    input_bytes: int
    output_bytes: int
    expectation: str


def _next_results_dir(root: Path) -> Path:
    """Return a timestamped output directory that does not collide."""
    today = datetime.datetime.now().strftime("%Y%m%d")
    base = root / f"results.{today}.host_view"
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
    """Wait for END CAPTURE marker from XiaoHostTracer."""
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


def _read_status_snapshot(ser, timeout_s: float = 3.0) -> Optional[dict]:
    """Read one host status JSON payload from the tracer shell."""
    ser.write(b"status\n")
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        line = ser.readline().decode("utf-8", errors="replace").strip()
        if not line.startswith("{"):
            continue
        try:
            doc = json.loads(line)
        except json.JSONDecodeError:
            continue
        if doc.get("event") == "status":
            return doc
    return None


def _read_node_status(ser, ua: int, timeout_s: float = 3.0) -> Optional[dict]:
    """Read one per-node status JSON payload from the tracer shell."""
    ser.write(f"status {ua}\n".encode("utf-8"))
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        line = ser.readline().decode("utf-8", errors="replace").strip()
        if not line.startswith("{"):
            continue
        try:
            doc = json.loads(line)
        except json.JSONDecodeError:
            continue
        if doc.get("event") != "status":
            continue
        if doc.get("ua") != ua:
            continue
        return doc
    return None


def _configure_real_node_traffic(
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
    """Enable slowwalker and write(read()) loopback for the two real-node UAs."""
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
        _tracer_client.send_generator_command(ser, "enable", "slowwalker", ua=ua)
        _tracer_client.configure_loopback_write_read(
            ser,
            ua=ua,
            src_byte=loopback_byte,
            src_bit=loopback_bit,
            dst_byte=loopback_byte,
            dst_bit=loopback_bit,
        )
        _tracer_client.send_generator_command(
            ser, "enable", "toggleoutfrominput", ua=ua
        )


def _build_node_plan(args: argparse.Namespace) -> list[NodePlan]:
    """Build per-UA declaration and expectation plan for the selected scenario."""
    nodes = [
        NodePlan(
            ua=args.ua_a,
            input_bytes=args.ua_a_in,
            output_bytes=args.ua_a_out,
            expectation=EXPECT_ONLINE,
        ),
        NodePlan(
            ua=args.ua_b,
            input_bytes=args.ua_b_in,
            output_bytes=args.ua_b_out,
            expectation=EXPECT_ONLINE,
        ),
    ]

    if args.scenario == SCENARIO_REAL_NODES_MISMATCH:
        for index, node in enumerate(nodes):
            if node.ua == args.mismatch_ua:
                nodes[index] = NodePlan(
                    ua=node.ua,
                    input_bytes=args.mismatch_in,
                    output_bytes=args.mismatch_out,
                    expectation=EXPECT_GEOMETRY_MISMATCH,
                )
                break
    elif args.scenario == SCENARIO_REAL_NODES_PLUS_UA32:
        nodes.append(
            NodePlan(
                ua=args.ua_c,
                input_bytes=args.ua_c_in,
                output_bytes=args.ua_c_out,
                expectation=EXPECT_OFFLINE,
            )
        )
    return nodes


def _expectations_json(nodes: list[NodePlan]) -> dict[str, dict]:
    """Convert node plan to manifest-friendly expectation JSON."""
    out: dict[str, dict] = {}
    for node in nodes:
        out[str(node.ua)] = {
            "expectation": node.expectation,
            "declared_in": node.input_bytes,
            "declared_out": node.output_bytes,
        }
    return out


def main() -> int:
    """Run one host-view validation scenario and write capture + manifest."""
    parser = argparse.ArgumentParser(description="Host-view bench validation gather")
    parser.add_argument(
        "--scenario",
        choices=(
            SCENARIO_REAL_NODES_PASS,
            SCENARIO_REAL_NODES_MISMATCH,
            SCENARIO_REAL_NODES_PLUS_UA32,
        ),
        default=SCENARIO_REAL_NODES_PASS,
    )
    parser.add_argument(
        "--port", default=None, help="Host device; default resolves from bench.json"
    )
    parser.add_argument("--secs", type=int, default=45, help="Capture run time in seconds")
    parser.add_argument("--ua-a", type=int, default=30)
    parser.add_argument("--ua-b", type=int, default=31)
    parser.add_argument("--ua-c", type=int, default=32)
    parser.add_argument("--ua-a-in", type=int, default=7)
    parser.add_argument("--ua-a-out", type=int, default=7)
    parser.add_argument("--ua-b-in", type=int, default=4)
    parser.add_argument("--ua-b-out", type=int, default=4)
    parser.add_argument("--ua-c-in", type=int, default=4)
    parser.add_argument("--ua-c-out", type=int, default=4)
    parser.add_argument("--mismatch-ua", type=int, default=31)
    parser.add_argument("--mismatch-in", type=int, default=1)
    parser.add_argument("--mismatch-out", type=int, default=1)
    parser.add_argument("--ua-a-walker-byte", type=int, default=3)
    parser.add_argument("--ua-b-walker-byte", type=int, default=2)
    parser.add_argument("--walker-period-ms", type=int, default=1000)
    parser.add_argument("--ua-a-loopback-byte", type=int, default=3)
    parser.add_argument("--ua-a-loopback-bit", type=int, default=1)
    parser.add_argument("--ua-b-loopback-byte", type=int, default=2)
    parser.add_argument("--ua-b-loopback-bit", type=int, default=1)
    parser.add_argument("--tag", default="host_view_validation")
    parser.add_argument("--out", default="auto")
    args = parser.parse_args()
    args.port = args.port or _tracer_client.host_port()

    node_plan = _build_node_plan(args)
    root = Path(__file__).resolve().parent / "data"
    out_dir = _next_results_dir(root) if args.out == "auto" else Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)

    log_path = out_dir / f"{args.tag}.log"
    manifest_path = out_dir / "manifest.json"
    ser = _tracer_client.reboot_and_reconnect(args.port)
    cleanup_ok = True

    try:
        if not _tracer_client.sync_and_validate_boot(ser):
            print("ERROR: boot validation failed", file=sys.stderr)
            return 1

        print("Configuring runtime topology...")
        _display(ser, 1, f"host-view {args.scenario}")
        _display(ser, 2, "cfg nodes")
        for node in node_plan:
            _tracer_client.send_command(
                ser, f"node add {node.ua} {node.input_bytes} {node.output_bytes}"
            )
            _tracer_client.send_command(ser, f"node enable {node.ua}")
        _tracer_client.flush_lines(ser)

        print("Configuring traffic on real nodes...")
        _display(ser, 1, f"u{args.ua_a}s{args.ua_a_walker_byte} u{args.ua_b}s{args.ua_b_walker_byte}")
        _configure_real_node_traffic(
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
        _tracer_client.flush_lines(ser)

        print(f"Starting capture for {args.secs}s...")
        _display(ser, 2, f"run {args.secs}s")
        _tracer_client.send_command(ser, f"run {args.secs}")
        if not _await_end_capture(ser, args.secs):
            _display(ser, 2, "run timeout")
            print("ERROR_TIMEOUT: END CAPTURE not seen", file=sys.stderr)
            return 1

        node_addresses = tuple(node.ua for node in node_plan)
        print("Quiescing traffic before dump...")
        _display(ser, 2, "quiescing")
        quiet_ok = _tracer_client.quiesce_traffic_preserving_ring(
            ser, node_addresses=node_addresses
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
            str(node.ua): _read_node_status(ser, node.ua) for node in node_plan
        }

        with log_path.open("w", encoding="utf-8") as handle:
            handle.write("# CMRI Host-View Validation\n")
            handle.write(f"# scenario: {args.scenario}\n")
            handle.write(f"# timestamp: {datetime.datetime.now().isoformat()}\n")
            for node in node_plan:
                handle.write(
                    f"# ua{node.ua}: in={node.input_bytes} out={node.output_bytes} expect={node.expectation}\n"
                )
            for line in dump_lines:
                handle.write(line + "\n")
        print(f"Wrote capture: {log_path}")

        manifest = {
            "scenario": args.scenario,
            "tag": args.tag,
            "port": args.port,
            "secs": args.secs,
            "capture_file": log_path.name,
            "node_expectations": _expectations_json(node_plan),
            "status_snapshot": status_snapshot,
            "node_statuses": node_statuses,
            "timestamp": datetime.datetime.now().isoformat(),
        }
        manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        print(f"Wrote manifest: {manifest_path}")
        _display(ser, 2, "done")
    finally:
        cleanup_ok = _tracer_client.shutdown_and_verify_quiet(
            ser, node_addresses=tuple(node.ua for node in node_plan)
        )
        try:
            ser.close()
        except Exception:
            pass

    return 0 if cleanup_ok else 1


if __name__ == "__main__":
    sys.exit(main())
