# Changelog

High-level changes, newest first.

## Unreleased

### Added
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
