# ADR-0003: Serial-transport self-echo is misconfiguration, not traffic

Date: 2026-08-28
Status: Accepted
Related: #104, #106, #96
Cross-links: `docs/DESIGN.md` (Terms: Framing/Transport; One product, two seams; Units ladder — "bytes: serial rendering (codec, TXEN, SYN preamble)"), `docs/two-wire-echo-bench-findings.md`, ADR-0001
Companion issues (to be filed, cited from this ADR):
- **Serial-transport `rxDuringTxObserved` defect signal.**
  Add a serial-transport-scoped signal for self-echo. The signal
  fires when RX bytes arrive while TXEN is asserted and the discard
  is not active (`AlwaysOff`, or `Auto` before arming). Surface
  the signal in tracer status. In `Auto` mode, the signal arms the
  byte-level cancel and emits the diagnostic ("self-echo observed:
  misconfigured bus driver — `!RE` stuck active; echo-cancel
  remediation enabled"). Keep the signal on `SerialCMRITransport`.
  Do not put it on the base `CMRITransport` seam or `CMRIHost`. A
  future `TcpCMRITransport` must not fabricate it. TDD plus desktop
  tests.

  The steady-state truth table (all rows are local `!RE` — "can I
  hear myself talk?"):

  | # | Configuration | Occurrence | What it is |
  |---|---|---|---|
  | 1 | `!RE` tied to GND (current rev) | 100% | self-echo |
  | 2 | Software `!RE`, correct | never | deterministic |
  | 3 | New rev (`!RE` tied to TXEN) | never | physically absent |

  The two-pin ordering race (intermittent self-echo from
  TXEN/`!RE` overlap) is a non-issue with the tied-pin wiring. One
  GPIO makes both-active or both-inactive impossible. Foreign-
  transmitter contention is out of scope. It shows up as garbage
  and aborts before STX sync, not as RX-during-TX. It trips the
  decoder restart and abort counters, not this signal.

  Shape: a boolean latch (`rxDuringTxObserved`). The truth table
  is binary (always or never). A count of bursts adds nothing.
  There is no transient case to distinguish from persistent. Do not
  use a per-byte counter. It is noise that mirrors the TX rate. The
  latch answers yes or no.

- **Board-rev verification (TXEN tied to !RE).** Blocked on new
  hardware. Re-run the Phase A `XiaoBenchEcho` probe on a rev'd
  cpNode-Xiao board whose MAX3491 `!RE` is driven by TXEN, not tied
  to GND. Expect discard `AlwaysOff` or Auto-never-triggered,
  self-echo = 0, and stray `0x00` = 0. The defect the mitigation
  compensates for is physically absent. This work is outstanding for
  both the Host side (this library) and #9's future Node side until
  new boards are available.

## Context

Issue #104 and PR #106 shipped a byte-level echo-cancel in
`SerialCMRITransport`. `pumpReceive_` discards RX bytes while
`txState_ == kWriting`. `sendPacket` adds a one-char-time guard
band. The work framed this as "2-wire support." That reads as a
feature — a thing the library does because 2-wire is a supported
mode.

The 2026-08-28 bench (`docs/two-wire-echo-bench-findings.md`)
reframes it. On a correctly-wired 2-wire bus, the receiver is off
during transmit (`!RE` driven by TXEN). There is no echo at all.
The echo the Host sees is a defect signal. It means the
transceiver `!RE` is stuck active. On this bench, `!RE` is tied to
GND. That is a 4-wire design choice held over onto a 2-wire
jumpered bus. The echo-cancel does not provide 2-wire support. It
mitigates a misconfigured bus driver so the library degrades safely
on hardware that someone has not fixed.

This is a serial-transport-layer condition. It is bound to the
physical serial medium and the transceiver receiver-enable wiring.
It is not an image-layer, frame-layer, or protocol-level fault. It
does not exist on a non-serial carrier.

### The trailing NULL is a load-bearing safety property, not an assertion

The bench also measured a framing-valid `0x00` one char time after
the last echo byte, in `kIdle` (after TXEN deassert). A prior
cpNode sketch hung in an infinite loop. It waited for another RX
character because a stray NULL triggered a decoder bug. That
cautionary tale means the NULL fate through this library decoder
needs a test, not an assertion. Code reading says a lone `0x00` fed
to `CMRIFrameDecoder` in `kHunt` is dropped by `handleData_`. Pre-
STX bytes are never stored. The decoder returns to `kHunt`. But
whether the decoder is genuinely in `kHunt` when the NULL arrives,
and whether it stays clean, is a runtime fact. Phase B characterizes
this: feed the real transport the measured echo-plus-NULL
sequence and prove the decoder returns to a clean `kHunt` with no
partial frame held and no counter storm. The ADR claim about the
NULL is conditional on that test passing.

## Decision

Three layers, separated by scope:

1. **Hardware is the fix.** Tie `!RE` to TXEN (DE and `!RE` from
   one GPIO). The receiver is physically off while the driver is
   enabled. There is no echo and no trailing artifact. This is a
   board rev, not firmware. It is invisible on 4-wire. The only time
   `!RE` goes high is while the Host transmits on the poll pair. The
   protocol forbids a reply during a poll (one talker at a time). RX
   is enabled whenever a reply could legitimately arrive.

2. **The library echo-cancel (`#104`) is mitigation, not a feature.**
   It compensates for the misconfiguration when someone has not
   fixed the hardware. Do not describe, name, or evolve it as if
   self-echo were expected bus traffic. That framing invites
   treating the defect as a supported operating mode.

   Scope of the byte-level discard: it runs while TXEN is asserted
   — through `kWriting` and `kDraining`, until deassert. It does
   not run through `kWriting` alone (as shipped in v0.1.0).
   `kWriting` ends the moment the port accepts the last byte. For a
   small frame on a fast UART, that is near-instant. The self-echo
   arrives one char time later, squarely in `kDraining`. A
   `kWriting`-only discard is a near-empty mitigation for the very
   echo it claims to handle. Extending it through deassert loses no
   legitimate reply. A Node cannot reply until it has received ETX
   (interop 2.3.15, corrected by E10). The fastest real Node turns
   around in small milliseconds, far beyond the one-char-time guard
   band. The v0.1.0 `kWriting`-only scope came from the v1.1
   wording of 2.3.15, which mis-models the wire physics. A
   2026-08-28 bench showed cancel ON/OFF made no difference. The
   echo always arrived in `kDraining` when the discard was off.

3. **Instrumentation and remediation are one three-state
   mechanism, not a passive counter plus a flag.** The echo-cancel
   mode is a config knob with three values: `always off`, `always
   on`, and `auto` (the default). In `auto`, the transport watches
   for RX bytes while TXEN is asserted. On the first observation it
   arms the byte-level cancel and surfaces a diagnostic ("self-echo
   observed: misconfigured bus driver — `!RE` stuck active;
   echo-cancel remediation enabled"). `always off` is the explicit
   opt-out for known-correct 4-wire or fixed 2-wire hardware.
   `always on` forces the cancel regardless of observation. The
   defect signal (`rxDuringTxObserved`) and the remediation are
   one mechanism. The observation surfaces the diagnosis and arms
   the mitigation. This is an instrumentation-layer concern,
   recorded for the companion issue.

## The scope limit, and why it is a layering rule

The self-echo defect, the `rxDuringTxObserved` signal, and the
mitigation are members of the **serial transport** failure-mode
vocabulary. They are scoped to what a wired RS-485 medium can
actually exhibit. Do not lift them into the image, packet, or
host/protocol layers. Those layers are medium-neutral.

A non-serial carrier has a different and disjoint set of failure
modes. On CMRI-over-TCP there is no TXEN, no `!RE`, no echo, and
no stray byte. The echo defect and `rxDuringTxObserved` are out of
scope there. The meta-principle this ADR records:

> Each transport layer owns its own failure-mode vocabulary and
> instrumentation, scoped to what that medium can actually produce.
> An instrumentation signal that names a condition the underlying
> medium cannot produce is a design smell.

The layering test this decision must pass: adding a non-serial
transport (TCP, MQTT-as-carrier) must not require it to fabricate a
serial-specific signal to satisfy a host-scope or protocol-scope
interface. If it does, the layering is wrong. The signal has been
hoisted out of its layer. `CMRIHost` and the frame/codec stay
medium-neutral. `rxDuringTxObserved` lives in
`SerialCMRITransport` (or its port adapter), gated on the serial
medium.

This is consistent with DESIGN.md "packet seam is NOT product-wide."
The carrier-specific concerns stay at the carrier. The
strategy/product layers above them carry no protocol qualifier and
no carrier-specific failure vocabulary.

## Alternatives rejected

- **Treat echo as expected 2-wire traffic (the framing #104
  originally implied).** Rejected. It disguises a hardware defect as
  a supported mode. It guarantees the defect is never surfaced. It
  invites future "improvements" to the echo path as if echo were
  legitimate traffic. The bench showed the echo presence is itself
  the diagnosis.

- **Fix the library and leave the hardware.** Rejected as the whole
  answer. The mitigation is worth shipping. Deployed 4-wire boards
  held over to 2-wire need it. But it is not the cure. The trailing
  `0x00` artifact lands in `kIdle` after the cancel window. The
  byte-discard cannot reach it. Only the hardware fix removes it at
  the source.

- **Scope the discard to `kWriting` only (as shipped) to protect
  4-wire replies during drain.** Rejected on physics. A Node
  cannot reply until it has received ETX (interop 2.3.15, corrected
  by E10). No legitimate 4-wire reply arrives during the Host drain
  window. The fastest real Node turns around in small milliseconds,
  far beyond the one-char-time guard band. The `kWriting`-only
  scope came from the v1.1 wording of 2.3.15, which mis-models the
  wire. A 2026-08-28 bench showed cancel ON/OFF made no difference.
  The echo arrived in `kDraining` when the discard was off.
  Extending the discard through deassert loses nothing and catches
  the echo. The desktop test `test_rx_works_while_transmit_drains`
  encodes the fiction. The fake port queues a reply at tick 1
  "while draining." The fake port models no wire propagation and no
  node processing latency. A real node could not produce that byte
  at that instant. The test passes because it tests a model, not the
  physics. It needs a physics-aware replacement.

- **Hoist `rxDuringTxObserved` to the host/protocol layer as a
  generic "echo" counter.** Rejected on the layering rule above. It
  would force every transport, including a future TCP/MQTT carrier,
  to implement a signal for a condition that medium cannot produce.

- **Rely on UART hardware error flags to reject the stray `0x00`.**
  Rejected by measurement. The bench read `err0` (no break, no
  framing, no parity) on every burst. The stray byte is framing-
  valid. It reaches the RX FIFO as a real byte. The library cannot
  lean on the hardware error gate. On idle the SYN-preamble design
  absorbs it. A lone `0x00` cannot start `FF FF STX`. The real risk
  is interleaving with a node reply, which is the Phase C question.

## Consequences

- The `#104` echo-cancel stays. Its documentation and any future
  evolution describe it as mitigation-of-misconfiguration, not a
  feature. The byte-discard scope extends through TXEN deassert
  (`kWriting` and `kDraining`), not `kWriting`-only. The
  `setEchoCancelEnabled(bool)` flag is superseded by the three-
  state mode (`always off`, `always on`, `auto`). The boolean
  becomes the `always on`/`always off` explicit choice. `auto` is
  the new default. It self-detects the defect. `begin()` must not
  clobber the mode. The v0.1.0 `begin()` unconditionally resets
  the flag to true. The three-state mode needs the survives-begin
  contract the inter-byte timeout already has.

- A serial-transport `rxDuringTxObserved` defect signal is added
  (companion issue). TDD plus desktop tests. Surface it in tracer
  status. It is gated on the serial medium. It does not appear in
  `MockCMRITransport` or a future `TcpCMRITransport` contract. In
  `auto` mode it is also the trigger that arms the cancel and emits
  the diagnostic.

- `docs/DESIGN.md` gains no new normative clause from this ADR
  yet. The serial-rendering rung ("bytes: codec, TXEN, SYN
  preamble") already places TXEN at the correct layer. This ADR
  records the semantic boundary (echo = defect signal;
  per-transport failure vocabulary) that the normative text does
  not state explicitly. A clause may be promoted on review.

- The board-rev verification is a blocked companion issue. Re-run
  the Phase A probe on TXEN-tied-to-`!RE` hardware. Expect echo =
  0 and stray = 0.

## Revisit trigger

Revisit when a second carrier transport (TCP or MQTT-as-carrier)
is designed or implemented. At that point, confirm its failure-mode
vocabulary is disjoint from the serial transport. Confirm no host-
scope or protocol-scope interface forces it to implement
`rxDuringTxObserved` or any serial-specific signal. Either promote
the per-transport-failure-vocabulary rule into a DESIGN clause, or
close this ADR as Accepted with the clause in place.
