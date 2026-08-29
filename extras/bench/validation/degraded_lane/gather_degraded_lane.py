#!/usr/bin/env python3
"""Gather one degraded-lane bounding bench validation capture (#87/#88).

Reproduces the #80 condition on hardware with the same three-node
population the unit test uses (test_degraded_participation_is_bounded):
- UA30 (healthy): compiled in at 7/7, conforming, with slowwalker
  generator load so the round-robin has real contention.
- UA31 (degraded, misconfigured): a real alive node declared 4/4
  against its physical 3/3. Its replies carry the wrong geometry and
  are rejected — this is the #80 condition.
- UA32 (degraded, silent): the compiled-in phantom — a nonexistent
  node that never replies. Its polls are valid degraded-lane traffic,
  not pollution: the 80/20 rule bounds ALL imperfect nodes, and a
  silent node is the other failure mode the gates exist to bound.

After a 60 s capture, the host status snapshot carries the
degraded-lane ledger (degradedGrants, degradedSlotDenials,
degradedBandwidthDenials, degradedClampBypasses) and the ring dump
carries the per-UA TX P poll distribution.

The bench is currently 2-wire, so the host's CDC ring dump sees all TX
(and the self-echo on RX, which echo-cancel discards). Sniffer witnesses
are optional here — they would see the same traffic — so this gather
follows the CDC-only pattern used by the dual_node and host_view suites.
"""

from __future__ import annotations

import argparse
import datetime
import json
import re
import sys
import time
from pathlib import Path
from typing import Optional

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "probes" / "Issue47"))
import _tracer_client


def _next_results_dir(root: Path) -> Path:
    """Return a timestamped output directory that does not collide."""
    today = datetime.datetime.now().strftime("%Y%m%d")
    base = root / f"results.{today}.degraded_lane"
    candidate = base
    suffix = 1
    while candidate.exists():
        candidate = Path(str(base) + f".{suffix}")
        suffix += 1
    return candidate


def _display(ser, line: int, message: str) -> None:
    """Write one OLED annotation line through the tracer display verb."""
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


def _read_status_snapshot(ser, timeout_s: float = 5.0) -> Optional[dict]:
    """Read one host status JSON payload from the tracer shell.

    The host-scoped status line (roster + generators + degraded-lane
    ledger) is longer than a per-node line and can be truncated by CDC
    backpressure (#86/#99). The degraded-lane ledger fields
    (degradedGrants, degradedSlotDenials, etc.) appear before the
    roster/generators in the JSON, so they survive truncation. Try full
    JSON parse first; fall back to regex extraction of the ledger
    fields from a truncated line.
    """
    ledger_fields = (
        "degradedGrants",
        "degradedSlotDenials",
        "degradedBandwidthDenials",
        "degradedClampBypasses",
    )
    for _attempt in range(2):
        ser.reset_input_buffer()
        ser.write(b"status\n")
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            line = ser.readline().decode("utf-8", errors="replace").strip()
            if not line.startswith("{"):
                continue
            if '"event":"status"' not in line:
                continue
            # Distinguish host-scoped from per-node status: the
            # host-scoped line carries "roster" (and "degradedGrants");
            # per-node lines carry "present":true but no roster. Note
            # the roster array itself contains "ua": entries, so checking
            # for "ua": would wrongly skip the host-scoped line.
            if '"roster"' not in line and '"degradedGrants"' not in line:
                continue
            # Try full JSON parse first.
            try:
                doc = json.loads(line)
                if doc.get("event") == "status":
                    return doc
            except json.JSONDecodeError:
                pass
            # Fallback: the line was truncated, but the ledger fields
            # appear before the roster/generators that got cut. Extract
            # them with regex, same pattern sync_and_validate_boot uses
            # for image/version on truncated lines.
            doc = {"event": "status", "truncated": True}
            ok = True
            for field in ledger_fields:
                m = re.search(rf'"{field}":(\d+)', line)
                if m:
                    doc[field] = int(m.group(1))
                else:
                    ok = False
            if ok:
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


def main() -> int:
    """Run one degraded-lane bounding capture and write artifacts."""
    parser = argparse.ArgumentParser(description="Degraded-lane bench validation gather")
    parser.add_argument(
        "--port", default=None, help="Host device; default resolves from bench.json"
    )
    parser.add_argument("--secs", type=int, default=60, help="Capture run time in seconds")
    # UA30 is the healthy node: compiled in at 7/7, conforming.
    parser.add_argument("--healthy-ua", type=int, default=30)
    parser.add_argument("--healthy-in", type=int, default=7)
    parser.add_argument("--healthy-out", type=int, default=7)
    # UA31 is the misconfigured degraded node: a real alive node declared
    # 4/4 against its physical 3/3. Its replies carry the wrong geometry
    # and are rejected (#80 condition).
    parser.add_argument("--misconfigured-ua", type=int, default=31)
    parser.add_argument("--misconfigured-in", type=int, default=4)
    parser.add_argument("--misconfigured-out", type=int, default=4)
    # UA32 is the silent degraded node: the compiled-in phantom, a
    # nonexistent node that never replies. Its polls are valid
    # degraded-lane traffic — the 80/20 rule bounds ALL imperfect nodes.
    parser.add_argument("--silent-ua", type=int, default=32)
    parser.add_argument("--silent-in", type=int, default=4)
    parser.add_argument("--silent-out", type=int, default=4)
    # Generator load on the healthy node creates round-robin contention.
    parser.add_argument("--walker-byte", type=int, default=5)
    parser.add_argument("--walker-period-ms", type=int, default=1000)
    parser.add_argument("--tag", default="degraded_lane_validation")
    parser.add_argument("--out", default="auto")
    args = parser.parse_args()
    args.port = args.port or _tracer_client.host_port()

    healthy_ua = args.healthy_ua
    misconfigured_ua = args.misconfigured_ua
    silent_ua = args.silent_ua
    degraded_uas = [misconfigured_ua, silent_ua]
    all_uas = [healthy_ua] + degraded_uas

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
        _display(ser, 1, "degraded-lane")
        _display(ser, 2, "cfg nodes")
        # UA30 (healthy) and UA32 (silent phantom) are compiled in.
        # Just ensure both are enabled.
        _tracer_client.send_command(ser, f"node enable {healthy_ua}")
        _tracer_client.send_command(ser, f"node enable {silent_ua}")
        # UA31 (misconfigured): a real alive node declared 4/4 against
        # its physical 3/3. Its replies carry the wrong geometry and are
        # rejected — this reproduces the #80 condition on hardware.
        _tracer_client.send_command(
            ser, f"node add {misconfigured_ua} {args.misconfigured_in} {args.misconfigured_out}"
        )
        _tracer_client.send_command(ser, f"node enable {misconfigured_ua}")
        _tracer_client.flush_lines(ser)

        print("Configuring generator load on healthy node...")
        _display(ser, 1, f"u{healthy_ua} walker byte {args.walker_byte}")
        _tracer_client.send_generator_command(
            ser, "configure", "slowwalker", ua=healthy_ua,
            extra_args=f"byte {args.walker_byte} period {args.walker_period_ms}",
        )
        _tracer_client.send_generator_command(ser, "enable", "slowwalker", ua=healthy_ua)
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
            ser, node_addresses=tuple(all_uas),
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

        # Settle after the dump: the ring dump emits 1000+ CDC lines,
        # and trailing event data can consume the status-read window.
        # Flush and wait for the CDC stream to drain before reading status.
        _tracer_client.flush_lines(ser)
        time.sleep(1.0)
        ser.reset_input_buffer()

        status_snapshot = _read_status_snapshot(ser)
        node_statuses = {
            str(ua): _read_node_status(ser, ua) for ua in all_uas
        }

        with log_path.open("w", encoding="utf-8") as handle:
            handle.write("# CMRI Degraded-Lane Bounding Validation\n")
            handle.write(f"# healthy_ua: {healthy_ua} in={args.healthy_in} out={args.healthy_out}\n")
            handle.write(f"# misconfigured_ua: {misconfigured_ua} in={args.misconfigured_in} out={args.misconfigured_out}\n")
            handle.write(f"# silent_ua: {silent_ua} in={args.silent_in} out={args.silent_out}\n")
            handle.write(f"# secs: {args.secs}\n")
            handle.write(f"# timestamp: {datetime.datetime.now().isoformat()}\n")
            for line in dump_lines:
                handle.write(line + "\n")
        print(f"Wrote capture: {log_path}")

        manifest = {
            "scenario": "degraded_lane_bounding",
            "tag": args.tag,
            "port": args.port,
            "secs": args.secs,
            "healthy_ua": healthy_ua,
            "degraded_uas": degraded_uas,
            f"ua_{healthy_ua}_in": args.healthy_in,
            f"ua_{misconfigured_ua}_in": args.misconfigured_in,
            f"ua_{silent_ua}_in": args.silent_in,
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
            ser, node_addresses=tuple(all_uas),
        )
        try:
            ser.close()
        except Exception:
            pass

    return 0 if cleanup_ok else 1


if __name__ == "__main__":
    sys.exit(main())
