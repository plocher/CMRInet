#!/usr/bin/env python3
"""Gather a CDC backpressure capture and check the line terminator (#99).

This is the on-bench test for the writeCdcLine extraction in
src/testbed/CdcLineWriter.h. The desktop fake in tests/test_cdc_line.cpp
simulates a full buffer; only the real USB CDC stream under a slow
reader reproduces the #86 failure shape -- the ring fills,
setTxTimeoutMs(0) makes writes discard-and-return when full, and the
terminator must survive the one moment the buffer is full.

Why not `run <secs>`
--------------------
The sketch's ourOnTrace routes packets to the RAM ring while a run is
active, NOT to the CDC stream. So this script does NOT send `run`: with
walker enabled and no run active, every poll's I/T/P/R trace line
flows through writeCdcLine to CDC, which is the path under test.

Method
------
  1. Open the Host CDC port (DTR/RTS asserted so CdcConsole::open() is
     true and writes proceed) and validate boot identity.
  2. Enable walker on UA30 to maximise trace density.
  3. For --secs, drain the CDC stream SLOWLY (at most --chunk bytes per
     --sleep-ms) so the ring fills: body writes are dropped while
     terminators must still land within their reserved 50 ms slice.
  4. Send `quit`, then fast-drain for --tail-secs to capture the
     `final` line (the quit->final smoke through the trampoline +
     adapter + bound shell).
  5. Analyze the raw byte capture.

Pass criteria (the #86 fix survived the extraction)
---------------------------------------------------
  1. NO merged records: no newline-delimited chunk carries more than
     one "seq" field, and no `}{"seq"` adjacency (closed object glued
     to the next record's opening) appears with no newline between.
     Two records on one line is the missing-terminator signature.
     Hard gate.
  2. Non-vacuous traffic: at least --min-seq "seq" fields captured.
  3. Backpressure was real: either body truncation was observed, or the
     fast-drain backlog ratio (tail/slow) clears --backlog-floor.
     Dropped body bytes are gone and unobservable, so truncation is a
     weak signal; a large tail backlog proves the buffer was congested
     when the records were written, so "0 merges" is not vacuous.
  4. The `final` line arrived and was newline-terminated.

Artifacts (raw bytes + summary.json) are written under data/.
"""

from __future__ import annotations

import argparse
import datetime
import json
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "probes" / "Issue47"))
import _tracer_client

_SEQ_RE = re.compile(rb'"seq"\s*:\s*\d+')
# A closed object glued to the next record's opening with NO newline between.
# `[ \t]*` (not `\s*`) so a legitimate `}\n{"seq"` record boundary is NOT
# counted as a glue -- `\s*` would eat the newline and false-positive on
# every healthy record boundary.
_GLUE_RE = re.compile(rb'}[ \t]*{"seq"')
_FINAL_RE = re.compile(rb'"event"\s*:\s*"final"')


def _next_results_dir(root: Path) -> Path:
    """Return a timestamped output directory that does not collide."""
    today = datetime.datetime.now().strftime("%Y%m%d")
    base = root / f"results.{today}.cdc_backpressure"
    candidate = base
    suffix = 1
    while candidate.exists():
        candidate = Path(str(base) + f".{suffix}")
        suffix += 1
    return candidate


def _slow_capture(ser, secs: float, chunk: int, sleep_ms: float) -> bytearray:
    """Drain the CDC stream slowly to fill the ring and drop bodies."""
    sleep_s = sleep_ms / 1000.0
    buf = bytearray()
    deadline = time.time() + secs
    while time.time() < deadline:
        time.sleep(sleep_s)
        waiting = ser.in_waiting
        if waiting > 0:
            data = ser.read(min(waiting, chunk))
            if data:
                buf.extend(data)
    return buf


def _drain_until_final(ser, timeout_s: float) -> bytearray:
    """Fast-drain until the `final` line appears, or timeout_s elapses.

    Under heavy backpressure the `final` line is queued behind a large
    host-side backlog, so a fixed window can stop just before it. This
    drains as long as needed (bounded by timeout_s) and stops the moment
    the final line is seen.
    """
    buf = bytearray()
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        waiting = ser.in_waiting
        if waiting > 0:
            data = ser.read(waiting)
            if data:
                buf.extend(data)
                if _FINAL_RE.search(buf):
                    return buf
        else:
            time.sleep(0.01)
    return buf


def _analyze(raw: bytes, slow_bytes: int, tail_bytes: int) -> dict:
    """Score the capture against the #99 / #86 pass criteria."""
    chunks = raw.split(b"\n")
    newline_count = raw.count(b"\n")
    seq_total = len(_SEQ_RE.findall(raw))
    glue_count = len(_GLUE_RE.findall(raw))

    seq_per_chunk = [len(_SEQ_RE.findall(ch)) for ch in chunks]
    merged_chunks = sum(1 for c in seq_per_chunk if c > 1)
    glued_extra = sum(c - 1 for c in seq_per_chunk if c > 1)

    valid_records = 0
    truncated_terminated = 0  # has a seq, ends at a newline, but not JSON
    noise_chunks = 0  # non-empty, no seq, not JSON (harness text, markers)
    for idx, ch in enumerate(chunks):
        if not ch.strip():
            continue
        has_seq = bool(_SEQ_RE.search(ch))
        try:
            json.loads(ch.decode("utf-8", "replace"))
            valid_records += 1
        except Exception:
            if has_seq:
                # The last chunk has no trailing newline (capture ended
                # mid-record); it is not "terminated", so do not count it
                # as evidence of the terminator surviving.
                is_last = idx == len(chunks) - 1
                if is_last and not raw.endswith(b"\n"):
                    pass
                else:
                    truncated_terminated += 1
            else:
                noise_chunks += 1

    final_match = _FINAL_RE.search(raw)
    final_seen = final_match is not None
    final_terminated = False
    if final_match:
        end = final_match.end()
        final_terminated = raw[end:end + 1] == b"\n" or raw.endswith(b"\n")

    backlog_ratio = (tail_bytes / slow_bytes) if slow_bytes > 0 else 0.0

    return {
        "total_bytes": len(raw),
        "slow_bytes": slow_bytes,
        "tail_bytes": tail_bytes,
        "backlog_ratio": round(backlog_ratio, 2),
        "newline_count": newline_count,
        "seq_total": seq_total,
        "valid_records": valid_records,
        "truncated_terminated": truncated_terminated,
        "noise_chunks": noise_chunks,
        "merged_chunks": merged_chunks,
        "glued_extra_records": glued_extra,
        "adjacency_glues": glue_count,
        "final_seen": final_seen,
        "final_terminated": final_terminated,
    }


def _verdict(stats: dict, min_truncated: int, min_seq: int,
             backlog_floor: float) -> tuple[str, list[str]]:
    """Classify the run. Returns (verdict, reasons).

    The #86 regression is merged records (a dropped terminator glues two
    records on one line). Dropped body bytes are unobservable, so the
    vacuity guard is the fast-drain backlog ratio: a large tail vs the
    slow capture proves the buffer was congested when the records were
    written, so "0 merges" is not vacuous.
    """
    reasons: list[str] = []
    ok = True

    if stats["merged_chunks"] > 0 or stats["adjacency_glues"] > 0:
        ok = False
        reasons.append(
            f"FAIL: {stats['merged_chunks']} merged chunk(s), "
            f"{stats['adjacency_glues']} adjacency glue(s) -- a terminator "
            "was dropped and two records share a line (#86 regression)"
        )
    if not stats["final_seen"] or not stats["final_terminated"]:
        ok = False
        reasons.append(
            "FAIL: the `final` line did not arrive newline-terminated "
            "(quit->final smoke failed)"
        )
    if stats["seq_total"] < min_seq:
        ok = False
        reasons.append(
            f"FAIL: only {stats['seq_total']} seq field(s) captured "
            f"(need {min_seq}) -- non-vacuous traffic floor not met"
        )

    if not ok:
        return "FAIL", reasons

    backpressure = (stats["truncated_terminated"] >= min_truncated or
                    stats["backlog_ratio"] >= backlog_floor)
    if not backpressure:
        reasons.append(
            f"INCONCLUSIVE: no merged records, but no backpressure "
            f"evidence (truncated={stats['truncated_terminated']}, "
            f"backlog_ratio={stats['backlog_ratio']} < {backlog_floor}) -- "
            "re-run with a slower drain (--sleep-ms higher)."
        )
        return "INCONCLUSIVE", reasons

    if stats["truncated_terminated"] >= min_truncated:
        via = (f"body truncation observed "
               f"({stats['truncated_terminated']} truncated-but-terminated)")
    else:
        via = (f"backlog ratio {stats['backlog_ratio']}x >= "
               f"{backlog_floor}x (slow={stats['slow_bytes']} B, "
               f"tail={stats['tail_bytes']} B)")
    reasons.append(
        f"PASS: {stats['seq_total']} seq fields, "
        f"{stats['valid_records']} valid, 0 merged records. "
        f"Backpressure evidence: {via}. The terminator survived the "
        "congested regime."
    )
    return "PASS", reasons


def main() -> int:
    parser = argparse.ArgumentParser(
        description="CDC backpressure terminator test (#99)"
    )
    parser.add_argument("--port", default=None,
                        help="Host device; default resolves from bench.json")
    parser.add_argument("--secs", type=float, default=12.0,
                        help="Slow-capture (backpressure) window in seconds")
    parser.add_argument("--chunk", type=int, default=32,
                        help="Max bytes drained per slow-read tick")
    parser.add_argument("--sleep-ms", type=float, default=20.0,
                        help="Idle between slow-read ticks (sets drain rate)")
    parser.add_argument("--tail-secs", type=float, default=10.0,
                        help="Max fast-drain window after quit to catch `final`")
    parser.add_argument("--ua", type=int, default=30,
                        help="Node UA to drive with walker")
    parser.add_argument("--walker-period-ms", type=int, default=120,
                        help="Walker period (shorter = dirtier image)")
    parser.add_argument("--walker-byte", type=int, default=3,
                        help="Output byte the walker drives")
    parser.add_argument("--walker-invert", type=int, default=1,
                        choices=(0, 1),
                        help="Walker polarity: 0 = active-high, 1 = active-low "
                             "(default 1: maximum trace density)")
    parser.add_argument("--min-truncated", type=int, default=20,
                        help="Vacuity floor: truncated-but-terminated chunks")
    parser.add_argument("--min-seq", type=int, default=100,
                        help="Vacuity floor: total seq fields captured")
    parser.add_argument("--backlog-floor", type=float, default=3.0,
                        help="Vacuity floor: tail/slow backlog ratio")
    parser.add_argument("--tag", default="cdc_backpressure")
    parser.add_argument("--out", default="auto")
    args = parser.parse_args()

    args.port = args.port or _tracer_client.host_port()

    root = Path(__file__).resolve().parent / "data"
    out_dir = _next_results_dir(root) if args.out == "auto" else Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    raw_path = out_dir / f"{args.tag}.raw"
    summary_path = out_dir / "summary.json"

    print(f"Host port:    {args.port}")
    print(f"Backpressure: {args.secs}s @ {args.chunk} B / {args.sleep_ms} ms "
          f"(~{args.chunk * 1000.0 / args.sleep_ms:.0f} B/s drain)")

    ser = _tracer_client.reboot_and_reconnect(args.port)
    try:
        if not _tracer_client.sync_and_validate_boot(ser):
            print("ERROR: boot validation failed", file=sys.stderr)
            return 1

        print("Enabling walker to drive trace density on CDC...")
        _tracer_client.send_generator_command(
            ser, "configure", "walker", ua=args.ua,
            extra_args=(
                f"period {args.walker_period_ms} byte {args.walker_byte} "
                f"invert {args.walker_invert}"
            ),
        )
        _tracer_client.send_generator_command(
            ser, "enable", "walker", ua=args.ua,
        )
        _tracer_client.flush_lines(ser)

        print(f"Slow-capturing for {args.secs}s (ring should fill)...")
        slow = _slow_capture(ser, args.secs, args.chunk, args.sleep_ms)

        print("Sending quit; fast-draining until `final` is seen...")
        _tracer_client.send_command(ser, "quit", delay_s=0.0)
        tail = _drain_until_final(ser, args.tail_secs)

        slow_bytes = len(slow)
        tail_bytes = len(tail)
        raw = bytes(slow) + bytes(tail)
        raw_path.write_bytes(raw)
        print(f"Wrote raw capture: {raw_path} ({len(raw)} bytes; "
              f"slow={slow_bytes}, tail={tail_bytes})")

        stats = _analyze(raw, slow_bytes, tail_bytes)
        verdict, reasons = _verdict(stats, args.min_truncated, args.min_seq,
                                    args.backlog_floor)

        print()
        print("=================== CDC BACKPRESSURE VERDICT ===================")
        for k, v in stats.items():
            print(f"  {k:<22} {v}")
        print("  -----------------------------------------------------------")
        for r in reasons:
            print(f"  {r}")
        print(f"  VERDICT: {verdict}")
        print("===============================================================")

        summary = {
            "scenario": "cdc_backpressure_terminator",
            "tag": args.tag,
            "port": args.port,
            "secs": args.secs,
            "chunk": args.chunk,
            "sleep_ms": args.sleep_ms,
            "tail_secs": args.tail_secs,
            "ua": args.ua,
            "walker_period_ms": args.walker_period_ms,
            "walker_byte": args.walker_byte,
            "walker_invert": args.walker_invert,
            "verdict": verdict,
            "stats": stats,
            "reasons": reasons,
            "raw_file": raw_path.name,
            "timestamp": datetime.datetime.now().isoformat(),
        }
        summary_path.write_text(json.dumps(summary, indent=2) + "\n",
                                encoding="utf-8")
        print(f"Wrote summary: {summary_path}")

    finally:
        try:
            _tracer_client.shutdown_and_verify_quiet(
                ser, node_addresses=(args.ua,)
            )
        except Exception:
            pass
        try:
            ser.close()
        except Exception:
            pass

    return 0 if verdict == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
