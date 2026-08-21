# Issue 47: Backoff-under-loop-stall

- **Issue**: [plocher/CMRInet#47](https://github.com/plocher/CMRInet/issues/47)
- **Status**: open
- **Harness**: `gather_stall_sweep.py`, `gather_busy_yield.py`
- **Sketch**: `XiaoHostTracer`
- **Commands**: `reboot`, `node add 30 7 7`, `node add 31 4 4`, `enable stall <ms> period <p> mode <m>`
- **Analyzer**: `analyze_stall_sweep.py`, `analyze_busy_yield.py`

## Background

The host engine uses an exponential backoff timer for offline nodes. The timer doubles after each missed poll until it hits 32 seconds. This prevents an offline node from using bus bandwidth.

The backoff timer fails to increase if the `loop()` function stalls, even when the stall is shorter than the reply timeout. The timer gets stuck between 600 ms and 2 s. The offline node receives polls at ~1 Hz forever.

This bug occurred first during the OLED display tests (issue #11). A plain `delay(25)` command in the main loop reproduces the exact failure. Any code that blocks the loop (SD writes, sensor reads) causes this bug.

## Reproduction

The Python harness (`gather_stall_sweep.py` or `gather_busy_yield.py`):

1. Sends `reboot` and waits for the device to re-enumerate on CDC, providing a clean-slate hardware state. It then connects to `XiaoHostTracer` and validates the boot message.
2. Registers two nodes via runtime verbs: UA 30 (real node) and UA 31 (phantom).
3. Sends `enable stall` to put stalls in `loop()`.
4. Arms a capture using `run <secs>`. The sketch records `I` and `T` packets to a RAM ring buffer.
5. Emits `dump` to retrieve the ring buffer contents. The markers `BEGIN DUMP` and `END DUMP` delineate the contents.
6. Feeds the dump to the analyzer (`analyze_stall_sweep.py` or `analyze_busy_yield.py`).

### Clean Slate State (Round 1 Methodology)
Because runtime configuration is in place (as of #53/#54), state variables inside the Host library—such as `pollBackoffMs_`, `consecutiveMisses`, `needsInit_`, `freshness_`, `pollDueBy_`, and `lastTxMs_`—do not automatically reset to zero between test scenarios. Without a hardware reboot, a test starting at `stall_ms=1` might inherit backoff state from a previous failed run, corrupting the measurement. To ensure parity with the Round 1 compile-and-flash methodology, the harness sends the `reboot` verb before every scenario, guaranteeing all global timers and node state are cleared.

## Expected behavior

The engine schedules a retry poll for an offline node with exponential backoff. The test targets UA 31 (the phantom node).

- **Expected**: Gaps between UA-31 polls double from ~250 ms to 32 s and stay at 32 s.
- **Observed** (with the fake-stall probe active): Gaps cycle in a narrow band around ~1 s and do not increase.

## What a correct fix looks like

A correct fix ensures the poll-retry timer increases correctly across `host.tick()` calls. The accumulated backoff must start from when the retry was scheduled, not from when the next `tick()` occurs.

## Grid sweep

The `gather_stall_sweep.py` script sweeps the grid (stall duration × stall period) by sending commands to `XiaoHostTracer` over the CDC serial port. The raw captures land in `data/results.<today>/` with value-named files (e.g. `s25_p150_yield.log`). The `summary.csv` file records the verdict and the gap statistics for the whole grid. The sweep resumes if interrupted. Existing combos are skipped. This is not an automated agent task — you must run it yourself.
