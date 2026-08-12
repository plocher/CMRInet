# CMRInet implementations — cross-review synthesis

Synthesis of four parallel adversarial reviews against NMRA LCS-9.10.1 v1.1
(`docs/lcs-9.10.1_cmrinet_v1.1.pdf`). Detailed findings with spec/code
citations live in the per-implementation reports:

- `review-CMRI-adams.md` — ArduinoCMRI (Michael Adams), node-side, ~330 lines
  (reviewed at v1.5, re-scanned at upstream v1.7.0 — see its delta section)
- `review-cpNode-simple.md` — cpNode ("John's Simple"), node-side, ~690 lines
- `review-cpCMRI-enhanced.md` — cpCMRI v0.0.3 ("John's Enhanced"), node-side, ~1050 lines
- `review-MRCS-kernel.md` — MRCS_cpNode_kernel v1.6 monolithic sketch, ~1550 lines

Plus three Host-side (master) reviews — synthesized in §6:

- `review-JMRI-cmri-host.md` — JMRI's CMRI stack, the dominant fielded Host
- `review-CMRI-Controller-host.md` — SBHRS pre-JMRI USIC Host (Turbo-C, 1998–2014)
- `review-QBASIC-CTC-host.md` — SoCal Ry CTC machine on Chubb's own SPS
  subroutines (2003) — the spec's reference lineage

Finding totals: 27 [BUG], 16 [SPEC-DEVIATION], ~9 tagged [SPEC-AMBIGUITY]
(consolidating to 7 distinct spec defects below), 18 [DESIGN-LIMITATION],
25 [STRENGTH]. After the v1.7.0 rescan, 3 Adams findings are fixed
upstream (off-by-one accessors, missing 0x02 escape, INIT parse/flush)
and 2 new ones were found (see §4).

## 1. The spec itself is defective in known, specific ways

All four reviewers independently converged on the same core spec defect,
plus several secondary ones. These are the authoritative list for any
future master/node work:

1. **I-message bodies are not parseable under the spec's own rules**
   (the unanimous #1). DLE escaping is mandated only for T/R bodies
   (p.6 §D.a), yet I bodies legally contain raw 2/3/16 (dH/dL is any
   16-bit value; Table 1 CT values include 2; NS can be 2 or 3). A
   byte-scanning receiver cannot distinguish a CT byte of 0x03 from ETX,
   and non-addressed nodes told to "discard until ETX" (p.5) will
   early-terminate on such bytes. **De-facto resolution used by the
   whole fielded ecosystem: DLE-process every message body, all types.**
   NOW SETTLED WITH PRIMARY-SOURCE AUTHORITY (§6): all three reviewed
   Hosts escape I bodies — including Chubb's own SPS reference lineage,
   the very code the spec cites as its normative reference — and JMRI
   has a unit test asserting it. The spec text is the defect.
2. **Stop bits: spec says 8N1; the installed base is split.** Adams
   library + JMRI + classic Chubb practice: 8N2 (primary citation now
   in hand: Chubb's SPS sets LCR=7 with an in-code note recommending
   2 stop bits at 57600+, sanctioning 8N1 only ≤28800; JMRI is 8N2 by
   constant). MRCS kernel and the SBHRS Host: 8N1. Since a TX with
   2 stop bits is readable by an 8N1 RX (extra stop = idle), 8N2 TX is
   the interop-safe transmit choice; RX should be 8N1-tolerant.
3. **No node-side error/recovery semantics.** Timeout is defined only
   for the Host (p.8). Truncated frames, unknown MTs, oversized bodies,
   inter-byte gaps: all implementation-invented — and this is exactly
   where every implementation's bugs cluster.
4. **dH/dL transmission-delay granularity unspecified** ("between
   transmissions", p.6): de-facto = per transmitted character; also
   only SMINI/SUSIC/USIC are named, so whether CPNODE must honor it is
   open. All AVR implementations silently mis-honor delays >16383 µs.
   Not vestigial: the SBHRS Host shipped dH/dL=0/200 (2 ms/char) as
   self-defense for a slow DOS receiver — the living reason the field
   exists.
5. **C-type (CPNODE) I-body layout is undefined.** The spec names
   NDP 'C' but never defines its NS/CT semantics. The cpNode family
   uses a private dialect: `<NDP><DLH><DLL><opts1><opts2><NIN><NOUT>`.
   A master must speak this dialect for cpNodes.
6. **SYN handling underspecified**: is the 2-SYN preamble mandatory to
   *require*? May extra SYNs appear? Is unescaped 0xFF in a body legal
   (yes — nobody escapes it, and all four handle raw 0xFF-in-body
   correctly, but the spec never says).
7. **Body length "0 to 256"**: pre- or post-DLE-stuffing is unstated
   (wire frames can be ~2x logical length); pre-init behavior (may a
   node answer polls before I?) unstated — fielded nodes do.

## 2. De-facto wire conventions a master MUST follow

Distilled from what fielded nodes actually require/tolerate:

- **Escape 2/3/16 in EVERY body you transmit — including I messages.**
  Never escape 0xFF. (Both John parsers and the kernel unescape all
  bodies; Adams' ignore-path assumes escaped I bodies too.)
- **Send exactly two SYNs, then STX.** Adams' parser *drops the frame*
  on a third 0xFF; nobody requires more than two; the kernel doesn't
  require any (bare STX starts a frame there — one more reason the
  master must never emit stray 0x02 between frames).
- **Emit clean, gapless, complete frames.** Multiple nodes buffer
  pre-STX garbage into the body (inCnt bug) or exit their flush when
  the UART goes momentarily empty; mid-frame pauses and inter-frame
  debris actively corrupt fielded nodes' state.
- **Send the full output image in every T frame** — short T frames make
  the kernel and cpNode-simple drive outputs from stale RAM.
- **Poll timeout must be per-node configurable — reply latency is
  version-dependent.** Adams ≤v1.5 nodes hard-delay 50 ms before every
  reply (timeout floor ~60–100 ms); Adams v1.5.1+ replies in ~50 µs;
  kernel-class nodes reply in ~1 loop pass (can be ~25 ms with heavy
  IOX config); dH/dL adds per-character delay if nonzero (send 0).
- **Tolerate replies that overlap your own ETX.** Fast nodes (Adams
  v1.5.1+, kernel) start replying while the master's last byte may
  still be draining — drop the master's driver promptly after flush,
  and on 2-wire setups expect a contention window.
- **After sending I, immediately follow with a full T.** Adams v1.7.0
  decodes the INIT payload into the same buffer its outputs are read
  from — outputs transiently drive garbage until the first T arrives.
- **Treat missed responses as retryable, and re-INIT after repeated
  misses.** Nodes desync, eat frames after INIT (Adams), or answer
  polls before ever being initialized (kernel). A silent node is
  recoverable; a persistently silent node needs operator attention.
- **Master-side RS-485 discipline**: assert TXEN → write → `flush()`
  to full drain → drop TXEN immediately (nodes reply fast; only the
  legacy Adams ≤v1.5 50 ms delay ever masked sloppy masters — modern
  nodes won't).
- **UA range**: spec allows 0–127, but the cpNode family clamps to
  0–64 (and silently *changes* out-of-range to 64 — two misconfigured
  nodes can collide on address 64). Master should support 0–127 and
  warn above 64.

## 3. Recurring bug patterns (the anti-checklist)

The same defects appear independently in multiple codebases — these are
the things the master engine's receiver must get right, and the exact
list a test suite should cover:

- **Blocking byte reads without timeout** (kernel, cpNode-simple;
  Adams/cpCMRI are non-blocking): one truncated frame = node hung
  forever / watchdog reset. → Non-blocking state machine + inter-byte
  timeout (~2–3 char times) that resets to hunt state.
- **Body index not reset at STX + pre-header bytes buffered** (kernel,
  cpNode-simple): one noise byte shifts the whole body → silent wrong
  outputs. → Reset index on STX; never store while not in-frame.
- **Off-by-one overrun guards** (`>` vs `>=`: kernel, cpNode-simple,
  Adams accessors — the latter fixed upstream in v1.5.1/v1.6.0, and
  notably the v1.5.1 fix itself over-corrected before v1.6.0 got it
  right; cpCMRI ring-buffer slot mismatch): 1–2 byte OOB writes into
  adjacent I/O state. → Guard before every store; unit-test the
  boundaries.
- **No length validation** of received bodies vs configured geometry
  (all four): short/long frames silently mis-drive outputs, worst case
  an infinite TX loop jamming the bus (cpNode-simple `byte j < int i`).
- **Missing TX-side DLE escaping or incomplete escape set** (cpCMRI:
  none at all; Adams ≤v1.6.0: forgets 0x02, fixed in v1.7.0): the
  kernel and Adams v1.7.0 are the only fully spec-correct TX escape
  tables. Masters must still tolerate unescaped 0x02 from fielded
  ≤v1.6.0 Adams nodes.
- **TXEN dropped before UART drain** (cpCMRI when GPIO TXEN is used):
  truncated replies on generic MAX485 hardware; invisible on cpNode
  hardware with auto-TXEN. cpNode-simple gets this exactly right.
- **No frame-level abort/no double buffering** (all): partially
  received or poisoned frames become live output data. → Parse into a
  staging buffer; commit to consumers only on valid ETX.
- **`char`-typed protocol constants / signedness traps** (kernel;
  cpNode-simple is consistent-but-fragile): 0xFF comparisons silently
  change meaning between AVR and ARM/ESP32. → `uint8_t` at the read.
- **Debug channels that corrupt the protocol or the node itself**
  (cpCMRI ships with per-byte tracing ON, sometimes onto the CMRI
  port; cpNode-simple's debug sprintf can smash its own UA). → Debug
  off by default, never on the protocol Stream, bounded formatting.
- **Untested "obvious" code** (cpCMRI's on-disk src is an uncommitted
  refactor that cannot pass a single byte — livelocked reader, ring
  off-by-one; its `|`-vs-`&` flag tests): none of these survive a
  five-line unit test with a mock Stream. **The single strongest
  process lesson: the master library needs a mock-Stream test harness
  from day one.**

## 4. Scorecard: what each implementation contributes

**ArduinoCMRI (Adams)** — Best structural idea: a fully DLE-aware
ignore path for non-addressed traffic; clean composable byte-at-a-time
`_decode()` + `process_char()` (multi-node on one port); since v1.7.0,
a native Unity test suite + CI with a mocked Stream — the exact test
pattern the master library should copy. Upstream is actively
maintained: v1.5.1–v1.7.0 fixed the off-by-one accessors, the missing
0x02 escape, the INIT parse/flush header-spoof, and the 50 ms reply
delay (now ~50 µs). Still present at v1.7.0: third-SYN frame drop, no
inter-byte timeout (dangling-DLE frame corruption), poll answered
before its ETX is verified, dH/dL ignored, no double buffering. New
in v1.7.0: INIT payload decoded into the live output buffer — outputs
drive garbage between I and the first T.

**cpNode (Simple)** — Best: textbook TXEN discipline
(assert/write/flush/deassert), correct 0xFF decisions both directions,
honest self-documenting comments. Worst: blocking reads, inCnt bug,
unvalidated lengths with a possible infinite TX loop, address clamp.
Its `getPacket()` is the PLAN.md reference — carry over its DLE
ordering, not its blocking core.

**cpCMRI (Enhanced)** — Best *architecture* of the four: resumable
non-blocking per-byte state machine (RESET/SYNC/HEADER/ADDRESS/PTYPE/
BODY/ESCAPE), full 256-byte bodies with guard-before-store, packet as
a value type, `packetToString()` debug affordances. Worst: the on-disk
src is broken WIP (livelock + ring off-by-one — the hardware-tested
code is at git tag `checkpoint1`), no TX escaping at all, TXEN drop
before drain, uninitialized members. **Action item: the working tree
of `libraries/cpCMRI` should be reconciled with `checkpoint1` before
anything else is built on it.**

**MRCS kernel** — Best: the only fully spec-correct TX escape table
(with the definitive don't-escape-0xFF comment), pre-latched poll
responses for minimal turnaround, IOX OR-latching of short pulses,
8N1 data point from fielded MRCS hardware. Worst: blocking reads,
inCnt/overrun bugs, shared RX/TX buffer (stale-data hazards), flush
exits on empty. It defines de-facto behavior for deployed MRCS nodes:
masters must escape I bodies and never rely on node resilience.

## 5. Implications for the master library — seed for the brainstorm

What the research settles (proposed as decisions, open to challenge):

- **The receive core should be cpCMRI's architecture with the missing
  hygiene added**: resumable non-blocking state machine, explicit
  ESCAPE state, DLE-before-ETX ordering, guard-before-store, PLUS
  inter-byte timeout → reset, error counters, staging buffer with
  commit-on-ETX, `uint8_t` discipline, all members initialized.
- **Wire policy (transmit)**: exactly two SYNs; escape 2/3/16 in all
  bodies including I; never escape 0xFF; full output image every T;
  TXEN assert → write → flush → deassert; default 8N2 TX (configurable
  8N1), tolerate both on RX; dH/dL = 0.
- **Poll engine policy**: per-node timeout (default ~100 ms, tunable
  down), miss counter with re-INIT backoff (already in PLAN.md — the
  research confirms and quantifies it), treat pre-init R replies and
  post-INIT swallowed frames as normal, budget for 50 ms-delay nodes.
- **Test-first**: a mock-Stream harness that can replay byte sequences
  (including every pathological case in §3) is justified by four
  codebases' worth of evidence that hardware bring-up does not catch
  these bugs. This also gives the emulator/testbed a fuzzing seed list.
  Adams v1.7.0's Unity + mocked-Stream + CI setup is a working example
  to crib from — and a cautionary one: its suite still misses the
  third-SYN drop, so the §3 anti-checklist must drive the test plan.
- **Node version matters**: the same library differs materially across
  fielded versions (Adams v1.5 vs v1.7.0: reply latency 50 ms vs 50 µs,
  escaping behavior, INIT handling). The master's per-node config
  should not bake in one version's quirks.

Open questions to brainstorm (not settled by the research):

1. **One library or two?** The framing/DLE/state-machine core is
   demonstrably shared (all four node implementations AND the master
   need it); the *engines* differ (node reacts, master owns schedule +
   timeouts). Options: (a) single lib with `CMRI_Master`/`CMRI_Node`
   classes over a shared framing core; (b) master-only lib now
   (PLAN.md's current stance), merge later; (c) three-layer split:
   framing core lib + node lib + master lib. The research strengthens
   the case that the shared core is real and small (~150–250 lines)
   — but also shows the risk of refactoring deployed node code
   (cpCMRI's broken WIP is a cautionary tale).
2. **API shape for the master**: callback-driven (cpCMRI-style
   handlers), polled state inspection (cpNode-style), or a per-node
   session object (`CMRINode` handle with `.outputs()`, `.inputs()`,
   `.state()`, `.stats()`)? How much of JMRI's node-table concept
   (NDP types, geometry per node) belongs in the library vs the
   sketch?
3. **Multi-node now or later?** PLAN.md says one hardcoded slave.
   The poll-list/timeout machinery is identical for N nodes; the
   research suggests designing the state machine per-node from the
   start costs little and avoids a second refactor.
4. **Strictness knobs**: should the master have a "conformance test"
   mode (strict spec framing, deliberate edge-case injection — raw
   0xFF bodies, escaped-byte-in-last-position, max-length frames,
   truncated frames) to exercise nodes as a protocol testbed? The
   §3 anti-checklist is effectively the test plan.
5. **Which node bugs are worth fixing upstream** (separate efforts):
   cpCMRI src reconciliation with checkpoint1; cpNode-simple's
   blocking read + inCnt + overrun trio. Adams upstream is confirmed
   active (v1.7.0 fixed several of our findings independently) —
   remaining candidates for upstream PRs: third-SYN drop, inter-byte
   timeout, INIT-clobbers-output-buffer, poll-before-ETX; the repo's
   test suite makes these easy to submit with regression tests.
## 6. Host-side synthesis — the answer key
Three Hosts reviewed: JMRI (dominant, current), SBHRS CMRI-Controller
(pre-JMRI USIC era), and the QBASIC CTC machine built on Chubb's own
SPS subroutines (the spec's reference lineage). Together they settle
most of what §2 could only recommend.
### Settled wire facts (all three Hosts agree)
- **Escape 2/3/16 in every TX body including I; never escape 0xFF.**
  Unanimous across three decades of Hosts; JMRI unit-tests it. Also:
  JMRI's CPNODE I bodies end with SIX RAW 0xFF pad bytes — any parser
  that resyncs on SYN mid-frame breaks on every JMRI cpNode init.
- **Exactly two SYNs on TX; require none on RX.** All three Host
  receivers hunt a bare STX and ignore SYNs entirely. "Two SYNs" is a
  TX-side convention only. Bench instrument: count SYNs, never require.
- **UA verified on RX by classic Hosts, NOT by JMRI.** SBHRS and QBASIC
  strictly check reply UA and MT; JMRI checks neither (any STX..ETX
  frame satisfies the outstanding poll, and its sensor manager decodes
  ANY reply body as input bits routed by UA — an echoed T flips
  sensors). The new Host must verify UA+MT like the classics.
- **dH/dL: send 0** (QBASIC/JMRI default 0; SBHRS's 2 ms was DOS-era
  self-defense). Expose per-node as a compatibility/conformance knob.
### JMRI's numbers (what modern nodes are tuned against)
- Poll (P) timeout **250 ms** hard constant; per-node knobs exist but
  are dead code. **No retransmission ever**; on timeout, next node.
- Re-init ladder: **>5 consecutive P timeouts → re-I + forced full T +
  all sensors → UNKNOWN**; silent nodes retried forever, never dead.
- **500 ms always-consumed stall after every I** (engine waits for a
  reply that never comes); ~**2 ms gap after T**; **5 ms poll pacing**;
  poll order = node registration order; per-node wire order I → T → P.
- **T on change only, always full image** (classic Hosts instead send
  full-image T every cycle unconditionally — both patterns are
  fielded; nodes must tolerate either; periodic-T-refresh is a safe
  optional extra).
- **Only nodes with ≥1 input sensor are ever polled** — output-only
  nodes never get P and are never re-inited (users define dummy inputs
  as a workaround). Bench Host: poll everything or offer keepalive.
- **Replies capped at 120 bytes (118 data)** — an ecosystem ceiling:
  JMRI cannot poll nodes with >118 input bytes; longer R frames desync
  its receiver. The spec's 256 is theoretical.
- **CPNODE I-body dialect pinned** (the bytes CMRInet must emit
  and its emulator must parse): `'C' dH dL opts1 opts2 NI NO FF×6`
  (13-byte body); opts1 bits {0:USECMRIX, 1:SENDEOT, 2:USEBCC} default
  0; NI/NO include the onboard 2+2. JMRI also speaks NDP 'O' (CPMEGA)
  and extended MTs E/Q/D/W/A/C/M — none in LCS-9.10.1; don't choke on
  them, decode 'E' (SENDEOT).
### Historical facts that change assumptions
- **Inter-byte TX gaps are historically normal.** The QBASIC reference
  Host sent byte-at-a-time THR-polled with interpreter-scale gaps;
  classic nodes therefore tolerate arbitrary intra-frame spacing.
  §2's "gapless frames" rule protects the *newer* node code (flush-on-
  empty, inter-byte-timeout bugs), not the classics — keep it, but add
  a slow-byte-spaced-TX conformance test to flush out intolerant nodes.
- **Classic Hosts never re-init mid-session** (I sent once, ever) — so
  fielded classic-node re-init paths are under-exercised; bench-test
  them deliberately. JMRI's re-init ladder is the modern behavior.
- **The reference Host trained the ecosystem's conformance**: QBASIC
  rejects replies with wrong UA/MT or unescaped 2/3 in bodies — which
  is *why* fielded nodes escape correctly. Its SMINI/SUSIC CT
  validation tables are the only executable spec of CT semantics;
  lift them into the config validator and test vectors.
- **Host-side bugs mirror the node anti-checklist** (§3 applies to both
  sides): blocking/no-timeout reads (SBHRS DOS spin, QBASIC babbling-
  node stall, JMRI parked in loadChars), DLE-blind STX hunts (SBHRS,
  QBASIC), dangling-DLE eats ETX (JMRI), commit-before-validate (QBASIC
  stores a fabricated 0x00 on DLE-then-timeout — occupied block reads
  VACANT: the sharpest argument anywhere for commit-on-ETX staging),
  POSIX IXON left enabled freezing TX on a raw 0x13 (SBHRS unix).
### Bench-instrument requirements (vetted features to borrow)
- From JMRI: passive metrics listener (timeout/truncated/unrecognized
  counters + 10-poll averaged turnaround), NDP-aware packet monitor
  with per-node/per-type filters and raw-vs-decoded DLE view, walking-
  bit + loopback wraparound diagnostics, halt-polling primitive, hex
  packet injector, TCP transport for the same byte protocol. JMRI has
  NO node emulator (its "simulator" is a null modem) — CMRInet's
  emulator fills a real gap.
- From SBHRS: whole-app emulation behind a one-flag wire stub (real vs
  emulated node behind the same interface), domain-level AND bit-level
  stimulus injection, live per-card pin view with physical↔logical pin
  mapping, frame tracing taxonomy (frame summary / byte trace /
  errors-only) with named protocol bytes and grouped-binary payloads,
  event log with unseen-entry highlighting, UART-overrun counters
  distinct from protocol errors, loud-not-hanging failure mode.
- From QBASIC: per-node UART error attribution, conformance-event
  taxonomy as counters (bad-UA/bad-MT/unescaped-2-3/ETX-mismatch),
  cycle-time telemetry, perturbation-aware tracing (off by default),
  and a **"classic reference emulation mode"** (8N2, generous timeout,
  immediate re-poll, never re-init, full-image T every cycle) as a
  ground-truth baseline for A/B-ing node behavior.
### New Host-side design rules for CMRInet
- Verify reply UA and MT=='R' against the outstanding poll; count and
  discard everything else. Size RX for 256-byte bodies (beat JMRI's
  118 ceiling). Inter-byte timeout on RX; dangling DLE = framing error.
- Timeouts wall-clock, per-byte AND per-message, per-node configurable
  (defaults compatible with JMRI-tuned nodes: 250 ms P, ~500 ms post-I
  settle configurable down, ~2 ms post-T, ~5 ms pacing, >5-miss
  re-init with full T + input-state invalidation).
- Per-node freshness/health as first-class API state (QBASIC's silent
  stale-inputs hold is the cautionary tale), never a hidden print.
- TX staging sized 2×body+8 or escape-while-streaming (two classic
  Hosts have latent worst-case-escaping overflows).
- Desktop-side tooling: fully raw termios (no IXON) — the SBHRS unix
  freeze is invisible until one specific data byte appears.
