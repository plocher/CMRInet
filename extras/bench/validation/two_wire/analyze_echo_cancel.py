#!/usr/bin/env python3
"""analyze_echo_cancel — Phase B echo-cancel capture analyzer.

Reads a Phase B gather output (manifest + capture log of PKT lines + the
host status snapshot) and characterizes the echo's path through the real
stack for one capture. Reports observed facts:

  - repliesRejected and unsolicitedPackets from the host status snapshot
    (the defect signal: cancel OFF -> rej climbs once per poll; ON -> flat)
  - the RX traffic seen in the ring (which MTs, which UAs) — whether the
    echo assembled into frames that reached drainReceive_
  - decoder stats (timeoutAborts, framesDecoded) for the trailing 0x00

This does NOT pass/fail the library; it reports what the real stack did
with the echo, so the ADR's mechanism claims are held against observations.

Usage:
    analyze_echo_cancel.py <results-dir> [--write-summary]
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Optional

PKT_RE = re.compile(
    r"^PKT\s+t=(?P<t>\d+)\s+(?P<dir>TX|RX)\s+UA=(?P<ua>\d+)\s+mt=(?P<mt>[A-Z])"
)


@dataclass
class PhaseBResult:
    echocancel: str
    ua: int
    bursts_seen: int
    tx_i_or_t: int
    tx_polls: int
    rx_frames: int
    rx_mt_counts: dict[str, int]
    rx_ua_counts: dict[int, int]
    replies_rejected: Optional[int]
    unsolicited_packets: Optional[int]
    decode_errors: Optional[int]
    frames_decoded: Optional[int]
    timeout_aborts: Optional[int]
    summary: str


def _analyze_lines(lines: list[str], ua: int) -> tuple[int, int, int, int,
                                                       dict[str, int], dict[int, int]]:
    tx_i_or_t = 0
    tx_polls = 0
    rx_frames = 0
    rx_mt: dict[str, int] = {}
    rx_ua: dict[int, int] = {}
    bursts_seen = 0
    for line in lines:
        m = PKT_RE.search(line)
        if not m:
            continue
        d = m.group("dir")
        mt = m.group("mt")
        u = int(m.group("ua"))
        if d == "TX":
            if mt in ("I", "T"):
                tx_i_or_t += 1
            if mt == "P":
                tx_polls += 1
        elif d == "RX":
            rx_frames += 1
            rx_mt[mt] = rx_mt.get(mt, 0) + 1
            rx_ua[u] = rx_ua.get(u, 0) + 1
    return bursts_seen, tx_i_or_t, tx_polls, rx_frames, rx_mt, rx_ua


def _load_manifest(target: Path) -> dict:
    mf = target / "manifest.json"
    if not mf.exists():
        raise FileNotFoundError(f"manifest not found: {mf}")
    return json.loads(mf.read_text(encoding="utf-8"))


def main() -> int:
    p = argparse.ArgumentParser(description="Analyze Phase B echo-cancel capture")
    p.add_argument("target", help="Results directory from gather_echo_cancel.py")
    p.add_argument("--write-summary", action="store_true")
    args = p.parse_args()

    target = Path(args.target)
    if not target.is_dir():
        print(f"ERROR: target must be a results directory: {target}", file=sys.stderr)
        return 2
    try:
        manifest = _load_manifest(target)
    except (FileNotFoundError, json.JSONDecodeError) as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 2

    ec = manifest.get("echocancel", "unknown")
    ua = int(manifest.get("ua", 32))
    cap = target / manifest.get("capture_file", "")
    if not cap.exists():
        print(f"ERROR: capture not found: {cap}", file=sys.stderr)
        return 2
    lines = cap.read_text(encoding="utf-8", errors="replace").splitlines()

    _, tx_it, tx_p, rx_n, rx_mt, rx_ua = _analyze_lines(lines, ua)

    status = manifest.get("status_snapshot") or {}
    host_stats = status.get("host") or {}
    rej = host_stats.get("repliesRejected")
    uns = host_stats.get("unsolicitedPackets")
    dec_errs = host_stats.get("decodeErrors")
    # decoder stats may be nested under the host or a transport block
    frames_dec = host_stats.get("framesDecoded")
    timeout_ab = host_stats.get("timeoutAborts")

    # Build a human summary.
    if ec == "off":
        verdict = ("cancel OFF: echo reached drainReceive_ — "
                   f"repliesRejected={rej}, unsolicitedPackets={uns} "
                   "(defect exposed; mitigation disabled)")
    elif ec == "on":
        verdict = ("cancel ON: mitigation active — "
                   f"repliesRejected={rej}, unsolicitedPackets={uns} "
                   "(expect flat if the echo was eaten before drainReceive_)")
    else:
        verdict = f"echocancel={ec} (unknown state)"

    summary = (
        f"echocancel={ec}; TX I/T={tx_it}, TX P={tx_p}, RX frames={rx_n}; "
        f"RX mt={rx_mt}; RX ua={rx_ua}; "
        f"repliesRejected={rej}, unsolicitedPackets={uns}, "
        f"decodeErrors={dec_errs}, framesDecoded={frames_dec}, "
        f"timeoutAborts={timeout_ab}. {verdict}"
    )

    res = PhaseBResult(
        echocancel=ec,
        ua=ua,
        bursts_seen=0,
        tx_i_or_t=tx_it,
        tx_polls=tx_p,
        rx_frames=rx_n,
        rx_mt_counts=rx_mt,
        rx_ua_counts=rx_ua,
        replies_rejected=rej,
        unsolicited_packets=uns,
        decode_errors=dec_errs,
        frames_decoded=frames_dec,
        timeout_aborts=timeout_ab,
        summary=summary,
    )

    print(f"Capture: {cap}")
    print(f"echocancel: {ec}  phantom UA: {ua}")
    print(f"TX: I/T={tx_it}, P={tx_p}")
    print(f"RX frames in ring: {rx_n}  (mt={rx_mt}, ua={rx_ua})")
    print(f"host: repliesRejected={rej}, unsolicitedPackets={uns}, "
          f"decodeErrors={dec_errs}, framesDecoded={frames_dec}, "
          f"timeoutAborts={timeout_ab}")
    print()
    print(summary)

    if args.write_summary:
        sp = target / "summary.json"
        sp.write_text(json.dumps(asdict(res), indent=2) + "\n", encoding="utf-8")
        print(f"\nWrote summary: {sp}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
