# Two-wire self-echo bench findings

The factual record from the 2026-08-28 bench session that characterized the
RS485 self-echo on a 2-wire (single-pair, full-duplex-jumpered) bus before
trusting the CMRInet echo-cancel assumptions in issue #104 / PR #106.

Project-private and experimental. Companion tooling:
`extras/bench/validation/two_wire/` (the `XiaoBenchEcho` probe + gather/analyze).

## Setup

- Bus: 2-wire, cpNode-Xiao RS485 block with `R+` tied to `T+` and `R-` tied
  to `T-` at the terminal block (the classic Chubb 2-wire conversion).
- Transceiver: MAX3491. **`!RE` is tied to GND** (receiver always on) —
  a 4-wire design choice; see Root cause below.
- Host: one Xiao ESP32-C6 running `XiaoBenchEcho` v0.3.0, a library-free
  probe (no CMRInet transport/codec/host compiled in). It asserts TXEN,
  gapless-writes a marker, tight-polls RX and the UART shift-register drain
  edge, deasserts TXEN at the drain edge, and emits a `micros()`-stamped
  JSON timeline to CDC *after* the burst. UART break/framing/parity raw
  flags are cleared before and read after each burst.
- Baud 28800 8N2 (~382 us/char). Captures are 20 s, ~38 bursts each.

## Observed facts

Five runs, one variable changed per step. `results.*` under
`extras/bench/validation/two_wire/data/`.

| run | marker | termination | bytes sent | bursts | full marker | stray 0x00 | BRK/FRM/PAR |
|-----|--------|-------------|-----------|--------|-------------|-----------|-------------|
| echo_4byte | 4 B (AA 55 AA 55) | none | 4 | 38/39 | 38/38 | 38 | 0/0/0 |
| echo_4byte_term | 4 B | 120 ohm | 4 | 38/39 | 38/38 | 38 | 0/0/0 |
| echo_deassert | 1 B (5A) | none | 1 | 38/39 | 38/38 | 38 | 0/0/0 |
| echo_deassert_term | 1 B | 120 ohm | 1 | 38/38 | 38/38 | 38 | 0/0/0 |
| echo_deonly_term | — | 120 ohm | 0 | 38 | n/a | 0 | 0/0/0 |

Stable across every run that sent bytes:

- **Echo is faithful.** Full marker recovered, in order, on every echoing
  burst. No corruption, no reordering.
- **Echo timing matches line-rate physics.** First echo byte arrives one
  char time (~382 us) after the TX queue; bytes spaced one char time
  apart; echo duration = (n-1) char times; TX wire time = n char times.
- **The marker echo is entirely within the TXEN-asserted window.** The
  last echo byte lands ~8 us before the `uart_wait_tx_done` drain edge;
  deassert follows ~2-3 us after that. 100% of echo bytes arrive before
  deassert.
- **One stray `0x00` per burst, on every burst that sent bytes.** It
  arrives ~386 us after the last real echo byte (one char time), which is
  also ~386 us after deassert (the two intervals coincide because the last
  echo byte lands ~8 us before deassert).
- **No UART error flags, ever.** `err0` on every burst — no break, no
  framing, no parity latched. The stray `0x00` is framing-valid.

## What each step ruled out

- **echo_deassert (1 byte)** isolated the deassert edge from multi-byte
  traffic. The stray still appeared identical to the 4-byte case, so it is
  a per-deassert-edge phenomenon, not a multi-byte framing tail.
- **echo_4byte_term / echo_deassert_term (120 ohm)** tested the
  line-condition hypothesis. Termination changed nothing — not the stray
  count, not its timing, not the error flags, not the echo latency. So the
  stray is **not** passive-line ringing or a missing-fail-safe-bias
  artifact.
- **echo_deonly_term (0 bytes)** cycled TXEN assert/deassert with no UART
  TX content. **Zero RX bytes.** So the stray is **not** the DE/receiver
  transition by itself — TXEN dropping on a quiet UART produces nothing on
  RX. The artifact requires actual serial data to have been transmitted.

## Root cause

The stray `0x00` is a **trailing artifact of the always-on receiver
decoding the host's own echoed frame** on the 2-wire (jumpered) pair.

`!RE` is tied to GND, so the MAX3491 receiver never disables. On 2-wire,
the driver and receiver share the pair, so while DE is asserted the
receiver hears the host's own TX echoed back (the faithful marker echo
above). As the last echoed stop bit clears and the driver releases the
pair, the receiver — still synchronized to the echo stream — decodes one
more character as the differential collapses back to idle. That character
is framing-valid all-zeros: `0x00`. It is paced by the echo stream (one
char time after the last real echo byte), not by the deassert edge, which
is why the DE-only test (no echo stream) produced nothing.

This is the condition that the 2-wire RS-485 `!RE`-disable-during-TX
discipline exists to prevent: with `!RE` driven high during transmit, the
receiver is physically off while DE is asserted, so there is no echo to
decode and no trailing artifact.

## Where this refines the common explanation

External guidance (MAX3491 application notes) correctly identifies the
`!RE`-tied-to-GND condition and the "group DE and `!RE` together" fix. Two
of its specific claims are refined by this bench data:

1. **"Often accompanied by a UART framing error."** Not here. Every burst
   read `err0` — the stray `0x00` is framing-valid (clean start bit, eight
   zeros, a valid stop bit), so it reaches the UART RX FIFO as a real byte
   and would reach the CMRInet decoder, not be caught by a hardware error
   gate. The library cannot rely on the UART error flags to reject it.
2. **"Lack of bus termination/biasing" as the cause.** Contradicted by the
   A/B: 120 ohm termination did not suppress the stray. Termination is not
   the fix on this bench; the `!RE`->TXEN tie is. (Termination is still
   good practice for signal integrity; it just does not address this
   artifact.)

## Implication for #104 / PR #106

- The library's byte-discard (`pumpReceive_` while `txState_ == kWriting`)
  and the one-char-time guard band in `sendPacket` are the right
  defense-in-depth for the echo itself: the faithful marker echo lands
  entirely within `kWriting`, so it is eaten at the byte level before it
  assembles. That part of #104's model holds against the measured echo.
- The stray `0x00` lands in `kIdle` (after deassert, after drain), so the
  `kWriting` discard does **not** eat it — it reaches the decoder. On idle
  it is harmless: a lone `0x00` cannot start a CMRI frame (`FF FF STX`),
  and the inter-byte timeout expires it. The CMRI SYN-preamble design
  exists for exactly this class of inter-frame garbage.
- The real risk is Phase C (a node reply arriving right after the host's
  deassert): the stray `0x00` could interleave with the node's `R`-frame
  bytes and corrupt the decode. Whether that materializes depends on node
  reply timing relative to the ~386 us stray window.
- The hardware fix (board rev: tie `!RE` to TXEN so RX is disabled during
  TX) removes the echo and the stray at the source, making #104's
  echo-cancel defense-in-depth rather than the primary mechanism. With
  `!RE` driven, the byte-discard + guard band handle only the residual
  turn-off timing window.

## Artifacts

Captures + `summary.json` under
`extras/bench/validation/two_wire/data/results.20260828.{echo_4byte,echo_4byte_term,echo_deassert,echo_deassert_term,echo_deonly_term}`.
Reproduce with `extras/bench/validation/two_wire/gather_echo_probe.py`
(`--marker`, `--mode deonly`, `--no-flash`) and
`analyze_echo_probe.py`.

## Phase B — echo through the real stack (2026-08-28)

Phase A measured the echo with a library-free probe. Phase B ran that
same echo through the real CMRInet stack (CMRIHost + SerialCMRITransport
+ TracerShell) against a phantom UA (32, no board), so the only RX
traffic was the host's own self-echo. The echo-cancel mitigation
(`#104`) was toggleable at runtime over CDC (`echocancel on|off`), so
B1 (cancel OFF) and B2 (cancel ON) were one variable, no reflash.

Two 20 s captures, `results.20260828.echo_cancel_{off,on}`:

| | B1 (cancel OFF) | B2 (cancel ON) |
|---|---|---|
| TX I/T / P | 3 / 7 | 3 / 7 |
| RX echo frames assembled | 10 (I=1, T=2, P=7) | 10 (I=1, T=2, P=7) |
| unsolicited | 11 | 11 |
| rejected | 0 | 0 |
| decodeErrors | 0 | 0 |

**The cancel toggle made no difference.** Both runs are identical to
within noise. The host's own I/T/P echoes assembled into valid
frames and reached `drainReceive_` in both cases.

Why: the byte-discard is scoped to `kWriting`, which ends the moment
the port accepts the last byte — near-instant for a 6-byte frame on a
fast UART. The echo arrives one char time (~382 us) later, squarely in
`kDraining`, when `pumpReceive_` feeds the decoder normally regardless
of `echoCancelEnabled_`. The desktop test
`test_echo_cancel_does_not_discard_during_draining` pinned this; the
bench confirmed it on real wire. The shipped `kWriting`-only discard is
a near-empty mitigation for the echo it claims to handle.

What actually handled the echo (cancel-flag-independent): `drainReceive_`'s
frame-level classification. Every echo frame was classified as
**unsolicited (11), not rejected (0)**. The P-echo arrives before the
scheduler opens the reply gate (`kAwaitWait`), because the echo
round-trips faster than the tick cycle sets the phase, so it hits
`phase_ != kAwaitWait` → unsolicited, never reaching the `kMtMismatch`
path that #104's `repliesRejected`-pollution concern was about.

**NULL safety closed on real hardware.** `decodeErrors: 0` in both runs.
The stray `0x00` (measured in Phase A) is absorbed by the decoder's
hunt state on the real stack — no hang, no decode error, no counter
storm. The cpNode-hang-class concern is closed. The desktop tests
(`test_decode_null_after_frame_is_dropped_cleanly`,
`test_decode_null_while_hunting_does_nothing`) pin the mechanism.

## Phase B consequence — the physics correction

The bench showed the `kWriting`-only discard scope was set by interop
2.3.15 v1.1 ("expect a fast Node to begin its reply while the Host's
ETX still drains"), which mis-models the wire: a Node cannot reply until
it has received ETX, so no legitimate reply arrives during the Host's
drain window. Extending the byte-discard through `kDraining` (until
TXEN deassert) catches the self-echo at no cost to real traffic. That
correction is recorded as erratum E10 and applied to rule 2.3.15 (interop
profile v1.2), and the corrected mechanism is in ADR-0003 and DESIGN D13.

## Phase B artifacts

Captures + manifests under
`extras/bench/validation/two_wire/data/results.20260828.echo_cancel_{off,on}`.
Reproduce with `extras/bench/validation/two_wire/gather_echo_cancel.py`
(`--echocancel on|off`, `--no-flash`) and `analyze_echo_cancel.py`.
The probe is `extras/bench/XiaoBenchEchoCancel`.

## Phase C — real node on the 2-wire bus (2026-08-28)

Phase B used a phantom UA (no board, only self-echo). Phase C put a
real node (UA30, Xiao_I2C firmware, NI=7 NO=7) on the 2-wire bus,
so the host's RX now carries *both* the self-echo *and* a legitimate
R reply — the case the ADR's frame-level classification was for.
Cancel toggle was the one variable (C1 off, C2 on), no reflash.

Two 20 s captures, `results.20260828.echo_cancel_real_{off,on}`:

| | C1 (cancel OFF) | C2 (cancel ON) |
|---|---|---|
| TX I/T / P | 2 / 225 | 2 / 337 |
| RX in ring | 420 (I1 T1 P225 R193) | 649 (I1 T1 P337 R310) |
| polls / replies | 225 / 193 | 337 / 310 |
| rejected | 0 | 0 |
| unsolicited | 229 | 341 |
| decodeErrors | 0 | 0 |
| node 30 state | ONLINE | ONLINE |

**All four ADR predictions held on real wire.**

1. **Self-echo → unsolicited, not rejected.** The host's own I/T/P
   echoes assembled and reached `drainReceive_`, which classified
   every one as unsolicited (229/341), never rejected (0/0). The
   P-echo carries UA=30 and MT='P', which *would* hit `kMtMismatch`
   → rejected — but it arrives before the scheduler opens the reply
   gate (`kAwaitWait`), because the echo round-trips faster than
   the tick cycle sets the phase. So it hits `phase_ != kAwaitWait` →
   unsolicited, the path Phase B predicted. `repliesRejected` stays
   flat at 0 — the #104 pollution concern does not manifest.

2. **Node R reply → accepted.** 193/310 R replies from the real node
   were accepted (node 30 ONLINE in both runs). The R reply arrives
   *after* the host's TXEN deasserts (physics: the node cannot reply
   until it has received ETX), so the host is in `kAwaitWait` with the
   reply gate open, and the R matches UA + MT='R' → accepted.

3. **Cancel ON/OFF made no difference.** unsolicited/poll is ~1.02
   (C1) and ~1.01 (C2) — one self-echo per poll in both cases. The
   echo arrives in `kDraining` (not `kWriting`), so the byte-discard
   is off in both runs. This confirms the Phase B finding on real wire:
   the shipped `kWriting`-only discard is a near-empty mitigation,
   and the frame-level `drainReceive_` classification is the mechanism
   that actually handles the self-echo, cancel-flag-independent.

4. **Stray 0x00 is harmless, even with a real reply on the bus.**
   `decodeErrors: 0` in both runs. The stray `0x00` (measured in Phase A)
   is absorbed by the decoder's hunt state and does not interleave with
   or corrupt the node's R reply. The cpNode-hang-class concern is
   closed on real wire with real traffic.

**New observation (not an ADR prediction): the miss rate is ~12-14%
and cancel-independent.** C1: 32 misses/225 polls (14.2%); C2: 27/337
(8.0%). The rate is similar in both and is not affected by the cancel
toggle — consistent with the echo cancel only touching the host's RX
path, not the node's. The likely cause is node-side: on 2-wire with
`!RE` tied to GND, the *node* also hears its own reply echo, and the
cpNode firmware's own echo handling is a separate question from the
CMRInet library's #104. Flagged for the cpNode side, not this PR.

## Phase C consequence for the ADR

Phase C validated the ADR's core claim on real wire: **the
frame-level `drainReceive_` classification (saw our own UA/MT →
unsolicited) is the mechanism that handles the self-echo, and it is
cancel-flag-independent.** The byte-level discard (`kWriting`-only,
as shipped) is near-empty and made no observable difference in either
Phase B (phantom) or Phase C (real node). The corrected design
(ADR-0003 + DESIGN D13 + interop E10): extend the byte-discard
through `kDraining` to TXEN deassert as defense-in-depth, and keep
the frame-level classification as the content backstop that actually
works today.

## Phase C artifacts

Captures + manifests under
`extras/bench/validation/two_wire/data/results.20260828.echo_cancel_real_{off,on}`.
Reproduce with `extras/bench/validation/two_wire/gather_echo_cancel.py`
(`--node '30 7 7'`, `--echocancel on|off`, `--no-flash`) and
`analyze_echo_cancel.py`.
