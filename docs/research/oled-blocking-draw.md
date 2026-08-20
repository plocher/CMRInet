# OLED driver options — blocking-draw impact on the host scheduler

Investigation triggered by #11: enabling the OLED on the Xiao Host
disrupted `CMRIHost`'s poll schedule — UA30 turnaround bounced
6ms↔26ms and UA31's poll-retry backoff stopped accumulating (stuck
cycling ~1/s instead of doubling to a 32s cap). This note records what
was tried, what was refuted, and the options forward.

**Update (session close):** the final probe (`delay(25)` at 150 ms
cadence, no OLED calls at all) reproduces the stuck backoff
identically to the Adafruit draw path. The bug is *not*
display-specific — it is a `CMRIHost` deadline-accumulation bug
triggered by any recurring blocking work of similar duty cycle. Filed
separately as #47 for library-side diagnosis. The OLED options below
remain valid recommendations for the display-side concern, but they
would only mask the underlying library bug from sketches that happen
to have a well-behaved draw path — they do not fix the bug for other
callers with SD writes / LoRa frames / EEPROM commits / etc. in
their loops.

## The mechanism

`Adafruit_SSD1306::display()` has no dirty-rectangle tracking — it
always streams the full 1024-byte framebuffer over I2C, blocking
`loop()` (and therefore `host.tick()`) for the duration of the
transfer. At 400kHz that is ~23ms; the theoretical 1MHz floor is ~9ms.
A single CMRI poll/reply exchange is ~14ms, so a draw that straddles
an exchange overruns it: the reply lands while `loop()` is blocked in
the I2C transfer, and the measured turnaround (`nowMs -
gateArmedMs_`) reads ~6+20≈26ms instead of the true ~6ms. The same
~20ms stall corrupts the backoff `Deadline` accumulation, preventing
UA31's retry interval from doubling.

This violates the library's non-blocking-tick design (D6): the engine
assumes `loop()` runs fast between ticks.

## What was tried and refuted

### I2C at 1MHz (Fast Mode Plus) — refuted as sufficient

Bumped `Wire.setClock(1000000)` after `display.begin()` on the
diagnostic build (OLED on, `onTrace` packet logging, CDC
`setTxTimeoutMs(0)`). Measured via 75s CDC trace capture:

- **Latency: still bimodal, same ~20ms delta.** Distribution peaked
  at 9ms (1306 samples, the fast floor) and 29–33ms (secondary
  cluster). Before 1MHz: 6ms vs 26ms (20ms delta). After: 9ms vs
  ~30ms (~21ms delta). The per-draw stall did not shrink — either the
  ESP32-C6 I2C peripheral isn't clocking at 1MHz (pull-up / clock-tree
  limit), or Arduino Wire's ≤32-byte chunking overhead dominates at
  this rate and the theoretical 9ms isn't reachable. The 9ms floor
  (was 6ms) is just a shifted baseline — every exchange got ~3ms
  slower from something, and the stall is still ~20ms on top.
- **Backoff: still not accumulating.** 76 UA31 polls in 75s with gaps
  clustering at 600–1950ms — stuck cycling in the ~1s band, never
  doubling toward 32s. Same as the OLED-on build at 400kHz.

**Conclusion:** the stall *magnitude* (~20ms of blocked loop) is what
disrupts the scheduler, not the bus *speed*. Halving the bus speed did
not halve the stall, and even if it had, ~9ms would still be marginal
against a ~14ms poll cycle. Faster I2C within the full-buffer Adafruit
model is the wrong lever. Reverted — no benefit, and 1MHz carries
side-effect risk (marginal pull-ups, affects all I2C devices on D4/D5).

### Adafruit vs plain `delay(25)` — confirmed identical (positive regression)

Bench probe with three `#define` knobs in
`extras/bench/probes/SimpleHostDiag2/SimpleHostDiag2.ino` isolated the
draw path from any suspicion of Adafruit- or I2C-specific mechanism:

| Configuration | UA 96 polls in 60 s | Gap pattern | Backoff |
| --- | --- | --- | --- |
| No draws, no stall | 5 | 508 → 16 261 → 32 262 ms | ✅ doubling |
| No draws + `delay(25)` every 150 ms | 59 | 600 – 2 550 ms, stuck | ❌ stuck |
| Full Adafruit draw every 150 ms | 63 | 600 – 2 100 ms, stuck | ❌ stuck |

A naked `delay(25)` at the same cadence reproduces the bug identically
to the Adafruit draw path (63 vs 59 polls; same gap distribution).
The mechanism is any recurring ~17% duty-cycle blocking loop stall,
not Adafruit or I2C or SSD1306. Rules out: Wire interrupt masking,
I2C bus arbitration, Adafruit GFX state pollution, or any
library-init side effects. Filed as issue #47.

### `clearDisplay() + display()` at setup only — no disruption

Same diagnostic build with `DIAG_INIT_DISPLAY=1`, `DIAG_DRAW=0`: a
single ~23 ms blocking framebuffer transfer at boot did not disrupt
backoff (identical to no-init case: 5 polls in 60 s, doubling to
32 s). Confirms the stall *magnitude* is not what matters — a
*single* 23 ms stall is harmless. It is the *recurrence pattern* at
150 ms cadence that breaks deadline accumulation.

### Redraw-on-change alone — refuted as sufficient (by analysis)

The panel renders `lastTurnaroundMs` raw (no smoothing). That field is
overwritten on every accepted reply, so between two 150ms redraws
(~10 exchanges) the rendered row text differs nearly every time → the
"did anything change?" check almost always says yes → the full draw
fires anyway. Redraw-on-change gives ~no reduction while the latency
field is live and unsmoothed. It only becomes effective if the
volatile field is *also* quieted (smoothed/quantized) — but smoothing
hides the very diagnostic (the 6↔26 bounce) that tells us a draw
straddled an exchange. So smoothing is a last-resort fallback that
trades a genuine signal for display stability.

## The lever that works: reduce bytes per draw

The sufficient target is per-draw block ≲ a few ms — well under one
poll cycle (~14ms), so a draw no longer straddles an exchange and the
backoff `Deadline` can accumulate again. Reached by:

- **U8x8 text mode** (no framebuffer; `drawString` writes 8×8 cells
  directly to GDDRAM). A header update is ~7 cells (~60–100B, <1ms)
  instead of 1024 bytes. Best fit for this text-only panel; loses
  graphics, restricts to 8×8/8×16 fonts (fine here).
- **Partial/page flush** (SSD1306 window commands, push only changed
  pages). One 128-byte page is ~3ms at 400kHz. Adafruit lacks this
  API; U8g2 has `updateDisplayArea()` / `setBufferCurrTileRow()`.
- **Segmented flush** (one page per `loop()` pass, cycling through 8
  pages). No single `loop()` blocks more than one page-time. The
  design-correct D6 answer: makes the draw genuinely non-blocking by
  construction.

All three stack with redraw-on-change, which becomes a free
multiplier once the per-draw cost is negligible (can redraw
unconditionally every 150ms and still keep the raw latency
diagnostic).

## Driver landscape (researched)

- **Adafruit_SSD1306** (current): full-buffer only, no partial flush,
  no dirty tracking. `display()` always pushes 1024 bytes.
- **U8g2** (olikraus, 6.4k stars): three modes — full (`F`, same as
  Adafruit), page/tile (`1`/`2`, 1/8 RAM, `setBufferCurrTileRow` for
  single-row push), and U8x8 character (no framebuffer at all). Has
  `updateDisplayArea()` for dirty-rectangle partial flush. None are
  non-blocking — all call Arduino `Wire` and block until transfer
  completes. No async/DMA without writing a custom low-level I2C
  callback.
- **bitbank ss_oled / OneBitDisplay**: lightweight, near-zero RAM for
  byte-boundary text, fast. Smaller footprint than U8g2.

A hardware constraint: the ESP32-C6 is single-core, so offloading the
draw to a FreeRTOS task does not give true parallelism — a blocking
I2C transfer still monopolizes the one core. On the C6 the only real
levers are fewer bytes (U8x8/partial) and faster clock.

## Recommendation

For this text-only panel: **U8x8 text mode** is the cleanest real fix
— cuts per-draw transfer 10–100×, keeps the raw latency diagnostic,
and is contained to each sketch's draw function (the `HostStatusPanel`
already returns strings, so the metric logic is driver-agnostic). The
scope of a U8x8 migration touches SimpleHost, XiaoHostTracer, and
XiaoSniffer (all have OLEDs); the cpNode `Xiao_I2C` node is in a
different library/repo and should be a separate follow-up.

Smoothing + redraw-on-change is the fallback of last resort, only if
forced to stay on full-buffer Adafruit — explicitly accepting the loss
of the bounce diagnostic. A max-watermark latency field (peak over a
window, slow-decay) is a spread-preserving alternative worth
considering: stable enough for redraw-on-change if ever needed, but
still surfaces loop-stall events instead of averaging them away.
