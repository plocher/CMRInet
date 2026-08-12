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

## Goal
A bench "JMRI Master" instrument: a Xiao-based board (no IOX expanders;
SSD1306 OLED + RS485 CMRINet + WiFi OTA) that emulates the JMRI side of
CMRInet against **one hardcoded slave node**. It sends Init / Transmit /
Poll messages, parses the Read responses coming back, walks the slave's
outputs through wrap-around / cylon patterns, and reports link health on
the OLED. Together with the `CMRINet-examples/Xiao_I2C` node sketch it
forms a complete two-board test bench: master drives, node responds,
jumpers loop outputs back to inputs.

## Why a separate library (not a cpNode refactor)
The existing `cpNode` library is slave-focused: its state machine waits
for packets and reacts. A master inverts control — it owns the schedule,
transmits I/T/P, and waits (with timeouts) for R. The reusable parts of
`cpNode` (frame parsing, DLE handling, TXEN discipline) are private to a
class built around slave assumptions. Rather than refactor a deployed,
working library, this repo deliberately duplicates the ~150 lines of
framing knowledge and builds the master engine fresh. Once the tracer
bullet has traction, merging the two libraries around a shared framing
core is a separate, later discussion (Phase 3).

## Repo layout
- `src/CMRIMaster.h` / `src/CMRIMaster.cpp` — the master protocol engine
  (library proper; no display, no WiFi, no pattern logic)
- `examples/CMRI-Master/CMRI-Master.ino` — the emulator sketch
- `examples/CMRI-Master/display.{h,cpp}`, `ota.{h,cpp}`,
  `secrets.h.example` — sketch-local copies harvested from
  `CMRINet-examples/Xiao_I2C`, free to diverge (display gains
  master-specific status; ota drops in unchanged)

## Protocol notes (master side)
- Frame: `SYN SYN STX <UA+65> <type> <payload...> ETX`, with DLE
  escaping of payload bytes that collide with control characters
- Master sends: `I` (init: NDP 'C', DLH/DLL transmit delay, options,
  byte counts), `T` (output bytes), `P` (poll request)
- Master receives: `R` (input bytes) from the addressed node
- RS485 is half duplex: assert TXEN only while transmitting and drop it
  immediately after the last byte, or the node's reply gets clobbered.
  At 28800 8N2 one char is ~0.38 ms; a 6-byte reply is ~3 ms
- The receive parser mirrors cpNode's `getPacket()` state machine
  (SYN SYN STX addr type data ETX, DLE unescape, buffer-overrun guard)

## Core state machine (non-blocking, advanced from loop())
`IDLE → SEND_INIT → [SEND_T → SEND_P → AWAIT_R]…` repeating, where:
- `AWAIT_R` has a response timeout (start ~100 ms, tune down); timeout
  increments a miss counter, a good reply clears it and stores IB
- N consecutive misses (start N=5) → `NO_RESPONSE`: back off and
  re-send `I` periodically until the node answers again (this makes a
  power-cycled node recover without restarting the master)
- Counters: inits sent, polls sent, replies OK, misses, framing errors
- Everything non-blocking so the OLED refresh and OTA polling stay
  live, same discipline as the node sketch

## Example sketch configuration (mirrors Xiao_I2C conventions)
- `#define` toggles: `USE_OLED`, `USE_OTA`
- `NODE_NAME` default `"CMRI-Master"` — the master is not a node, so no
  node-number suffix; used for the OLED header and OTA mDNS hostname
- `SLAVE_UA` (30), `SLAVE_INPUT_BYTES` / `SLAVE_OUTPUT_BYTES` (4/4 to
  match the Xiao_I2C example node), `CMRINET_SPEED` (28800)
- `POLL_PERIOD_MS`, `PATTERN_STEP_MS`, pattern select (wrap / cylon)
- `secrets.h` (gitignored) + committed `secrets.h.example` for WiFi

## Display semantics (master variant)
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

## Validation
- Bench: this master wired to the existing Xiao_I2C node (UA 30, 4/4)
- Pass: node's spinners spin; master's miss count stays 0; looped-back
  IB tracks the driven pattern (inverted per active-low convention)
- Negative tests: unplug the node → `NO-RESPONSE` + periodic re-init;
  power-cycle the node → automatic recovery; wrong `SLAVE_UA` → clean
  timeout behavior, no lockups
- Compile matrix: `USE_OLED` × `USE_OTA` combos for
  `esp32:esp32:XIAO_ESP32C6`

## Phases
1. **Tracer bullet** — `CMRIMaster` sends P and parses R only; OLED
   shows hit/miss counters and spinners. Proves framing, TXEN
   turnaround, and timeout core against real hardware.
   Acceptance: sustained polling of the bench node with zero
   unexplained misses.
2. **Full emulator** — add T + pattern generator, I on startup and the
   re-init/backoff policy, full link-state display, counters.
3. **Merge discussion** — with the master engine proven, evaluate
   extracting a shared framing core with the `cpNode` slave library
   (or folding master support into it). Deferred by design.

## Non-goals (for now)
- Polling multiple nodes (single hardcoded slave only)
- Full JMRI configurability (card types beyond CPNODE assumptions,
  SUSIC/SMINI, per-node option semantics)
- Protocol stress/fuzz testing of the node
