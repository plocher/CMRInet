# cpNode-Master

CMRINet **master-side** (poller) protocol engine and bench "JMRI Master"
emulator — the testing counterpart to the slave-focused
[`cpNode`](../cpNode) library.

Status: **planning / tracer bullet** — see [PLAN.md](PLAN.md).
Agents picking up this work: start with [HANDOFF.md](HANDOFF.md).

- `src/` — `CMRIMaster` protocol engine (Init / Transmit / Poll out,
  Read responses in, timeouts and re-init policy)
- `examples/CMRI-Master/` — Xiao ESP32-C6 emulator sketch: drives one
  hardcoded slave node with wrap/cylon output patterns, OLED link/bit
  display, WiFi OTA

The master deliberately duplicates a small amount of CMRINet framing
knowledge from `cpNode` rather than refactoring that deployed library;
merging the two around a shared core is deferred until this work has
traction (PLAN.md, Phase 3).
