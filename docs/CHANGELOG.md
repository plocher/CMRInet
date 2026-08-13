# Changelog

High-level changes, newest first.

## Unreleased

### Added
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
