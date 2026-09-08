# IoxJig — cpNode-IOX manufacturing-validation jig

Standalone validator for assembled cpNode-IOX cards (issue #32). It talks to
the MCP23017 expanders directly over the local I2C bus — no CMRI, no RS-485,
no host. The CMRI engines are absent by design: the jig judges the card's
electrical reality (chips, registers, jumpered loopback paths), not the
protocol stack on top of it.

A run scans the bus, health-checks every found chip at register level, then
drives a **pattern matrix** through every declared loopback coupling in
**both port directions** under a sweep of **rate blocks** (step pacing ×
I2C clock), self-reading the driver stage on every step so a fault names its
stage. The run ends in a `GREEN OK` / `RED FAIL` verdict on the OLED, the
onboard LED, and the serial stream.

## Wiring

Board: cpNode-Xiao (Seeed XIAO ESP32-C6).

| Pin | Function |
|-----|----------|
| D4  | SDA — shared I2C bus: expanders 0x20-0x27 + OLED 0x3C |
| D5  | SCL |
| D2  | optional start button to GND (`JIG_USE_BUTTON`; no hardware in v1) |
| LED_BUILTIN | lights when the verdict lands |

The DUT is the IOX card's expander chain on that same bus. For validation the
card's ports are looped back to each other with a jumper/cable set — e.g. the
bench testbed: one MCP23017 at 0x20 with an 8-conductor cable set joining
port A bit *i* to port B bit *i*.

**Safety doctrine (non-negotiable):** a pin pair joined by a jumper must
never have both ends configured as outputs. `jigplan.h` makes double-drive
structurally unrepresentable — see *Jumper map* below — and `jigiox.cpp`
enforces the ordering procedurally (all ports INPUT first, OLAT preloaded
before any OUTPUT flip).

## Jumper map

Discovery finds the chips; the **map** says which pins are wired to which.
Edit `kJumperPairs[]` at the top of `IoxJig.ino` to match your assembly. The
default is the bench testbed (single 0x20, full 8-bit A<->B).

The map is validated before ANY direction write. `validateMap()` refuses —
with a named reason on the verdict line — when:

- a bit index is outside 0..7 (`map_bad_bit`) or an address outside the
  0x20-0x27 expander window (`map_bad_addr`);
- a pair joins two pins of the same port (`map_same_port_pair`) — direction
  is per-port, so such a pair could never be walked;
- one pin is claimed by two pairs (`map_duplicate_pair`);
- one (chip, port) would drive one pair and read another (`map_role_conflict`)
  — that port would be OUTPUT and INPUT at once, and its partner pair would
  see both jumper ends driven.

A map that passes validation cannot produce a both-OUTPUT condition in
either walk phase, whatever the physical jumper reality is.

## Pattern matrix

One **step** writes ONE pattern byte to one driver port and judges every
paired reader bit of that coupling at once. Each coupling (driver port →
reader port) runs 26 patterns per phase, in six families:

| Family | Patterns | What it stresses |
|--------|----------|------------------|
| `walker_set` | one-hot sweep `0x01..0x80` | single-bit paths, set-in-cleared |
| `walker_cleared` | inverted sweep `0xFE..0x7F` | single-bit paths, cleared-in-set |
| `roll2` | `0x03, 0x0C, 0x30, 0xC0` | adjacent-bit pairs — bridges, crosstalk |
| `roll4` | `0x0F, 0xF0` | nibble groups switching together |
| `checker` | `0x55, 0xAA` | maximum adjacent-bit alternation |
| `flash` | `0x00, 0xFF` | every bit at once — di/dt, pull-up headroom |

Both phases run every pattern (phase 1 drives endpoint 1 of each pair,
phase 2 reverses), so with the default map one repetition is
8 pairs' coupling × 26 patterns × 2 phases = 52 steps.

## Rate blocks

Detection doctrine: **fast dominates slow** — anything that passes at line
speed passes at 1 s settle — so the schedule runs fast-first, pushes to the
bus limits, and comes back down ("and back again"). The slow blocks exist to
*characterize* faults (fails even at the slowest pace = hard fault; fails
only at speed = marginal), not to re-detect. Default schedule
(`kRateBlocks[]` in `IoxJig.ino`), repeated `JIG_MATRIX_REPEATS` times:

| Block | Settle | I2C clock | Purpose |
|-------|--------|-----------|---------|
| 0 | 50 ms | 100 kHz | characterization pace ("slow is ms") |
| 1 | 10 ms | 400 kHz | fast-mode bus |
| 2 | 0 ms | 800 kHz | full-out, above OLED spec |
| 3 | 0 ms | 1 MHz | the bus limit — may not hold; that is a datum |
| 4 | 10 ms | 400 kHz | descent |
| 5 | 50 ms | 100 kHz | back to the characterization pace |

1 MHz is **not required to work**: "the bus works at 400/800 but not 1M" is
itself a finding — it may characterize the bench topology (pull-ups,
capacitance) as much as the DUT. Every fault line and the verdict's
`faults_by_block` array name the block, so rate-vs-reliability reads
straight off the transcript.

**Strict verdict:** ANY miss at ANY rate block faults the run. After a
faulted step the jig takes `JIG_RETRIES` statistical re-reads — they NEVER
mask the fault; they classify it. Per-pair read counters (retries included)
yield `stuck_at` (every read failed) vs `marginal` (some passed), and
`faults_slowest` says whether the slowest pace saw it too (hard) or only
speed did (margin).

## OLED boundary doctrine

Display updates happen at run **boundaries only** — boot, inventory,
rate-block starts, verdict — never inside a block. OLED bus traffic never
interleaves with expander transactions, and an OLED failure (the SSD1306 is
out of spec above 400 kHz) can never influence expander results: a missed
update at a high-rate boundary just means a less frequent picture. The
serial tier carries the whole story regardless; with no panel at all the run
degrades headless.

Fault identification is visual, in the donor node grid's vocabulary (static,
no animation): block-boundary screens carry a `b<block> p<pair> <class>`
summary of the most recent fault, and the RED verdict screen draws a **fault
map** — one row per chip, Port B cell group left / Port A right, bits 7..0
left-to-right; failed bits are filled and halo-boxed, healthy bits hollow.
The bottom identification line carries the chip address(es) and the worst
pair (`20 p6 A6-B6`) on the left and the fault classes on the right. Both
endpoints of a failed pair are marked: through a loopback the two ends are
not separable, so the marked path is the honest unit.

## Run flow

1. Boot screen on OLED, identity line on serial, 3 s grace window
   (`kAutoRunDelaySecs`) — lets the serial console attach.
2. The run auto-starts (v1 has no D2 button hardware). After the verdict the
   board halts with the LED on; **any serial line re-triggers a fresh run** —
   no reflash, no reset. With `JIG_USE_BUTTON` built in, a D2 press does the
   same.
3. Sequence: map validation -> bus scan -> register health floor ->
   `JIG_MATRIX_REPEATS` × (rate block × (phase 1, phase 2)) -> per-pair soak
   health -> verdict. Default totals: 52 × 6 × 2 = 624 steps, tens of
   seconds wall clock.

## Compile-time knobs

| Knob | Default | Meaning |
|------|---------|---------|
| `JIG_VERSION` | `1.1.0` | identity string on the boot line |
| `JIG_USE_OLED` | `1` | `0` runs headless |
| `JIG_USE_BUTTON` | `0` | enable the D2 start button (needs hardware) |
| `JIG_MATRIX_REPEATS` | `2` | repetitions of the whole rate-block sweep |
| `JIG_RETRIES` | `3` | statistical re-reads after a fault (never mask it) |
| `JIG_IDLE_PATTERN` | `0xFF` | driver-port OLAT preload before enabling |

## Serial format

115200 baud. JSON lines (one event per line) plus one human `VERDICT:`
banner. Events:

- `boot` — image identity, version, pair count.
- `run_start` — pairs, blocks, reps, steps per repetition.
- `refused` — map refused; `cause` names the `validateMap()` reason.
- `scan` — expander addresses that ACKed + unknown-responder count.
- `scan_note` — a chip found but not covered by the map
  (`present_not_walkable`).
- `setup_fault` — pre-walk failures: `chip_absent`, `register_fault`,
  `direction_config_fault` (the last carries `rep`/`block`/`phase`).
- `chip_health` — per-chip register probe result (`ok`).
- `rate` — a rate block begins: `rep`, `block`, `delay_ms`, `i2c_khz`.
- `phase` — walk phase begins within the block.
- `step_fault` — one step failed: `class` is `driver` (GPIO self-read
  disagreed with the commanded pattern, or the write NAKed — `cause:
  i2c_write`), `loopback` (self-read OK; paired reader bits missed), or
  `setup` (`cause: i2c_read`). Carries `rep`, `block`, `phase`, `family`,
  `pattern`, the drive/self/expected/observed bytes, the paired-bit `mask`,
  the `miss` byte, and `retry_pass` (how many statistical re-reads passed).
- `soak` — per-block summary: steps and faults run in that block, with its
  `delay_ms`/`i2c_khz`.
- `pair_health` — per-pair soak health after the matrix, for unhealthy
  pairs only: `stuck_at` (every read failed) or `marginal` (some passed),
  with read-level `attempts`, `faults`, and `faults_slowest` (failures in
  the slowest block — the hard-vs-speed-margin datum). A healthy pair is
  the absence of a line.
- `verdict` — `green`, steps, faults, `classes` (bitmask rendered as e.g.
  `setup|loopback`), and `faults_by_block` (fault count per rate block —
  the rate-vs-reliability datum).

Then the human banner: `VERDICT: GREEN OK (steps=624 faults=0 classes=none)`
or `VERDICT: RED FAIL ...`.

## Fault classes

- **setup** — refused map, chip absent, register-health failure, direction
  configuration failure, step-level I2C read failure. The card never got a
  fair walk; fix the fixture/card and rerun.
- **driver** — the driver stage did not produce the commanded pattern
  (GPIO self-read mismatch) or refused the write.
- **loopback** — the driver stage was correct but paired reader bits missed.
  Through a loopback alone a missing conductor and a dead reader pin read
  identically, so per-bit misses report `loopback`; a whole reader port
  stuck across all its paired bits escalates to a reader-port fault
  (`stuck_at` pair health on every bit of that port is the evidence). The
  `miss` byte names the failed bits; `pairForReaderBit` (in `jigplan.h`)
  maps them back to jumper pairs.

## Build and flash

Compile (from the library root):

```sh
arduino-cli compile \
  --fqbn esp32:esp32:XIAO_ESP32C6 \
  --library /Users/jplocher/Dropbox/Arduino/libraries/CMRInet \
  --library /Users/jplocher/Dropbox/Arduino/libraries/Adafruit_GFX_Library \
  --library /Users/jplocher/Dropbox/Arduino/libraries/Adafruit_SSD1306 \
  --library /Users/jplocher/Dropbox/Arduino/libraries/Adafruit_BusIO \
  --build-path /tmp/ioxjig_build \
  examples/IoxJig/IoxJig.ino
```

Flash — `arduino-cli upload` takes no `--library` flag; it uploads the
compiled binary from the build directory with `--input-dir`. Resolve the
target port with `extras/bench/bench resolve --role Node --id <n>` (or
`arduino-cli board list`), and confirm board identity per
`docs/testbed-physical-notes.md` — a port path is not a board identity:

```sh
arduino-cli upload \
  -p /dev/cu.usbmodem<NNNN> \
  --fqbn esp32:esp32:XIAO_ESP32C6 \
  --input-dir /tmp/ioxjig_build \
  examples/IoxJig/IoxJig.ino
```

## Files

- `jigplan.h` — Arduino-free planning/validation core: map validation, the
  pattern table, coupling and step enumeration, expected-value math,
  bit-to-pair attribution, fault classification, soak health, verdict
  aggregation. Desktop-tested by `tests/test_iox_jig.cpp` (Unity, via
  `make check`).
- `jigiox.h/.cpp` — MCP23017 access. Every transaction returns its Wire
  status (a NAK is attributable as a chip fault, never read as data);
  `jigApplyPhaseRoles()` enforces the safe ordering: all-INPUT base, OLAT
  preload before OUTPUT, pull-ups + raw polarity on readers.
- `jigdisplay.h/.cpp` — SSD1306 screens; headless-degrading; boundary-only
  draws.
- `IoxJig.ino` — run state machine, rate-block scheduler, serial JSON
  emitter, LED/OLED UX.
