#!/usr/bin/env python3
"""Unit tests for analyze_echo_probe — synthetic self-echo timelines.

The probe emits one JSON event per line, grouped by burst id ("n"):
  {"e":"assert","n":..,"us":..}      TXEN asserted
  {"e":"tx","n":..,"us":..}           marker gapless-queued to the UART
  {"e":"drained","n":..,"us":..}      shift register empty (wire edge)
  {"e":"deassert","n":..,"us":..}     TXEN deasserted (at the drain edge)
  {"e":"rx","n":..,"us":..,"v":..}    one RX byte observed
  {"e":"end","n":..,"us":..}          capture window closed

These tests feed hand-built timelines and assert the characterization.
"""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import analyze_echo_probe as ae  # noqa: E402

MARKER = [170, 85, 170, 85]  # 0xAA, 0x55, 0xAA, 0x55


def _line(e: str, n: int, us: int, v: int | None = None) -> str:
    s = f'{{"e":"{e}","n":{n},"us":{us}'
    if v is not None:
        s += f',"v":{v}'
    return s + "}"


def _burst_clean() -> list[str]:
    # All four echo bytes arrive while TXEN is asserted (before deassert).
    return [
        _line("assert", 0, 1000),
        _line("tx", 0, 1010),
        _line("drained", 0, 2400),
        _line("rx", 0, 1100, 170),
        _line("rx", 0, 1450, 85),
        _line("rx", 0, 1800, 170),
        _line("rx", 0, 2150, 85),
        _line("deassert", 0, 2410),
        _line("end", 0, 2500),
    ]


def _burst_tail() -> list[str]:
    # Fourth echo byte arrives after TXEN deassert (the tail question).
    return [
        _line("assert", 1, 10000),
        _line("tx", 1, 10010),
        _line("drained", 1, 11400),
        _line("rx", 1, 10100, 170),
        _line("rx", 1, 10450, 85),
        _line("rx", 1, 10800, 170),
        _line("deassert", 1, 11410),
        _line("rx", 1, 11500, 85),
        _line("end", 1, 12000),
    ]


def test_clean_echo_characterized() -> None:
    res = ae.analyze_lines(_burst_clean(), marker=MARKER)
    assert res.present is True
    assert res.bursts_seen == 1
    assert res.bursts_with_echo == 1
    assert res.bursts_with_full_marker == 1
    b = res.bursts[0]
    assert b.echo_present is True
    assert b.echo_full is True
    assert b.echo_bytes == 4
    assert b.rx_order_ok is True
    assert b.rx_before_deassert == 4
    assert b.rx_after_deassert == 0
    assert b.latency_assert_us == 100
    assert b.duration_us == 1050
    assert b.tx_wire_us == 1390


def test_partial_and_after_deassert() -> None:
    res = ae.analyze_lines(_burst_clean() + _burst_tail(), marker=MARKER)
    assert res.bursts_seen == 2
    assert res.bursts_with_echo == 2
    assert res.bursts_with_full_marker == 2  # both recovered all four, in order
    b1 = res.bursts[1]
    assert b1.rx_before_deassert == 3
    assert b1.rx_after_deassert == 1
    # 7 of 8 echo bytes arrived before deassert across both bursts
    assert abs(res.rx_before_deassert_frac - 0.875) < 1e-9
    # both bursts had latency 100 (first rx - assert)
    assert res.latency_assert_mean == 100.0
    assert res.latency_assert_min == 100
    assert res.latency_assert_max == 100


def test_no_echo_means_not_present() -> None:
    lines = [
        _line("assert", 0, 1000),
        _line("tx", 0, 1010),
        _line("drained", 0, 2400),
        _line("deassert", 0, 2410),
        _line("end", 0, 2500),
    ]
    res = ae.analyze_lines(lines, marker=MARKER)
    assert res.present is False
    assert res.bursts_with_echo == 0
    assert "NO ECHO" in res.summary


def test_garbage_lines_skipped() -> None:
    lines = ["garbage", "", "{not json", _line("assert", 9, 5)] + _burst_clean()
    res = ae.analyze_lines(lines, marker=MARKER)
    # burst 9 has only an assert (no echo); burst 0 is the clean one
    assert res.bursts_seen == 2
    assert res.bursts_with_echo == 1
    assert res.bursts_with_full_marker == 1


def test_out_of_order_echo_not_full() -> None:
    lines = [
        _line("assert", 0, 1000),
        _line("tx", 0, 1010),
        _line("drained", 0, 2400),
        _line("rx", 0, 1100, 85),   # wrong order: 0x55 first
        _line("rx", 0, 1450, 170),
        _line("rx", 0, 1800, 170),
        _line("rx", 0, 2150, 85),
        _line("deassert", 0, 2410),
        _line("end", 0, 2500),
    ]
    res = ae.analyze_lines(lines, marker=MARKER)
    b = res.bursts[0]
    assert b.echo_present is True
    assert b.echo_bytes == 4
    assert b.rx_order_ok is False
    assert b.echo_full is False
    assert res.bursts_with_full_marker == 0


def test_stray_byte_after_deassert_surfaced() -> None:
    # A non-marker 0x00 arrives after TXEN deassert (the deassert-edge
    # artifact). It must surface as a stray, not be folded into echo stats.
    lines = _burst_clean()[:7] + [
        _line("deassert", 0, 2410),
        _line("rx", 0, 3142, 0),    # stray NULL after deassert
        _line("end", 0, 3500),
        _line("err0", 0, 3501),     # no HW error flag (yet)
    ]
    res = ae.analyze_lines(lines, marker=MARKER)
    b = res.bursts[0]
    assert b.echo_bytes == 4
    assert b.rx_before_deassert == 4
    assert b.rx_after_deassert == 0       # echo stays all-before
    assert b.stray_bytes == 1
    assert b.stray_after_deassert == 1
    assert b.stray_before_deassert == 0
    assert b.stray_values == [0]
    assert res.bursts_with_stray == 1
    assert res.total_stray_bytes == 1
    assert res.stray_values == {0: 1}
    assert res.stray_before_deassert_frac == 0.0
    assert "STRAY" in res.summary


def test_err_event_parsed_brk_frm() -> None:
    # err3 = bit0(brk) + bit1(frm) latched during the burst.
    lines = _burst_clean() + [_line("err3", 0, 2510)]
    res = ae.analyze_lines(lines, marker=MARKER)
    b = res.bursts[0]
    assert b.err_mask == 3
    assert b.err_brk is True
    assert b.err_frm is True
    assert b.err_par is False
    assert res.bursts_with_brk == 1
    assert res.bursts_with_frm == 1
    assert res.bursts_with_par == 0
    assert "BRK on 1" in res.summary
    assert "FRM on 1" in res.summary


def test_stray_correlated_with_brk_frm() -> None:
    # The expected deassert-edge signature: stray 0x00 after deassert AND
    # a latched brk+frm flag, on the same burst.
    lines = _burst_clean()[:7] + [
        _line("deassert", 0, 2410),
        _line("rx", 0, 3142, 0),
        _line("end", 0, 3500),
        _line("err3", 0, 3501),
    ]
    res = ae.analyze_lines(lines, marker=MARKER)
    b = res.bursts[0]
    assert b.stray_bytes == 1 and b.stray_values == [0]
    assert b.err_brk is True and b.err_frm is True
    assert res.bursts_with_stray == 1 and res.bursts_with_brk == 1


def test_single_byte_marker_full_match() -> None:
    # The TXEN-deassertion correlation test: marker = one byte (0x5A).
    mk = [0x5A]
    lines = [
        _line("assert", 0, 1000),
        _line("tx", 0, 1010),
        _line("drained", 0, 1400),
        _line("rx", 0, 1100, 90),    # 0x5A echo
        _line("deassert", 0, 1410),
        _line("rx", 0, 1800, 0),     # stray NULL after deassert
        _line("end", 0, 2500),
        _line("err3", 0, 2510),
    ]
    res = ae.analyze_lines(lines, marker=mk)
    b = res.bursts[0]
    assert b.echo_full is True
    assert b.echo_bytes == 1
    assert b.stray_bytes == 1 and b.stray_values == [0]
    assert b.err_brk is True and b.err_frm is True
    assert res.marker == [0x5A]
