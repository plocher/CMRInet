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
THRESHOLD_MS = 5000
DEFAULT_PHANTOM_UA = 31

PKT_RE = re.compile(
    r"^PKT\s+t=(?P<t>\d+)\s+(?P<dir>TX|RX)\s+ua=(?P<ua>\d+)(?:\s+mt=(?P<mt>[A-Z]))?"
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


def print_result_text(res: AnalyzerResult, header_metadata: dict = None):
    if header_metadata:
        print("=== Log Context ===")
        for k, v in header_metadata.items():
            print(f"{k.ljust(18)}: {v}")
        print("===================")
        print()
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


