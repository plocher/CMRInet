# Two-wire self-echo probe (Phase A)
Bench validation suite that characterizes the RS485 self-echo on a 2-wire
(single-pair) bus **before** any CMRInet echo-cancel assumption is trusted.

## Why this exists
Issue #104 / PR #106 ships a byte-level echo-cancel whose timing model
(echo confined to the TX-asserted window; a one-char-time guard band
covers the tail) is unvalidated on real 2-wire wire. This suite builds a
base of *observed facts* — echo presence, latency, duration, placement
relative to the TXEN assert/deassert edges, stray (non-marker) RX bytes,
and UART break/framing/parity flags — that the library's assumptions are
later held against. It does **not** pass/fail the library.

## What it measures
`XiaoBenchEcho` is a **library-free** probe: assert TXEN, gapless-write a
marker, poll RX and the UART shift-register-drain edge in a tight loop,
deassert TXEN at the drain edge (same discipline as the library), then
keep polling RX for a tail window. All events are timestamped with
`micros()` and emitted to USB CDC *after* the burst, so CDC writes do not
perturb the timing under measurement. No CMRInet transport, codec, or host
engine is compiled in — this isolates the transceiver and UART.

The OLED identifies the probe as `ECHO` (distinct from the tracer `TRC`,
sniffer `SNIFFER`, and cal host `CAL`) and shows live per-burst echo
status: marker hex, echo FULL/PART/NONE with byte count, stray count, and
a B/F/P flag when a UART break/framing/parity error latched. The
`display.display()` push blocks ~25ms, so every OLED draw is deferred to
*after* the timed burst and CDC emission — it never lands in the tight
RX/drain poll, so the timing measurement is unperturbed.

Per burst the analyzer reports:
- echo present? (any marker byte seen on RX)
- full marker recovered, in order?
- latency: TXEN-assert to first RX byte, and drain to first RX byte
- echo duration: first to last RX byte
- placement: how many echo bytes arrive **before** vs **after** TXEN deassert
- **stray (non-marker) RX bytes**: count, distinct values, before/after deassert
- **UART error flags**: break (B), framing (F), parity (P) latched during the burst
- TX wire time (drain minus tx queue) as a sanity check

## TXEN-deassertion correlation test
A single-byte marker (`--marker 5A`) sends `TXEN - 1 byte - !TXEN`, so the
deassert edge is the only event under study, isolated from multi-byte
traffic. A stray NULL (`0x00`) on RX paired with a latched UART
break/framing flag is the signature of a deassert-edge line artifact (line
floating/ringing when the driver releases an unterminated pair), not a
transmitted byte.

## Run
```shell
# 4-byte marker echo baseline (default), 20s
extras/bench/.venv/bin/python extras/bench/validation/two_wire/gather_echo_probe.py

# single-byte TXEN-deassertion correlation test
extras/bench/.venv/bin/python extras/bench/validation/two_wire/gather_echo_probe.py \
  --marker 5A --tag echo_deassert

# characterize a capture
extras/bench/.venv/bin/python extras/bench/validation/two_wire/analyze_echo_probe.py \
  extras/bench/validation/two_wire/data/results.YYYYMMDD.echo_deassert
```

`--no-flash` skips compile/upload if the probe is already on the board.
`--port` overrides the bench.json Host resolution. `--marker` takes
space-separated hex (`5A` or `AA 55 AA 55`); the probe confirms the new
marker back in its epoch, which the manifest records.

## Termination A/B
The bench runs unterminated by default (see docs/testbed-physical-notes.md).
To test whether the deassert-edge stray byte is a line-condition artifact:
1. Run the single-byte probe unterminated (above) -> baseline `echo_deassert`.
2. Add a 120 ohm termination across the pair at one end of the bus segment.
3. Re-run with `--no-flash --marker 5A --tag echo_deassert_term` (no reflash
   needed; the probe is already loaded).
4. Compare the two `summary.json` files: if the stray-byte count and the
   BRK/FRM counts drop with termination, the artifact is line-condition,
   not firmware.

## Reading the result
- **NO ECHO OBSERVED** -> the bus is not 2-wire looped (check the
  TX+ to RX+ and TX- to RX- jumpers) or the receiver is disabled while
  TXEN is asserted. First gate: nothing else is meaningful until an echo.
- **ECHO CONFIRMED** with a latency / duration / placement profile -> the
  baseline. **STRAY bytes** and **BRK/FRM** counts are the deassert-edge
  artifacts the later phases (cancel OFF vs ON, real nodes) must not
  confuse with real traffic.

## Tests
```shell
extras/bench/.venv/bin/pytest extras/bench/validation/two_wire/test_analyze_echo_probe.py
```
