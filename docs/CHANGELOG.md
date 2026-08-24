# Changelog

High-level changes, newest first.

## Unreleased

### Changed
- Host configuration error model (issue #80, Design v1.2 D5, breaking):
  `addRemoteNode(...)` now returns its **own** `ConfigStatus` instead of
  `CMRIHost&`, and `begin()` returns `void` instead of a deferred status.
  `configStatus()` and the sticky host-wide status are gone. The previous
  batch model recorded the first rejection and short-circuited every later
  add so `begin()` could report it — reasonable for a hardcoded test rig,
  but wrong once configuration moved to a runtime verb shell, and a
  prerequisite for the runtime node-table mutation D5 now specifies.
  Callers registering several nodes decide for themselves how to treat a
  partial failure; the engine keeps no residual state. Chaining is retired
  (no call site actually used it except one test). All callers migrated:
  `SimpleHost`, `XiaoHostTracer`, `cmri_tracer`, `test_host`.

### Fixed
- `XiaoHostTracer` `node add` reported `addFailed` forever after a single
  rejected add, and silently ignored every subsequent add for the life of
  the boot (issue #80). The verb inspected the sticky host-wide
  `configStatus()`, which the old batch model never cleared, and the
  engine's own short-circuit suppressed the later calls. Both are gone;
  the verb now reports the individual call's status and names the reason.

### Added
- XiaoHostTracer dual-node validation support (issue #33): generator verbs now
  accept an optional `ua` target (`configure|enable|disable ... ua <n>`),
  defaulting to UA30 when omitted for backward compatibility. Generator runtime
  state is now per-UA so `slowwalker` and `toggleoutfrominput` can run
  concurrently on multiple nodes. `toggleoutfrominput` gained explicit
  byte/bit loopback controls (`src_byte`, `src_bit`, `dst_byte`, `dst_bit`) and
  a `mode write_read` option to support loopback `write(read())` validation
  flows. Added a new non-issue-specific bench validation suite at
  `extras/bench/validation/dual_node/` with
  `gather_bench_validation.py` and `analyze_bench_validation.py` for the UA30 +
  UA31 quick validation run.
- Bench USB port discovery (issue #68): the Python bench harness no longer
  hardcodes `/dev/cu.usbmodem*` paths (they are location-derived and shuffle
  on re-enumeration). New `extras/bench/bench_ports.py` resolver keys each
  role (Host, Sniffer TX/RX, Dongle, Nodes) on the board's USB serial
  number — every Xiao ESP32-C6 exposes a unique MAC-shaped iSerialNumber —
  via `extras/bench/bench.json`, which holds explicitly named bench sets
  (`Benches` map + `Default`) so multiple benches can share one file and
  one host. The `extras/bench/bench` CLI wraps it: `list` / `check` /
  `resolve` / `import` / `export` / `seed` / `default`, with jBOM-style
  `-o console|-|file` output and `-q`/`-v` detail control. `bench import`
  enriches a human-supplied seed (flat Type/ID/USB entries) into a
  serial-keyed set, behaviorally verifying the images we own (the tracer
  answers `status`; sniffers emit image-bearing stats every 5 s); absent
  roles are carried with a null Serial and loudly reported. All gather
  scripts and `_tracer_client` resolve lazily through it; `--port`/argv
  overrides still win. 28-test functional suite
  (`extras/bench/test_bench_cli.py`) drives the real CLI against a fixture
  config plus an injected fake enumeration.

### Fixed
- Fixed a latent bug in `XiaoHostTracer` where the OLED display perpetually showed `---` and 0 for metrics because the `node` pointer was never initialized in `setup()` (issue #56).

### Added
- Host OLED diagnostics, shared across both host sketches (issue #11):
  `src/SimpleHostMetrics.h` gains a `HostStatusPanel` class that owns
  the rolling metric state (polling rate, per-node error windows) and
  formats the header and per-node row strings. Both `SimpleHost.ino`
  and `XiaoHostTracer.ino` use it — the display logic cannot drift
  between the two sketches. The header line alternates between
  cycles/sec ("XX.Xc/s") and ms/cycle ("XXXms") every 5 s, smoothed
  over a 10 s window; a stalled engine shows "---ms". Per-node rows
  show state, right-justified last-turnaround latency ("XXXms"), and a
  rolling 5-second recent-error count ("XXerr"). No new library API
  surface — all values are read from today's public `CMRIHost::statistics()`
  and `RemoteNodeHandle::statistics()`. `XiaoHostTracer` gains the
  SSD1306 OLED it previously lacked (display was #11). 11 new Unity
  tests (`tests/test_host_display_metrics.cpp`); 159 total passing.
  The per-node backoff ladder (250 ms → 32 s) is deferred to a later
  increment that needs a `RemoteNodeHandle` accessor for `pollBackoffMs_`.
- `CONTEXT.md` — project glossary defining Engine, Strategy, Shell, and
  Vocabulary precisely, so the word "engine" does not drift again. The
  key sharpening: a component that only observes or drives an engine is
  not an engine, even when it holds a reference to one.

### Changed
- `CMRITracerEngine` renamed to `TracerShell` (D1 correctness fix): the
  class was misnamed — it does not implement the image contract (not an
  Engine) and it speaks a private C&C vocabulary, not the CMRInet
  protocol (the CMRI qualifier was wrong). Renamed to `TracerShell` —
  a command-and-control shell wrapping an engine, not an engine itself.
  Mechanical rename across all three call sites (`tests/test_tracer.cpp`,
  `extras/desktop/cmri_tracer.cpp` + `Makefile`,
  `examples/XiaoHostTracer/XiaoHostTracer.ino`). Also fixes a
  pre-existing undeclared-identifier bug in `cmri_tracer.cpp` (referenced
  a `node` that was never declared).
- XiaoSniffer CDC throughput (issue #45): the sniffer was silently
  dropping ~25% of frames at high bus rates (>50 Hz) because the
  per-frame JSON line was too long for the CDC ring buffer.
  `setTxTimeoutMs(0)` made writes discard-and-return when the buffer
  was full, so bursts of frames were lost without any error signal.
  Fix: compact the frame JSON (drop the constant `image`/`version`
  fields, already in the epoch line — ~30 chars/line, 32% reduction)
  and enlarge the CDC ring buffer (`setRxBufferSize(1024)`).
  Bench-verified: capture rate improved from ~75% to 99.6% at 55 Hz.
  The OLED stays on — it was not the bottleneck.

### Fixed
- `CMRIHost` poll/transmit starvation (issue #41): `runSchedule_` could send
  a full `T` in place of a due `P` indefinitely whenever a node's outputs
  stayed dirty, so a node's own output-update cadence — or a resonance
  with another node's reply-gate timeout sharing the round-robin — could
  starve its poll/reply cycle completely (reply pair goes silent even
  though the poll pair looks healthy). Reproduced and bisected on the
  cpNode-Xiao two-board bench: confirmed independently by an RS-485
  dongle, a second passive sniffer, the Node's own TXEN LED, and an
  `onTrace` packet capture (zero `P` to the live node across the whole
  window). New `CMRIHostConfig::maxOutputPreemptMs` (default 250 ms)
  bounds how long a due poll may be deferred by a pending transmit before
  the scheduler forces it through regardless of `outputsDirty_`. That
  bound depends on the round-robin's healthy-node cycle time staying well
  under the threshold, so a companion poll-retry backoff
  (`initialPollBackoffMs`/`maxPollBackoffMs`, doubling per consecutive
  miss up to a 32 s cap, cleared immediately on any accepted reply) keeps
  a chronically offline node from taxing every pass at full priority —
  without it, the anti-starvation bound alone inverts into the
  mirror-image defect (transmit starvation). Both ship together. 3 new
  Unity tests (`test_dirty_output_cannot_starve_poll_forever`,
  `test_anti_starvation_does_not_starve_transmit`,
  `test_poll_backoff_doubles_and_clears_on_reply`); 151 total passing.
  Hardware re-verified on the same bench: `SimpleHost` (unmodified,
  previously the reliable repro) now shows `HEALTHY` with hundreds of `R`
  frames per 15 s window, confirmed A/B/A across three separate flashes.
  See `docs/sniffer-reply-pair-findings.md` for the full diagnosis record.

### Added
- `examples/SimpleHost/` — the front-door Host tutorial (issue #31):
  a behavior-only sketch a human copying this library starts from, not
  a bench instrument. Polls two nodes (UA 30 live + UA 31 phantom),
  shows per-node health on the SSD1306 OLED, and runs two behaviors: a
  1 Hz walking-one bitwalk through one output byte (visual proof the
  full T path works) and an output that toggles on each rising edge of
  a real input (the input side of the API). Table-driven node config
  (`NodeInfo` struct array), `host.node(addr)` handle lookup, and an
  `onEvent` listener that prints a rejection diagnostic over USB CDC
  ("expected N input bytes, got M") so a misconfigured geometry is
  diagnosable from the Host side without reflashing the node.
  Bench-validated on the cpNode-Xiao two-board crossover: node 30
  ONLINE, bitwalk walking, input toggle driving an output. Closes #31.
- Host public API ergonomics, surfaced and resolved by #31 (breaking):
  `addRemoteNode(...)` now returns `CMRIHost&` so calls chain
  (`host.addRemoteNode(30, 12, 12).addRemoteNode(31, 4, 4).begin()`);
  a rejected add records its reason in `ConfigStatus` and
  short-circuits later adds. `begin()` returns `ConfigStatus` (was
  `void`) — `kOk` or the reason the first add failed (`kTooManyNodes`,
  `kAddressOutOfRange`, `kAddressInUse`, `kInputBytesTooLarge`,
  `kOutputBytesTooLarge`, `kAlreadyBegun`). New flat
  `addRemoteNode(address, inputBytes, outputBytes)` overload for
  readable simple sketches. New `node(address)` accessor returns a
  registered `RemoteNodeHandle*` by address (the sketch no longer
  holds a parallel handle array). New `configStatus()` reads the
  config-phase status before or after `begin()`. All callers migrated:
  `XiaoHostTracer`, `cmri_tracer`, `test_host`, `test_tracer`.
  145 tests still pass.
- Reply-rejection diagnostics on `CMRIHostEvent` (issue #31): the
  `kReplyRejected` event now carries a `ReplyRejectReason`
  (`kUaMismatch`/`kMtMismatch`/`kGeometryMismatch`), the rejected
  reply's body `length`, `ua`, and `mt`. A conformance-checking
  sketch can now print what the remote node actually sent ("expected 7
  input bytes, got 4") without reflashing the node. The drain path
  splits UA and MT mismatch into separate reject sites so each reason
  is reported. `replyRejectReasonString()` stringifier (in-namespace,
  placeholder for a future class member).
- `configStatusString()` / `replyRejectReasonString()` —
  human-readable names for `ConfigStatus` and `ReplyRejectReason`
  (in-namespace, placeholders for future class members).
- README refreshed: new "Architecture at a glance" TL;DR condensing
  DESIGN's one-product/two-seams model, units ladder, and naming
  grammar (pointer-style to D1-D13); new "Getting started" keyed off
  `examples/SimpleHost`; stale "Planned layout" replaced with the
  real repository layout. Host-focused now; extends to both roles when
  #9 lands.
- Interop profile open questions 7 and 8 (issue #31): OQ7 — define an
  I/T acknowledgment carrying the Node's self-description so a Host
  can validate configured geometry at init time (extends the E8
  revision thread; motivated by #31's geometry-mismatch finding);
  OQ8 — bus discovery via a self-identify MT so a Host can
  auto-configure its node table. Filed as #34 and #35.
- Follow-up tickets filed from the #31 ergonomics probe: #33 (second
  physical bench node), #36 (RemoteNodeHandle card-type / IOX-offset
  awareness — the phantom-byte trap), #37 (input edge detection /
  change event), #38 (per-node listener registration or node tag).
- I/T bench slice (issue #30): the shared `CMRITracerEngine` now
  registers both D7 listeners — `onEvent` (exchange/health) and
  `onTrace` (per-packet TX/RX) — and emits a `trace` JSON line per
  packet `{dir:"tx"|"rx",mt,ua,body}`, so I and T appear in telemetry
  with their MT and body alongside the counters. New output verbs make
  T bench-exercisable: `setbit <n> <0|1>`, `writeoutputs <hex>`,
  `forcetx` (bad args emit an `error` line, not a crash). Every
  telemetry line now carries the output image as `outputs` hex
  alongside `inputs`. Desktop `cmri_tracer` gains `--output-bytes`
  (default 7); the Xiao Host sketch gains `TRACER_OUTPUT_BYTES`
  (default 7). Versions bumped: cmri_tracer 0.3.0, XiaoHostTracer 0.2.0.
  13 new Unity tests (`tests/test_tracer.cpp`); 145 total passing.
- `examples/XiaoSniffer/` — passive RS-485 bus sniffer for the testbed
  (issue #30): a spare cpNode-Xiao wires R± to one bus pair, holds TXEN
  low (listen-only), and feeds the standalone `CMRIFrameDecoder` from
  `Serial1`, emitting each decoded frame as a JSON line over USB CDC.
  No Host/transport/TX — the minimal independent witness for I/T/P
  tap acceptance, and a reusable testbed asset the software notes point
  at for the adversarial and host-conformance use cases. Direction-blind
  (reports `"observed"`); one board = one pair.
- Phase 2 `CMRIHost` — I/T sending, full-image output, and re-init
  ladder (issue #8): the engine now speaks all four polled-strategy packet
  types. Per-node on-the-wire order is I → T → P (interop 2.3.1): a
  node's first exchange is an I (CPNODE 'C' dialect, 13-byte body
  `<'C'> <dH> <dL> <opts1> <opts2> <NI> <NO> <0xFF×6>`, interop
  E3), followed by a full-image T after a 500 ms settle. T is sent on
  output change (`setOutputBit`/`setOutputs`/`forceTransmit` mark the
  image dirty) and always carries the full output image (interop 2.3.2).
  After more than 5 consecutive P misses the re-init ladder arms: the
  next slot re-sends I + full T and invalidates cached input state by
  clearing freshness only — the last-good bytes are kept, since 0 is a
  valid consumer value and zeroing would assert "all clear" (the QBASIC
  review's F15 hazard); a recovered reply disarms the ladder. dH/dL are
  per-node knobs in `RemoteNodePolicy`, default 0 (erratum E4). I and T
  expect no reply (interop E8): a packet arriving during the I settle or T
  gap is counted in `unsolicitedPackets` and discarded, not rejected or
  errored. New `CMRIHostConfig` knobs: `postInitSettleMs` (500),
  `postTxGapMs` (2), `transmitRefreshMs` (0 = off). New
  `CMRIHostEventType::kReinitScheduled` event. New `RemoteNodeHandle`
  output API: `outputBit`/`outputByte`/`outputLength`,
  `setOutputBit`/`setOutputs`/`forceTransmit`. New
  `CMRINET_HOST_MAX_OUTPUT_BYTES` geometry knob. 17 new Unity tests
  (`tests/test_host.cpp`); 132 total passing.
- `Esp32UartCMRISerialPort` promoted from the Xiao tracer sketch
  into `src/` (issue #27): hardware transmit-drain truth
  (`uart_wait_tx_done(port, 0)`) is the correct shipped behavior for
  every ESP32 target, not an R&D-only fix. Header-only,
  `ARDUINO_ARCH_ESP32`-guarded (Design v1.1 D7: a platform guard on a
  platform-specific port is not a feature toggle; non-matching builds
  see an empty file). Promotion is a relocation — the behavior is
  unchanged from the #21 wire-tap bench pass. The sketch-local copy in
  `examples/XiaoHostTracer/` is removed; the sketch's `#include` now
  resolves to the library header. Closes the sketch-local R&D fix from
  #21; #25 stays open as root cause (with #28 as its sibling).
  Promotion re-verified on the two-board crossover bench: the 0.1.3
  image ran ~4100 exchanges with all counters zero (misses/errors/
  decodeErrors/recoveries all 0, ONLINE throughout) across ~75 s, and
  the passive poll-pair tap witnessed ~4100/4100 clean `ff ff 02 5f 50
  03` poll frames — zero clipped ETX tails (the #21 `… 5f 50 ff`
  signature absent). Captures in
  `docs/bench/2026-08-16-stage2-27-promotion-host.jsonl` and
  `…-tap.hex`.
- Inter-byte abort doctrine (issue #27, Design v1.1 D13):
  `SerialCMRITransport` ships a tolerant deployment default
  (`kShippedInterByteTimeoutMs`, 100 ms) instead of the rate-derived
  three-character-time value. The reference Host lineage transmitted
  with interpreter-scale gaps (interop 2.2.6) and fielded Nodes pace
  with dH/dL (erratum E4), so a strict shipped default would fail
  conforming history; the 250 ms reply gate is the truncation
  backstop. The rate-derived value is now an explicit conformance
  instrument via `rateDerivedInterByteTimeoutMs()` (valid before
  `begin()`), never a default. Generalizes the stage-1 USB-chunking
  and stage-2 C6-stall findings: the decoder measures gaps at tick
  granularity, so any host whose tick can stall measures arrival gaps,
  not wire gaps. Be strict in what you send, forgiving in what you
  accept.
- `SerialCMRITransport::rateDerivedInterByteTimeoutMs()` — the
  conformance-strict inter-byte abort derived from the port's
  character time (three char times, interop 2.2.6). An explicit opt-in
  for tracers and conformance scenarios (Design v1.1 D13).
- `SerialCMRITransport::kShippedInterByteTimeoutMs` — the tolerant
  shipped/deployment inter-byte abort default (100 ms).
- `cmri_tracer --conformance-strict` — opt into the rate-derived
  inter-byte abort and the rate-derived slow-gap thresholds (interop
  2.2.6 conformance instrument); the USB-deployment defaults (25 ms
  abort, 1/20 ms slow-gap) remain the normal bench mode.
- `transmitDrained()` seam contract documented on `CMRISerialPort`:
  the answer may be optimistic by ignorance (a buffer-only port is the
  permitted floor) but must never be optimistic by design — never
  `true` while the port's hardware still holds untransmitted bits it
  can see. `StreamCMRISerialPort` reframed as the portable floor with
  a contribution pattern for other cores (AVR `TXCn` bit; ESP32 worked
  example in `Esp32UartCMRISerialPort`). The transport keeps the
  estimate AND port-drain conjunction even for hardware-truth ports.
- DESIGN.md bumped to v1.1: D7 platform-guard clarification,
  transport-contract estimated-drain footnote, new D13 inter-byte
  abort doctrine. Existing `Design v1.0` VALIDATION tags re-stamped to
  v1.1 (decisions D1-D12 unchanged).
- Two-threshold receive-gap observability (issue #26): the frame
  decoder now distinguishes "took longer than expected" from "took so
  long I gave up." `CMRIFrameDecoder::Statistics` carries a cumulative
  `slowGaps` counter (gaps in the suspect band [nominalHi, abort)) and a
  `maxGapMs` watermark (the largest inter-byte gap seen mid-frame,
  including the fatal abort-gap). `setSlowGapThresholdsMs(lo, hi)` sets
  the band; lo=0 disables observability, hi<=lo leaves watermark-only.
  `SerialCMRITransport` derives lo (1 char time) and hi (3 char times)
  from the port character time; the override survives `begin()`. The
  desktop tracer emits `slowGaps`/`maxGapMs` on every JSON line and the
  three thresholds on the epoch line (`--slow-gap-lo-ms`,
  `--slow-gap-hi-ms`, USB defaults 1/20). Interop profile 2.2.6 revised
  to the grace-band receive model (profile v1.1; `VALIDATION` tags
  re-stamped). This would have located the #21 2 s stall from telemetry
  alone. 12 new tests (9 codec, 3 transport); 118 total passing.
- Stage-2 bench validation (issue #21): the stage-1 scenario passed
  unchanged against the Xiao ESP32-C6 Host on the two-board crossover
  bench — smoke (UNINITIALIZED→ONLINE first reply), sustained
  2459/2459 exchanges with all counters zero (turnaround p50 7 /
  max 36 ms on the real UART), wrong-UA, unplug (OFFLINE at 6
  consecutive misses, polling continues), automatic recovery, and the
  quiesce/resume/status/quit verbs. Passive wire tap witnessed
  8124/8124 clean polls and 1499/1499 clean node replies. JSONL
  telemetry and tap captures in `docs/bench/`.
- `src/testbed/CMRITracerEngine.h` — the shared testbed C&C engine
  (verbs in, JSON-lines telemetry out, D7 listeners), wrapped by both
  the desktop `cmri_tracer` and the Xiao Host sketch: "same engine,
  same listeners, different main()" is now enforced by construction.
  `ts` is integer ms since the stream's epoch line in every image;
  the epoch line carries the absolute anchor (`wallClock` on the
  desktop, `bootMs` on the Xiao) (issue #21).
- `examples/XiaoHostTracer/` — the stage-2 Xiao Host R&D image:
  CMRIHost on the cpNode-Xiao RS-485 block (28800 8N2, RX=D7, TX=D6,
  TXEN=D3), C&C over USB CDC, no OLED/OTA/WiFi. Build knobs
  `TRACER_ADDRESS` / `TRACER_INPUT_BYTES` / `TRACER_BAUD` /
  `TRACER_INTER_BYTE_TIMEOUT_MS` via `build.defines` (issue #21).
- `Esp32UartCMRISerialPort` (sketch-local): transmitDrained() from
  `uart_wait_tx_done(port, 0)` — hardware TX-complete truth instead
  of the buffer-level answer plus wire-time estimate (issue #21).
- Stage-1 bench validation (issue #7): the P/R tracer bullet ran on a
  real wire — Mac desktop Host through a USB-RS485 adapter to the
  flashed cpNode-Xiao (UA 95, 28800 8N2). Sustained run 1000/1000
  exchanges with 0 misses and 0 decode errors (turnaround p50 23 /
  max 41 ms); wrong-UA, unplug (OFFLINE, polling continues), and
  reconnect (automatic recovery) negative tests all passed. JSONL
  telemetry captures in `docs/bench/`.
- `CMRIHost::onEvent()` / `onTrace()` (D7 listener seam) —
  function-pointer listeners with a context cookie, locked at
  `begin()`: engine events (reply accepted/rejected, miss, node state
  change with old/new state) and TX/RX packet traces. 6 new tests
  (issue #7).
- Desktop Host harness (`extras/desktop/`, outside the Arduino
  build): `PosixCMRISerialPort` — the byte-port seam over fully raw
  termios (no IXON/IXOFF, per review-CMRI-Controller-host.md Finding
  7), non-blocking, 8N2, macOS `IOSSIOSPEED` for the nonstandard
  fielded 28800 rate — and `cmri_tracer`, a command-and-control CLI
  around `CMRIHost`: verbs (`quiesce`/`resume`/`status`/`quit`) on
  stdin, JSON-lines telemetry (monotonic seq, epoch marker,
  cumulative counters) on stdout (issue #7).
- Bench finding: USB serial adapters deliver RX bytes in
  latency-timer chunks (FTDI default 16 ms), so arrival gaps are not
  wire gaps; the rate-derived inter-byte timeout (~2 ms at 28800)
  aborts healthy frames. The tracer defaults the override to 25 ms
  (`--inter-byte-timeout-ms`).
- `CMRInet::CMRIHost` (`src/CMRIHost.h/.cpp`) — the polled-strategy
  Host engine, P/R slice: non-blocking `tick(nowMs)` poll schedule,
  round-robin over enabled nodes, reply gate opened at `sendComplete()`
  (default 250 ms, per-node override via nested `RemoteNodePolicy`),
  UA/MT reply verification, geometry-checked commit, miss counting
  with recovery and OFFLINE detection, poll pacing, and cumulative
  `CMRIHostStatistics` (issue #6).
- `CMRInet::RemoteNodeHandle`, `RemoteNodeConfig`, `RemoteNodeState`,
  `RemoteNodeStatistics` (`src/RemoteNodeHandle.h`) — the
  strategy-neutral per-node product surface: input image reads,
  freshness age, health state, cumulative statistics, and
  disable-only lifecycle. New `CMRINET_HOST_MAX_INPUT_BYTES` and
  `CMRINET_HOST_MAX_NODES` geometry knobs.
- 20 new Unity tests (`tests/test_host.cpp`) against the mock
  transport and scripted replay rig, covering poll emission, reply
  gating and overrides, reply verification, miss/recovery/offline
  and staleness transitions, pacing, backpressure retry, and
  byte-level gapped replay through the real decoder (issue #6).
- `CMRInet::SerialCMRITransport` (`src/SerialCMRITransport.h/.cpp`) —
  the serial/RS-485 packet transport: codec integration, non-blocking
  TXEN discipline (assert, gapless write, flush to full drain via
  wire-time estimate AND port drain report, deassert at once),
  rate-derived default inter-byte timeout (overridable, 0 = tolerate
  any gap), and decoder + UART error folding into `stats()`
  (issue #5).
- `CMRInet::CMRISerialPort` (`src/CMRISerialPort.h`) — the byte-port
  seam under the serial transport (raw bytes, TXEN line, drain query,
  character duration, hardware error count), so desktop tests drive
  the exact TXEN/drain/timeout discipline that runs on hardware.
- `CMRInet::StreamCMRISerialPort` (`src/StreamCMRISerialPort.h`,
  Arduino-only) — adapter over an Arduino `Stream` plus optional TXEN
  pin; stop-bit hook is the sketch's `Serial.begin(baud, SERIAL_8N2)`
  plus the adapter's bits-per-character (default 8N2).
- 14 new Unity tests (`tests/test_serial_transport.cpp`) against a
  scriptable fake port, covering TXEN ordering, `sendComplete`
  semantics, chunked writes, receive-during-drain, and stats.
- `CMRInet::CMRITransport` (`src/CMRITransport.h`) — the packet-seam
  transport contract with `LinkStatistics`, per the DESIGN.md transport
  contract (issue #4).
- `CMRInet::MockCMRITransport` — deterministic mock transport and
  scripted replay rig: packet- and byte-level injection (bytes run
  through the real decoder, optionally gap-metered per byte), send
  latency/backpressure/link-down shaping, sent-packet observation, and
  an ordered (UA, MT)-matched reply script (packet, bytes, or silence;
  repeat counts). Fixed-capacity storage, no allocation (issue #4).
- `CMRInet::Deadline` and `CMRInet::Age` (`src/CMRITime.h`) —
  injected-time helpers adapted from the elapsedMillis idea:
  centralized wrap-safe clock math with explicit armed/marked state,
  fail-safe sticky-due latching, and saturating ages with named
  `kNeverMarked` / `kExceededCapacity` sentinels.
- `// VALIDATION:` comment convention
  (`docs/agents/validation-comments.md`): greppable, versioned
  provenance tags from code to Spec / Interop / Design clauses;
  `Version:` lines added to DESIGN.md and the interop profile.
- 37 new Unity tests (`tests/test_time.cpp`,
  `tests/test_mock_transport.cpp`); the tests Makefile now builds
  multiple suites.
- `CMRInet::CMRIPacket` — the direction-neutral packet value type
  `{UA, MT, body}` with a `CMRINET_MAX_BODY` geometry knob (issue #3).
- Serial codec (`src/CMRIFrameCodec.h/.cpp`): `encodeFrame()` and the
  resumable non-blocking `CMRIFrameDecoder`, implementing the framing and
  DLE-escaping rules of the interop profile Part 2 (two-SYN preamble,
  escape 0x02/0x03/0x10 in every body including I, never 0xFF, DLE-aware
  hunt, inter-byte timeout, commit-on-ETX staging).
- Desktop test harness: vendored Unity v2.6.1 (no Ceedling) with a plain
  Makefile (`make -C tests`); 29 byte-vector tests seeded from the
  `docs/research/comparison.md` §3 anti-checklist.

### Fixed
- `examples/XiaoSniffer/` no longer stalls when a USB cable is plugged
  in but no terminal is open (DTR not asserted): `Serial.setTxTimeoutMs(0)`
  in `setup()` makes CDC writes discard-and-return when no host is
  draining the ring buffer, instead of blocking ~100 ms per write
  (Espressif's documented HWCDC workaround, arduino-esp32 #9043; safe on
  the C6's HWCDC path). At many frames/s that per-write stall was
  starving `loop()` and freezing the OLED.
- `examples/XiaoSniffer/` no longer hangs on a headless board: the
  USB CDC host wait in `setup()` is bounded at 3 s, and `emitLine()`
  skips the blocking `Serial.write` when no terminal is attached
  (live check; a later-attaching terminal resumes output). The OLED
  dashboard also no longer garbles once per-MT counters grow: the
  single `I:%u T:%u P:%u R:%u` line exceeded the 21-char width and
  wrapped onto the row below, so `drawStatus()` is reflowed to a fixed
  5-line layout (frames / P+R / T+I / abort+rst / last ua), every line
  <= 21 chars, with `setTextWrap(false)` as a belt-and-suspenders clip
  guard. Version bumped 0.2.0 -> 0.2.1.
- `CMRIHost.h` now includes `Arduino.h` for the ARDUINO-only
  `tick()` convenience overload — the library's first real Arduino
  compile could not see `millis()` (issue #21).
- TXEN clipped the poll's ETX mid-air every ~2 s on the Xiao Host
  (wire-tap verified `… 5f 50 ff` tails): the ESP32-C6 Arduino
  runtime stalls ~25–35 ms about every 2 s, the drain estimate
  expired during the stall, and TXEN dropped while the UART was
  still shifting. Fixed by hardware drain truth
  (`Esp32UartCMRISerialPort`); receiver-dependent symptom — the
  FTDI tap decoded some clipped tails clean while the node's
  MAX3491 saw garbage (issue #21).

### Changed
- `examples/XiaoSniffer/` OLED counters now print in fixed-width,
  right-aligned fields (`%10u`/`%8u`/`%4u`/`%3u`) so the labels and
  the second counter on each line stay pinned as digit counts change —
  no layout shift when a value grows by a digit. Pure UX polish on the
  0.2.1 fixed 5-line dashboard; no version bump.
- `examples/XiaoSniffer/` OLED right-column counters (R, I, rst) now
  share the same right margin so their digits line up vertically: I
  widened to `%8u` and rst to `%6u`, both ending at column 21 like R.
  Left-column counters (P, T, abort) keep column 10.
- Xiao Host R&D image version bumped to 0.1.3 (issue #27): the
  `Esp32UartCMRISerialPort` include now resolves to the library header
  and the port usage is namespace-qualified (`CMRInet::`). The image
  keeps its explicit 50 ms inter-byte override, so runtime behavior is
  unchanged from 0.1.2.
- Inter-byte timeout doctrine, generalizing the stage-1 USB-chunking
  finding: the decoder measures byte gaps at tick granularity, so
  ANY host whose tick can stall (USB latency timers, runtime
  housekeeping) measures arrival gaps, not wire gaps. The rate-derived
  default is a conformance instrument; deployed Hosts should run a
  tolerant limit (Xiao image: 50 ms) and rely on the 250 ms reply
  gate as the truncation guard. Be strict in what you send,
  forgiving in what you accept (issue #21).
