# CMRInet — CMRINet Master (JMRI Emulator) Plan

> **Note (2026-08-12):** the architecture sections below ("Why a
> separate library", "Protocol notes", "Core state machine") are
> superseded by [docs/DESIGN.md](docs/DESIGN.md), which records the
> agreed layer model, decisions D1-D12, and the transport contract.
> Wire behavior follows
> [docs/cmrinet-interop-profile-and-errata.md](docs/cmrinet-interop-profile-and-errata.md).
> The goal, display semantics, validation, and phasing here still
> stand, reinterpreted through DESIGN.md (see its revised Phase 1
> scope).

## Repo layout
- `src/CMRIMaster.h` / `src/CMRIMaster.cpp` — the master protocol engine
  (library proper; no display, no WiFi, no pattern logic)
- `examples/CMRI-Master/CMRI-Master.ino` — the emulator sketch
- `examples/CMRI-Master/display.{h,cpp}`, `ota.{h,cpp}`,
  `secrets.h.example` — sketch-local copies harvested from
  `CMRINet-examples/Xiao_I2C`, free to diverge (display gains
  master-specific status; ota drops in unchanged)

## Example sketch configuration (mirrors Xiao_I2C conventions)
- `#define` toggles: `USE_OLED`, `USE_OTA`
- `NODE_NAME` default `"CMRI-Master"` — the master is not a node, so no
  node-number suffix; used for the OLED header and OTA mDNS hostname
- `SLAVE_UA` (30), `SLAVE_INPUT_BYTES` / `SLAVE_OUTPUT_BYTES` (4/4 to
  match the Xiao_I2C example node), `CMRINET_SPEED` (28800)
- `POLL_PERIOD_MS`, `PATTERN_STEP_MS`, pattern select (wrap / cylon)
- `secrets.h` (gitignored) + committed `secrets.h.example` for WiFi


## Display semantics 
The chosen 'duino board (cpNode-Xiao) has an onboard OLED display that is
suitasble for runtime diagnostics and status reporting.  The following 
provides a general UX pattern to follow:
- Header: `NODE_NAME` + quantized spinners — `t` steps while Transmits
  flow, `r` steps while Read replies arrive; frozen `r` = node silent
- Grid: rows showing OB (the pattern being driven) and IB (what the
  node reported), with the same bit cells and change halos as the node
  sketch. With the node's B→A jumpers, IB mirrors the pattern through
  the active-low inversion (out 0 ⇒ in 1) — a built-in self test
- Status: link state (`INIT / POLLING / NO-RESPONSE`) + miss count,
  alongside the WiFi/OTA status from the node sketch
- OTA takeover screens (progress bar / success / failure hold)
  unchanged from the node sketch

## Initial implementation Phases
1. DONE: **Tracer bullet** — `CMRIMaster` sends P and parses R only; OLED
   shows hit/miss counters and spinners. Proves framing, TXEN
   turnaround, and timeout core against real hardware.
   Acceptance: sustained polling of the bench node with zero
   unexplained misses.
2. DONE: **Full emulator** — add T + pattern generator, I on startup and the
   re-init/backoff policy, full link-state display, counters.
3. DONE: **Merge discussion** — with the master engine proven, evaluate
   extracting a shared framing core with the `cpNode` slave library
   (or folding master support into it). Deferred by design.
4. DONE: Validation
   - Bench: The desktop Host application wired by way of an USB/RS422
     dongle to an Xiao_I2C node (UA 30, 4/4) running the cpNode repo's Xiao_I2C sketch
   - Pass: node's spinners spin; master's miss count stays 0; looped-back 
     IB tracks the driven pattern (inverted per active-low convention)
   - Negative tests: unplug the node → `NO-RESPONSE` + periodic re-init;
     power-cycle the node → automatic recovery; wrong `SLAVE_UA` → clean
     timeout behavior, no lockups
   - Compile matrix: `USE_OLED` × `USE_OTA` combos for
     `esp32:esp32:XIAO_ESP32C6`

## Non-goals (for bootstrap phase)
- Polling multiple nodes (single hardcoded slave only)
- Full JMRI configurability (card types beyond CPNODE assumptions,
  SUSIC/SMINI, per-node option semantics)
- Protocol stress/fuzz testing of the node

## Scope for the tracer bullet (Phase 1, revised)

In: codec, `SerialCMRITransport`, `MockCMRITransport`, CMRIHost
(I/T/P), `RemoteNodeHandle` (inputs, outputs, freshness, state, re-init
ladder), scripted-replay tests, OLED hit/miss display per PLAN.md.
Out (sequenced, not abandoned): TCP carrier transport (JMRI
`networkdriver` interop), warty CMRINode profiles (fidelity 3),
SUSIC/SMINI node types (bench roadmap). MQTT carrier and semantic
gateway belong to a sibling library (ADR-0004).

Open items to settle during tracer-bullet implementation:
1. Default output semantics: T-on-change (JMRI) per D9. Confirm on
   the bench. `forceTransmit()` exists either way. Bench note (Node
   M1–M5 lock-in): dense full-T (e.g. sub-second dirty bitwalk) is a
   known trigger for elevated Host `noReplies` while steady P/R
   turnaround stays ~6–7 ms and wire-level R usually still exists.
   Mechanism open — Host post-T / RX / reply-gate path, not Node pack
   I2C cost. Tracked as a follow-up issue; do not treat SimpleHost's
   30 s demo bitwalk as the product T policy.
2. Per-node input-change callback, or polled-only handle. Start
   polled-only. Add the callback only if diff-scanning hurts.
3. Counter granularity for conformance faults (D14). Deferred
   deliberately: events carry layer, attribution, and expected/actual,
   so the bench analyzer aggregates externally and real failure
   distributions decide which cuts earn a durable counter. Until then
   the residue is one total plus last-fault detail.
4. Warty-Node trait vocabulary (D3 fidelity 3). Traits are
   individually toggleable rather than a mode enum, because isolating
   one defect at a time is the point, and because the verb-based C&C
   regime drives them — so trait identifiers are part of the C&C
   vocabulary, mapped in one place rather than scattered comparisons.
   `ignore-init` and `tolerant-geometry` join the traits drawn from the
   research reviews. The strict Node (fidelity 2) is the default: a
   forgiving counterparty absorbs Host bugs and defeats the test rig.
