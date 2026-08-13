# Changelog

High-level changes, newest first.

## Unreleased

### Added
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
