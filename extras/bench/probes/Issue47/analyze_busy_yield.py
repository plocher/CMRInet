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

import _gap_deltas

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

    # v1 columns: stall_ms,period_ms,verdict,tx_events,max_gap_ms,median_gap_ms,p90_gap_ms,capture,transcript
    cap_idx = header.index("capture")
    v1_verdict_idx = header.index("verdict")
    
    header_v2 = header + ["verdict_v2", "monotone_prefix_max_ms", "it_count", "poll_count"]
    
    rows_v2 = []
    transitions = defaultdict(list)
    
    for row in rows:
        capture_name = row[cap_idx]
        v1_verdict = row[v1_verdict_idx]
        log_path = sweep_path / capture_name
        
        if log_path.exists():
            try:
                with open(log_path, "r", errors="replace") as f:
                    res = _gap_deltas.analyze_lines(f.readlines())
                v2_verdict = res.verdict
                m_max = res.monotone_prefix_max
                it_count = res.it_count
                poll_count = res.poll_count
            except Exception as e:
                v2_verdict = _gap_deltas.VERDICT_ERROR
                m_max = -1
                it_count = 0
                poll_count = 0
        else:
            v2_verdict = _gap_deltas.VERDICT_ERROR
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

    res = _gap_deltas.analyze_lines(lines)
    _gap_deltas.print_result_text(res)
    
    if res.verdict == _gap_deltas.VERDICT_PASS:
        return 0
    return 1

if __name__ == "__main__":
    sys.exit(main(sys.argv))

