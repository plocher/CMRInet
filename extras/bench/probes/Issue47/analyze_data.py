#!/usr/bin/env python3
"""Analyzer for regression #47 — backoff-under-loop-stall.

Consumes a `run.sh 47` capture (DIAG_TRACE lines of the form
``PKT t=<ms> TX ua=<n> mt=<c> len=<k>``) and reports the distribution
of gaps between successive TX polls of the phantom UA. A correct
`CMRIHost` should show gaps doubling from ~250 ms up to the 32 s cap.
The bug manifests as gaps cycling in a narrow band and never
accumulating.
"""

from __future__ import annotations

import argparse
import csv
import re
import statistics
import sys
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable, List, Tuple, Dict

VERDICT_PASS = "PASS"
VERDICT_FAIL_STUCK = "FAIL_STUCK"
VERDICT_FAIL_CYCLING = "FAIL_CYCLING"
VERDICT_FAIL_TOO_FEW = "FAIL_TOO_FEW"
VERDICT_FAIL_NO_TRACE = "FAIL_NO_TRACE"
VERDICT_FAIL_WRONG_UA_VOCAB = "FAIL_WRONG_UA_VOCAB"
VERDICT_ERROR = "ERROR"

CYCLING_DROP_RATIO = 0.5
CYCLING_HIGH_WATER_MS = 4000
THRESHOLD_MS = 8000
# Historical: the "phantom" is a deliberately-missing bench node the Host
# polls to drive poll-backoff. In the one-real-node era the phantom was
# UA 31; the bench later added a real node at UA 31, so the live phantom
# moved to UA 32 (compiled as TRACER_PHANTOM_UA in XiaoHostTracer). Kept
# at 31 here only to rescore old #47 captures; #47 is closed.
DEFAULT_PHANTOM_UA = 31

PKT_RE = re.compile(
    r"PKT\s+t=(?P<t>\d+)\s+(?P<dir>TX|RX)\s+ua=(?P<ua>\d+)(?:\s+mt=(?P<mt>[A-Z]))?"
)


@dataclass(frozen=True)
class Poll:
    """A single TX event to the phantom UA."""
    t_ms: int
    ua: int
    mt: str


@dataclass
class AnalyzerResult:
    verdict: str
    phantom_ua: int
    first_t_ms: int
    last_t_ms: int
    gaps: List[int] = field(default_factory=list)
    min_gap: int = -1
    max_gap: int = -1
    median_gap: int = -1
    p90_gap: int = -1
    monotone_prefix_max: int = -1
    it_count: int = 0
    poll_count: int = 0


def parse_events(lines: Iterable[str], ua: int) -> List[Poll]:
    """Extract TX events addressed to *ua* from a capture stream."""
    out: List[Poll] = []
    for line in lines:
        m = PKT_RE.search(line)
        if not m:
            continue
        if m.group("dir") != "TX":
            continue
        if int(m.group("ua")) != ua:
            continue
        # Default to "P" if old capture format lacking mt
        mt = m.group("mt") or "P"
        out.append(Poll(t_ms=int(m.group("t")), ua=ua, mt=mt))
    return out


def _looks_wire_encoded(lines: List[str], semantic_ua: int) -> bool:
    """True when capture appears wire-encoded where semantic UA was expected.

    This helper is used only after no semantic-UA poll events were found, so it
    refines failure diagnostics rather than changing a passing run to failing.
    """
    wire_ua = semantic_ua + ord("A")
    has_semantic = False
    has_wire = False
    for line in lines:
        m = PKT_RE.search(line)
        if not m or m.group("dir") != "TX":
            continue
        ua = int(m.group("ua"))
        if ua == semantic_ua:
            has_semantic = True
        if ua == wire_ua:
            has_wire = True
    return has_wire and not has_semantic


def classify_monotonicity(gaps: List[int]) -> Tuple[str, int]:
    """
    Classify the gap sequence into PASS, FAIL_STUCK, or FAIL_CYCLING.
    Also compute monotone_prefix_max: max(gaps[0..k]) where k is the
    largest index such that gaps[0..k] is non-decreasing.
    """
    if not gaps:
        return VERDICT_FAIL_TOO_FEW, -1

    # Calculate monotone_prefix_max
    monotone_prefix_max = gaps[0]
    for i in range(1, len(gaps)):
        if gaps[i] < gaps[i-1]:
            break
        monotone_prefix_max = max(monotone_prefix_max, gaps[i])

    max_gap = max(gaps)
    
    # Check for cycling: drops back.
    # FAIL_CYCLING iff there exist indices i < j such that 
    # gaps[j] < gaps[i] * 0.5 AND gaps[i] >= 4000 ms.
    is_cycling = False
    for i in range(len(gaps)):
        if gaps[i] >= CYCLING_HIGH_WATER_MS:
            for j in range(i + 1, len(gaps)):
                if gaps[j] < gaps[i] * CYCLING_DROP_RATIO:
                    is_cycling = True
                    break
        if is_cycling:
            break

    if max_gap >= THRESHOLD_MS:
        if is_cycling:
            return VERDICT_FAIL_CYCLING, monotone_prefix_max
        else:
            return VERDICT_PASS, monotone_prefix_max
    else:
        return VERDICT_FAIL_STUCK, monotone_prefix_max


def analyze_lines(lines: List[str], phantom_ua: int = DEFAULT_PHANTOM_UA) -> AnalyzerResult:
    if not any("PKT " in line for line in lines):
        return AnalyzerResult(
            verdict=VERDICT_FAIL_NO_TRACE,
            phantom_ua=-1,
            first_t_ms=-1,
            last_t_ms=-1
        )

    events = parse_events(lines, phantom_ua)
    
    it_count = sum(1 for e in events if e.mt in ("I", "T"))
    p_events = [e for e in events if e.mt == "P"]
    poll_count = len(p_events)

    if not p_events:
        if _looks_wire_encoded(lines, phantom_ua):
            return AnalyzerResult(
                verdict=VERDICT_FAIL_WRONG_UA_VOCAB,
                phantom_ua=phantom_ua,
                first_t_ms=-1,
                last_t_ms=-1,
                it_count=it_count,
                poll_count=0
            )
        return AnalyzerResult(
            verdict=VERDICT_FAIL_TOO_FEW,
            phantom_ua=phantom_ua,
            first_t_ms=-1 if not events else events[0].t_ms,
            last_t_ms=-1 if not events else events[-1].t_ms,
            it_count=it_count,
            poll_count=0
        )

    first_t_ms = p_events[0].t_ms
    last_t_ms = p_events[-1].t_ms

    if len(p_events) < 2:
        return AnalyzerResult(
            verdict=VERDICT_FAIL_TOO_FEW,
            phantom_ua=phantom_ua,
            first_t_ms=first_t_ms,
            last_t_ms=last_t_ms,
            it_count=it_count,
            poll_count=poll_count
        )

    gaps = [b.t_ms - a.t_ms for a, b in zip(p_events, p_events[1:])]
    g_sorted = sorted(gaps)
    min_gap = g_sorted[0]
    max_gap = g_sorted[-1]
    median_gap = int(statistics.median(gaps))
    p90_gap = g_sorted[int(0.9 * (len(g_sorted) - 1))]

    verdict, monotone_max = classify_monotonicity(gaps)

    return AnalyzerResult(
        verdict=verdict,
        phantom_ua=phantom_ua,
        first_t_ms=first_t_ms,
        last_t_ms=last_t_ms,
        gaps=gaps,
        min_gap=min_gap,
        max_gap=max_gap,
        median_gap=median_gap,
        p90_gap=p90_gap,
        monotone_prefix_max=monotone_max,
        it_count=it_count,
        poll_count=poll_count
    )


def print_result_text(res: AnalyzerResult):
    print(f"phantom UA        : {res.phantom_ua} ({res.poll_count} TX events, {res.it_count} I/T frames)")
    print(f"first / last t_ms : {res.first_t_ms} .. {res.last_t_ms}")
    if res.gaps:
        print(f"gaps (ms), median : {res.median_gap:.0f}")
        print(f"gaps (ms), min    : {res.min_gap}")
        print(f"gaps (ms), max    : {res.max_gap}")
        print(f"gaps (ms), p90    : {res.p90_gap}")
        print()
        print("gap sequence (ms):")
        for i, dg in enumerate(res.gaps):
            print(f"  {i:3d}: {dg}")
        print()
    
    if res.verdict == VERDICT_PASS:
        print(f"PASS: max gap {res.max_gap} ms >= {THRESHOLD_MS} ms (backoff is accumulating)")
    elif res.verdict == VERDICT_FAIL_STUCK:
        print(f"FAIL_STUCK: max gap {res.max_gap} ms < {THRESHOLD_MS} ms (backoff stuck; bug reproduced)")
    elif res.verdict == VERDICT_FAIL_CYCLING:
        print(f"FAIL_CYCLING: max gap {res.max_gap} ms, drops back below 50% (cycling)")
    elif res.verdict == VERDICT_FAIL_TOO_FEW:
        print(f"FAIL_TOO_FEW: insufficient data")
    elif res.verdict == VERDICT_FAIL_NO_TRACE:
        print(f"FAIL_NO_TRACE: no DIAG_TRACE lines in capture")
    elif res.verdict == VERDICT_FAIL_WRONG_UA_VOCAB:
        print(
            f"FAIL_WRONG_UA_VOCAB: capture appears wire-encoded for UA {res.phantom_ua}; "
            "decoded telemetry requires semantic UA"
        )
    
    if res.monotone_prefix_max >= 0:
        print(f"monotone_prefix_max: {res.monotone_prefix_max}")


def rescore(sweep_dir: str):
    sweep_path = Path(sweep_dir)
    csv_in = sweep_path / "summary.csv"
    csv_out = sweep_path / "summary_v2.csv"
    diff_out = sweep_path / "summary_v2_diff.md"

    if not csv_in.exists():
        print(f"ERROR: {csv_in} not found", file=sys.stderr)
        return 1

    with open(csv_in, "r") as f:
        reader = csv.reader(f)
        header = next(reader)
        rows = list(reader)
    def _safe_int(value: str, default: int = 0) -> int:
        try:
            return int(value)
        except Exception:
            return default

    # v1 columns: stall_ms,period_ms,verdict,tx_events,max_gap_ms,median_gap_ms,p90_gap_ms,capture,transcript
    cap_idx = header.index("capture")
    v1_verdict_idx = header.index("verdict")
    
    header_v2 = header + ["verdict_v2", "monotone_prefix_max_ms", "it_count", "poll_count"]
    
    rows_v2 = []
    transitions = defaultdict(list)
    
    for row in rows:
        capture_name = row[cap_idx].strip()
        v1_verdict = row[v1_verdict_idx].strip()
        log_path = sweep_path / capture_name
        is_skipped_row = v1_verdict.startswith("SKIPPED")
        
        if is_skipped_row:
            v2_verdict = v1_verdict
            if "monotone_prefix_max_ms" in header:
                m_max = _safe_int(row[header.index("monotone_prefix_max_ms")], default=0)
            else:
                m_max = 0
            if "it_count" in header:
                it_count = _safe_int(row[header.index("it_count")], default=0)
            else:
                it_count = 0
            if "poll_count" in header:
                poll_count = _safe_int(row[header.index("poll_count")], default=0)
            else:
                poll_count = 0
        elif capture_name and log_path.exists():
            try:
                with open(log_path, "r", errors="replace") as f:
                    res = analyze_lines(f.readlines())
                v2_verdict = res.verdict
                m_max = res.monotone_prefix_max
                it_count = res.it_count
                poll_count = res.poll_count
            except Exception as e:
                v2_verdict = VERDICT_ERROR
                m_max = -1
                it_count = 0
                poll_count = 0
        else:
            v2_verdict = VERDICT_ERROR
            m_max = -1
            it_count = 0
            poll_count = 0

        row_v2 = row + [v2_verdict, str(m_max), str(it_count), str(poll_count)]
        rows_v2.append(row_v2)
        
        if v1_verdict != v2_verdict:
            transitions[f"{v1_verdict} -> {v2_verdict}"].append((capture_name, m_max, row_v2))

    with open(csv_out, "w", newline='') as f:
        writer = csv.writer(f)
        writer.writerow(header_v2)
        writer.writerows(rows_v2)
        
    with open(diff_out, "w") as f:
        for trans, items in sorted(transitions.items()):
            f.write(f"## {trans}\n")
            for capture_name, m_max, row_v2 in items:
                f.write(f"- {capture_name}: monotone_prefix_max={m_max}\n")
            f.write("\n")
            
    print(f"Rescore complete. Wrote {csv_out} and {diff_out}")
    return 0


def main(argv: List[str]) -> int:
    parser = argparse.ArgumentParser(description="Analyzer for regression #47")
    parser.add_argument("target", help="Capture log file or sweep_results dir (if --rescore)")
    parser.add_argument("--phantom-ua", type=int, default=DEFAULT_PHANTOM_UA)
    parser.add_argument("--rescore", action="store_true", help="Batch rescore a directory")
    args = parser.parse_args(argv[1:])
    
    if args.rescore:
        return rescore(args.target)
        
    try:
        with open(args.target, "r", errors="replace") as f:
            lines = f.readlines()
    except FileNotFoundError:
        print(f"ERROR: File not found {args.target}", file=sys.stderr)
        return 2

    res = analyze_lines(lines, phantom_ua=args.phantom_ua)
    print_result_text(res)
    
    if res.verdict == VERDICT_PASS:
        return 0
    return 1

if __name__ == "__main__":
    sys.exit(main(sys.argv))

