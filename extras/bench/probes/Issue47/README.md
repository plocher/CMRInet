# Issue 47: Backoff-under-loop-stall

- **Status**: open
- **Harness**: `gather_data.py`
- **Sketch**: `XiaoHostTracer`
- **Commands**: `node add 30 7 7`, `node add 31 4 4`, `enable stall <ms> period <p> mode <m>`
- **Analyzer**: `analyze_data.py`

## Reproduction

The Python harness (`gather_data.py`):

1. Connects to `XiaoHostTracer` and validates its boot line.
2. Registers two nodes via runtime verbs: UA 30 (real node) and UA 31 (phantom).
3. Sends `enable stall` to inject blocking/yielding stalls in `loop()`.
4. Arms a capture using `run <secs>`. The sketch records `I` and `T` packets to a RAM ring buffer.
5. Emits `dump` to retrieve the ring buffer contents, delineated by `BEGIN DUMP` and `END DUMP` markers.
6. Feeds the dump to the analyzer (`analyze_data.py`).

## Expected vs observed

`CMRIHost` schedules a retry poll for a non-responsive node with exponential backoff, doubling per miss up to a 32 s cap. The failing node here is UA 31 (phantom).

- **Expected**: gaps between successive UA-31 polls double from ~250 ms → 500 ms → 1 s → 2 s → 4 s → 8 s → 16 s → 32 s and hold.
- **Observed** (with the fake-stall probe active): gaps cycle in a narrow band around ~1 s and never accumulate. 

## What a correct fix looks like

A fix should let the poll-retry deadline accumulate even when `host.tick()` is called after each stall — that is, the accumulated backoff must be measured from when the retry was *scheduled*, not from the arrival of the next `tick()`. 

## Grid sweep

`gather_data.py` sweeps the (stall_ms × stall_period_ms) grid by sending C&C verbs to `XiaoHostTracer` over the CDC serial port. Per-combo raw captures land in `data/results.<today>/` with value-named files (e.g. `s25_p150_yield.log`), and `summary.csv` records the verdict and gap statistics for the whole grid. Resumable (existing combos are skipped). Not an agent — run it yourself.
