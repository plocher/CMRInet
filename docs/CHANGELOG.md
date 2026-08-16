# Changelog

High-level changes, newest first.

## Unreleased

### Added
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
