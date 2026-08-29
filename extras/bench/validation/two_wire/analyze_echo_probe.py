#!/usr/bin/env python3
"""analyze_echo_probe — characterize the 2-wire self-echo.

Reads a XiaoBenchEcho capture (one JSON event per line, grouped by burst
id "n") and reports OBSERVED FACTS about the echo: presence, latency,
duration, placement relative to the TXEN assert/deassert edges, STRAY
(non-marker) RX bytes, and UART break/framing/parity flags. This is the
baseline the CMRInet echo-cancel assumptions (issue #104 / PR #106) are
later held against — it does NOT pass/fail the library.

Event vocabulary (one JSON object per line):
  {"e":"assert","n":N,"us":..}      TXEN asserted
  {"e":"tx","n":N,"us":..}          marker gapless-queued to the UART
  {"e":"drained","n":N,"us":..}     shift register empty (wire edge)
  {"e":"deassert","n":N,"us":..}    TXEN deasserted (at the drain edge)
  {"e":"rx","n":N,"us":..,"v":B}    one RX byte observed
  {"e":"end","n":N,"us":..}         capture window closed
  {"e":"errM","n":N,"us":..}        UART error mask M: bit0=brk,1=frm,2=par

Usage:
    analyze_echo_probe.py <results-dir> [--write-summary]
    analyze_echo_probe.py <capture.log>
"""
from __future__ import annotations

import argparse
import json
import statistics
import sys
from collections import Counter
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Optional

DEFAULT_MARKER = [170, 85, 170, 85]  # 0xAA, 0x55, 0xAA, 0x55


@dataclass
class BurstResult:
    """One burst's observed echo evidence."""

    n: int
    assert_us: Optional[int] = None
    tx_us: Optional[int] = None
    drained_us: Optional[int] = None
    deassert_us: Optional[int] = None
    rx: list[tuple[int, int]] = field(default_factory=list)  # (us, value)
    err_mask: Optional[int] = None
    err_us: Optional[int] = None
    echo_present: bool = False
    echo_full: bool = False
    echo_bytes: int = 0
    rx_order_ok: bool = False
    latency_assert_us: Optional[int] = None
    latency_drain_us: Optional[int] = None
    duration_us: Optional[int] = None
    rx_before_deassert: int = 0
    rx_after_deassert: int = 0
    tx_wire_us: Optional[int] = None
    # Stray (non-marker) RX bytes — the deassert-edge artifact signal.
    stray_bytes: int = 0
    stray_before_deassert: int = 0
    stray_after_deassert: int = 0
    stray_values: list[int] = field(default_factory=list)
    # UART hardware error flags latched during the burst.
    err_brk: bool = False
    err_frm: bool = False
    err_par: bool = False


@dataclass
class EchoCharacterization:
    """Aggregate observed-facts summary across all captured bursts."""

    present: bool
    marker: list[int]
    bursts_seen: int
    bursts_with_echo: int
    bursts_with_full_marker: int
    bursts_with_stray: int
    bursts_with_brk: int
    bursts_with_frm: int
    bursts_with_par: int
    total_stray_bytes: int
    stray_values: dict[int, int]
    stray_before_deassert_frac: float
    latency_assert_min: Optional[int]
    latency_assert_mean: Optional[float]
    latency_assert_max: Optional[int]
    duration_mean: Optional[float]
    rx_before_deassert_frac: float
    summary: str
    bursts: list[BurstResult]


def _group_bursts(lines: list[str]) -> dict[int, BurstResult]:
    """Parse JSON event lines into per-burst records, keyed by burst id."""
    bursts: dict[int, BurstResult] = {}
    for line in lines:
        s = line.strip()
        if not s.startswith("{"):
            continue
        try:
            doc = json.loads(s)
        except json.JSONDecodeError:
            continue
        n = doc.get("n")
        e = doc.get("e")
        if not isinstance(n, int) or not isinstance(e, str):
            continue
        us = doc.get("us")
        if not isinstance(us, int):
            continue
        b = bursts.setdefault(n, BurstResult(n=n))
        if e == "assert":
            b.assert_us = us
        elif e == "tx":
            b.tx_us = us
        elif e == "drained":
            b.drained_us = us
        elif e == "deassert":
            b.deassert_us = us
        elif e == "rx":
            v = doc.get("v")
            if isinstance(v, int):
                b.rx.append((us, v))
        elif e == "end":
            pass
        elif e.startswith("err") and len(e) > 3:
            try:
                b.err_mask = int(e[3:])
                b.err_us = us
            except ValueError:
                pass
    return bursts


def _finalize(b: BurstResult, marker: list[int], marker_set: set[int]) -> None:
    """Compute one burst's echo/stray/error evidence from its events."""
    rx_echo = [(us, v) for us, v in b.rx if v in marker_set]
    rx_stray = [(us, v) for us, v in b.rx if v not in marker_set]
    b.echo_bytes = len(rx_echo)
    b.echo_present = b.echo_bytes > 0
    seq = [v for _, v in rx_echo]
    b.rx_order_ok = seq == marker
    b.echo_full = b.rx_order_ok and b.echo_bytes == len(marker)
    if rx_echo:
        first_us, last_us = rx_echo[0][0], rx_echo[-1][0]
        if b.assert_us is not None:
            b.latency_assert_us = first_us - b.assert_us
        if b.drained_us is not None:
            b.latency_drain_us = first_us - b.drained_us
        b.duration_us = last_us - first_us
    if b.deassert_us is not None:
        for us, _ in rx_echo:
            if us < b.deassert_us:
                b.rx_before_deassert += 1
            else:
                b.rx_after_deassert += 1
        for us, _ in rx_stray:
            if us < b.deassert_us:
                b.stray_before_deassert += 1
            else:
                b.stray_after_deassert += 1
    if b.tx_us is not None and b.drained_us is not None:
        b.tx_wire_us = b.drained_us - b.tx_us
    b.stray_bytes = len(rx_stray)
    b.stray_values = [v for _, v in rx_stray]
    if b.err_mask is not None:
        b.err_brk = bool(b.err_mask & 0x1)
        b.err_frm = bool(b.err_mask & 0x2)
        b.err_par = bool(b.err_mask & 0x4)


def analyze_lines(lines: list[str], marker: list[int] | None = None) -> EchoCharacterization:
    """Characterize the self-echo across all bursts in a capture."""
    mk = marker if marker is not None else DEFAULT_MARKER
    mk_set = set(mk)
    bursts = _group_bursts(lines)
    for b in bursts.values():
        _finalize(b, mk, mk_set)
    ordered = sorted(bursts.values(), key=lambda x: x.n)
    with_echo = sum(1 for b in ordered if b.echo_present)
    with_full = sum(1 for b in ordered if b.echo_full)
    with_stray = sum(1 for b in ordered if b.stray_bytes > 0)
    with_brk = sum(1 for b in ordered if b.err_brk)
    with_frm = sum(1 for b in ordered if b.err_frm)
    with_par = sum(1 for b in ordered if b.err_par)
    total_stray = sum(b.stray_bytes for b in ordered)
    stray_counter: Counter[int] = Counter()
    for b in ordered:
        stray_counter.update(b.stray_values)
    total_stray_before = sum(b.stray_before_deassert for b in ordered)
    total_stray_all = sum(b.stray_before_deassert + b.stray_after_deassert for b in ordered)
    stray_before_frac = (total_stray_before / total_stray_all) if total_stray_all else 0.0
    lats = [b.latency_assert_us for b in ordered if b.latency_assert_us is not None]
    durs = [b.duration_us for b in ordered if b.duration_us is not None]
    total_before = sum(b.rx_before_deassert for b in ordered)
    total_rx = sum(b.rx_before_deassert + b.rx_after_deassert for b in ordered)
    rx_before_frac = (total_before / total_rx) if total_rx else 0.0
    present = with_echo > 0

    if not present:
        summary = (
            "NO ECHO OBSERVED — the bus is not 2-wire looped "
            "(check the TX+ to RX+ and TX- to RX- jumpers) or the receiver "
            "is disabled while TXEN is asserted. This is the first gate: "
            "no later 2-wire step is meaningful until an echo is seen."
        )
    else:
        lat_mean = statistics.fmean(lats) if lats else 0.0
        dur_mean = statistics.fmean(durs) if durs else 0.0
        parts = [
            f"ECHO CONFIRMED on {with_echo}/{len(ordered)} bursts",
            f"latency(assert->first-rx) mean {lat_mean:.0f}us "
            f"(min {min(lats)} / max {max(lats)})" if lats else "latency n/a",
            f"echo duration mean {dur_mean:.0f}us",
            f"{rx_before_frac * 100:.1f}% of echo bytes arrived before TXEN deassert",
            f"full marker recovered on {with_full}/{with_echo} bursts",
        ]
        if total_stray:
            vals = ", ".join(f"0x{v:02X}x{c}" for v, c in sorted(stray_counter.items()))
            parts.append(
                f"STRAY bytes on {with_stray}/{len(ordered)} bursts "
                f"({total_stray} total: {vals}); "
                f"{stray_before_frac * 100:.1f}% before deassert"
            )
        else:
            parts.append("no stray (non-marker) RX bytes")
        errparts = []
        if with_brk:
            errparts.append(f"BRK on {with_brk}")
        if with_frm:
            errparts.append(f"FRM on {with_frm}")
        if with_par:
            errparts.append(f"PAR on {with_par}")
        parts.append("UART err: " + (" ".join(errparts) if errparts else "none"))
        summary = "; ".join(parts) + "."

    return EchoCharacterization(
        present=present,
        marker=list(mk),
        bursts_seen=len(ordered),
        bursts_with_echo=with_echo,
        bursts_with_full_marker=with_full,
        bursts_with_stray=with_stray,
        bursts_with_brk=with_brk,
        bursts_with_frm=with_frm,
        bursts_with_par=with_par,
        total_stray_bytes=total_stray,
        stray_values=dict(stray_counter),
        stray_before_deassert_frac=stray_before_frac,
        latency_assert_min=min(lats) if lats else None,
        latency_assert_mean=statistics.fmean(lats) if lats else None,
        latency_assert_max=max(lats) if lats else None,
        duration_mean=statistics.fmean(durs) if durs else None,
        rx_before_deassert_frac=rx_before_frac,
        summary=summary,
        bursts=ordered,
    )


def _load_marker_from_manifest(target: Path) -> list[int]:
    """Read the probe marker from the gather manifest, else use the default."""
    mf = target / "manifest.json"
    if not mf.exists():
        return DEFAULT_MARKER
    try:
        doc = json.loads(mf.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError):
        return DEFAULT_MARKER
    mk = (doc.get("epoch") or {}).get("marker")
    if isinstance(mk, list) and mk:
        try:
            return [int(x) for x in mk]
        except (TypeError, ValueError):
            pass
    return DEFAULT_MARKER


def main() -> int:
    """Characterize one results directory or a raw capture log."""
    p = argparse.ArgumentParser(description="Characterize the 2-wire self-echo")
    p.add_argument("target", help="Results directory or capture log")
    p.add_argument("--write-summary", action="store_true",
                   help="Write summary.json for directory targets")
    args = p.parse_args()

    target = Path(args.target)
    marker = DEFAULT_MARKER
    capture_path = target
    summary_path: Optional[Path] = None

    if target.is_dir():
        marker = _load_marker_from_manifest(target)
        mf = target / "manifest.json"
        if mf.exists():
            try:
                doc = json.loads(mf.read_text(encoding="utf-8"))
                cf = doc.get("capture_file")
                if isinstance(cf, str):
                    capture_path = target / cf
            except (json.JSONDecodeError, OSError):
                pass
        if args.write_summary:
            summary_path = target / "summary.json"
    if not capture_path.exists():
        print(f"ERROR: capture not found: {capture_path}", file=sys.stderr)
        return 2

    lines = capture_path.read_text(encoding="utf-8", errors="replace").splitlines()
    res = analyze_lines(lines, marker=marker)

    print(f"Capture: {capture_path}")
    print(f"Marker: {' '.join(f'0x{v:02X}' for v in res.marker)}")
    print(f"Bursts seen: {res.bursts_seen}  with echo: {res.bursts_with_echo}  "
          f"full marker: {res.bursts_with_full_marker}  "
          f"with stray: {res.bursts_with_stray}  "
          f"BRK/FRM/PAR: {res.bursts_with_brk}/{res.bursts_with_frm}/{res.bursts_with_par}")
    for b in res.bursts:
        print(
            f"  burst {b.n}: echo={b.echo_present} full={b.echo_full} "
            f"bytes={b.echo_bytes} orderOk={b.rx_order_ok} "
            f"lat={b.latency_assert_us}us dur={b.duration_us}us "
            f"beforeD={b.rx_before_deassert} after={b.rx_after_deassert} "
            f"stray={b.stray_bytes}({b.stray_before_deassert}b/{b.stray_after_deassert}a) "
            f"err={'B' if b.err_brk else ''}{'F' if b.err_frm else ''}{'P' if b.err_par else ''}{'' if b.err_mask == 0 else ''} "
            f"txWire={b.tx_wire_us}us"
        )
    print()
    print(res.summary)

    if summary_path is not None:
        summary_path.write_text(
            json.dumps(asdict(res), indent=2) + "\n", encoding="utf-8"
        )
        print(f"\nWrote summary: {summary_path}")

    # Exit 0 whether or not an echo was seen — this phase reports facts,
    # it does not pass/fail the library. Non-zero is reserved for
    # unusable input (no capture file), handled above.
    return 0


if __name__ == "__main__":
    sys.exit(main())
