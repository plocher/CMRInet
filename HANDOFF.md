# Handoff — cpNode-Master Phase 1 (tracer bullet)

Audience: a fresh agent starting implementation of this library.
Scope, architecture, state machine, validation, and phasing are in
[PLAN.md](PLAN.md) — read it first; this file only adds context that
lives outside this repo or was learned the hard way.

## Where things are
- This repo: `~/Dropbox/Arduino/libraries/cpNode-Master` (you are here).
  `src/` and `examples/` are empty and waiting for Phase 1.
- Slave library (protocol reference): `~/Dropbox/Arduino/libraries/cpNode/src/`
  - `cpNode.cpp` top-of-file comments document the CMRINet message
    formats (I/P/R/T frames)
  - `getPacket()` (~lines 300-466) is the receive state machine to
    mirror: SYN SYN STX addr type data ETX, DLE unescape, overrun guard
  - `IOX::init()` (~line 468) shows the MCP23017 active-low input
    convention (IPOL register) — why loopback bits read inverted
  - Packet-layer inversion asymmetry worth knowing: `invert_in` is
    applied AFTER the sketch's `pack()` runs; `invert_out` BEFORE
    `unpack()` (cpNode.cpp ~127, ~144)
- Donor sketch (harvest, don't reinvent): `~/Dropbox/Arduino/CMRINet-examples/Xiao_I2C/`
  - `display.{h,cpp}` — NodeDisplay: dirty-flag rendering, quantized
    spinners, per-bit change halos, OTA screen-ownership latch,
    headless degradation. Copy into `examples/CMRI-Master/` and adapt
    (grid becomes OB-driven/IB-returned rows; status adds link state)
  - `ota.{h,cpp}` — OtaManager: non-blocking WiFi + ArduinoOTA with
    function-pointer hooks. Drops in UNCHANGED.
  - `secrets.h.example` — copy; real `secrets.h` is gitignored here
  - `Xiao_I2C.ino` — the sketch-structure conventions to mirror
    (toggles at top, NODE_NAME via STRINGIFY, frontmatter style)

## Build & validation environment
- No standalone arduino-cli. Use the IDE-bundled one:
  `/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli`
- FQBN: `esp32:esp32:XIAO_ESP32C6` (esp32 core 3.3.10 installed)
- Always pass `--libraries "$HOME/Dropbox/Arduino/libraries"`
- Habit from the node sketch: compile all four USE_OLED x USE_OTA
  combos (sed-toggled copies under /tmp); headless-OTA is the combo
  that must never break
- Bench validation target: the Xiao_I2C node board (UA 30, 4 in/4 out,
  port B outputs jumpered to port A inputs)

## Hard-won lessons (do not rediscover these)
- `WiFi.setSleep(false)` is REQUIRED on ESP32-C6 for reliable OTA —
  modem power-save stalls the espota TCP phase (invitation succeeds,
  transfer times out before onStart). Already in the donor `ota.cpp`.
- The first boot after a USB flash can have flaky WiFi/OTA state;
  power-cycle the board before blaming new code.
- A full SSD1306 frame push costs ~25 ms of I2C at 400 kHz. Do not
  refresh faster than ~100 ms; only push when a dirty flag says
  something changed. In OTA progress callbacks, repaint only when the
  integer percent changes.
- Spinners must advance at most one step per refresh (quantized) —
  indexing a glyph by a raw packet counter aliases into jitter.
- RS485 TXEN turnaround is the classic master-side bug: drop TXEN
  immediately after the last transmitted byte or the node's R reply
  gets clobbered. The slave lib's poll-response transmit path shows
  the working pattern.
- `Serial` belongs to the cpNode Monitor debug channel exclusively;
  sketches must not `Serial.print` (owner keeps `setDebugPort`
  commented out unless protocol-debugging).

## Owner's working style (matters for review)
- Discuss design before writing code; present options with a
  recommendation and let him pick. Small iterations with visible diffs.
- DRY is enforced: single-source #defines (see NODE_NAME/STRINGIFY in
  the donor sketch), named constants over magic numbers (DISP_ROWS
  pattern), consistent `#ifndef` style.
- Feature toggles live in the SKETCH, not in library headers; modules
  carry no #ifdefs — unreferenced code is dropped by the linker.
- Modules stay domain-pure: display knows nothing of CMRI/WiFi; ota
  knows nothing of rendering; the sketch wires them with capture-less
  lambda hooks.
- Follow his repo rules: feature branches, conventional semantic
  commits, tests/verification before commit, commit only when asked.

## Suggested skills
- `diagnose` — for bench bring-up problems (framing, TXEN timing)
- `zoom-out` — if the cpNode library internals get confusing
- `handoff` — regenerate this document at the end of your session

## First moves for Phase 1
1. Design the `CMRIMaster` public API in `src/CMRIMaster.h` (mirror the
   plan's state machine; discuss before implementing).
2. Scaffold `examples/CMRI-Master/` from the donor sketch.
3. Tracer bullet per PLAN.md Phase 1: P out, R in, hit/miss on OLED.
