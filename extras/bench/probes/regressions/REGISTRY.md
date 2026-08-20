# Regression registry

Every regression that has a durable bench reproduction is listed here.
The canonical entry is authoritative: it names the issue, the defines
that activate the probe in `RegressionHost.ino`, how to reproduce the
symptom, and what a correct fix should look like.

`extras/bench/probes/regressions/sweep_47.py` is the harness for #47.
It drives the `XiaoHostTracer` sketch over USB CDC using capture-mode
verbs (`run`, `dump`, `reset`, `enable stall`, etc.) instead of relying
on compile-time configuration.

## Adding a new regression

1. Extend `examples/XiaoHostTracer/XiaoHostTracer.ino` if new generator
   commands or trace behaviors are required.
2. Write a Python harness (like `sweep_47.py`) that sets up the conditions,
   triggers a `run`, captures the `dump`, and passes it to an analyzer.
3. (Optional) Drop a `analyzers/<issue>_*.py` next to this file that
   consumes the captured block and prints a PASS/FAIL summary.
4. Add a section here.

## #47 — Backoff-under-loop-stall

- **Issue**: [plocher/CMRInet#47](https://github.com/plocher/CMRInet/issues/47)
- **Status**: open
- **Harness**: `sweep_47.py`
- **Sketch**: `XiaoHostTracer`
- **Commands**: `node add 30 7 7`, `node add 31 4 4`, `enable stall <ms> period <p> mode <m>`
- **Analyzer**: `analyzers/47_gap_deltas.py`

### Reproduction

The Python harness:

1. Connects to `XiaoHostTracer` and validates its boot line.
2. Registers two nodes via runtime verbs: UA 30 (real node) and UA 31 (phantom).
3. Sends `enable stall` to inject blocking/yielding stalls in `loop()`.
4. Arms a capture using `run <secs>`. The sketch records `I` and `T` packets to a RAM ring buffer.
5. Emits `dump` to retrieve the ring buffer contents, delineated by `BEGIN DUMP` and `END DUMP` markers.
6. Feeds the dump to the analyzer.

### Expected vs observed

`CMRIHost` schedules a retry poll for a non-responsive node with
exponential backoff, doubling per miss up to a 32 s cap (see issue
#41). The failing node here is UA 31 (phantom).

- **Expected**: gaps between successive UA-31 polls double from
  ~250 ms → 500 ms → 1 s → 2 s → 4 s → 8 s → 16 s → 32 s and hold.
- **Observed** (with the fake-stall probe active): gaps cycle in a
  narrow band around ~1 s and never accumulate. Same behavior
  reproduces with the real Adafruit OLED draw — that positive
  regression test is what filed #47 in the first place.

### What a correct fix looks like

A fix should let the poll-retry deadline accumulate even when
`host.tick()` is called after each stall — that is, the accumulated
backoff must be measured from when the retry was *scheduled*, not
from the arrival of the next `tick()`. The 2D grid sweep of
(magnitude × cadence) is still open: the analyzer below reports gap
distribution so future investigators can decide whether the bug is
magnitude-thresholded, duty-cycle-thresholded, resonance-specific,
or triggered by any recurring block.

### Grid sweep

`sweep_47.py` sweeps the (stall_ms × stall_period_ms) grid by sending
C&C verbs to `XiaoHostTracer` over the CDC serial port. Per-combo
raw captures land in `sweep_results/` with value-named files
(e.g. `s25_p150_yield.log`), and `sweep_results/summary.csv` records the
verdict and gap statistics for the whole grid. Resumable (existing combos
are skipped). Not an agent — run it yourself.
