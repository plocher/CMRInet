# Regression registry

Every regression that has a durable bench reproduction is listed here.
The canonical entry is authoritative: it names the issue, the defines
that activate the probe in `RegressionHost.ino`, how to reproduce the
symptom, and what a correct fix should look like.

`extras/bench/probes/regressions/run.sh <issue-number>` compiles
`RegressionHost` with the entry's defines, flashes the Host board,
captures the CDC stream, and (if an analyzer for the issue exists)
runs it against the capture.

## Adding a new regression

1. Add a `#if defined(...)` guard block in
   `extras/bench/probes/RegressionHost/RegressionHost.ino` that
   activates the desired probe behavior. Name the guard
   `REGRESSION_<issue-number>_<short-slug>` so the source cross-references
   the tracker.
2. Add a case clause to `run.sh` mapping the issue number to the
   `-D` defines the guard needs.
3. (Optional) Drop a `analyzers/<issue>_*.py` next to this file that
   consumes the CDC capture and prints a PASS/FAIL summary.
4. Add a section here.

## #47 — Backoff-under-loop-stall

- **Issue**: [plocher/CMRInet#47](https://github.com/plocher/CMRInet/issues/47)
- **Status**: open
- **Guard**: `REGRESSION_47_BLOCKING_INTERFERES_WITH_BACKOFF`
- **Activate**: `-DDIAG_FAKE_STALL_MS=25 -DDIAG_FAKE_STALL_PERIOD_MS=150`
- **Also useful**: `-DDIAG_TRACE` — adds a per-packet TX/RX trace to
  USB CDC and makes CDC writes non-blocking. The analyzer below
  requires this.
- **Analyzer**: `analyzers/47_gap_deltas.py`

### Reproduction

Register two nodes in `nodeTable[]`: UA 30 with a real Node board
replying, UA 31 phantom (no hardware). The `RegressionHost.ino`
already has this configuration.

Under the probe, the sketch:

1. Suppresses the OLED draw entirely, so nothing about the display
   path is a confounder.
2. Injects `delay(DIAG_FAKE_STALL_MS)` every `DIAG_FAKE_STALL_PERIOD_MS`
   in `loop()` — a plain, minimal, recurring blocking stall.
3. Writes a splash to the OLED at boot with the exact `stall=` and
   `period=` values, so a photograph of the board self-labels the
   captured log.

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

`sweep_47.sh` (next to `run.sh`) sweeps the (stall_ms × stall_period_ms)
grid: it recompiles and reflashes `RegressionHost` for every combination,
captures the CDC trace, and runs the analyzer. Per-combo transcripts and
raw captures land in `sweep_results/` with value-named files
(`s${s}_p${p}.txt` / `.log`), and `sweep_results/summary.csv` records the
verdict and gap statistics for the whole grid. Resumable (existing combos
are skipped unless `--force`). Not an agent — run it yourself.
