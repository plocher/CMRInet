# Issue 47: Backoff-under-loop-stall

- **Issue**: [plocher/CMRInet#47](https://github.com/plocher/CMRInet/issues/47)
- **Status**: open
- **Harness**: `gather_stall_sweep.py`, `gather_busy_yield.py`
- **Sketch**: `TracerHost`
- **Commands**: `reboot`, `node add 30 C 7 7`, `node add 31 C 4 4`, `enable stall <ms> period <p> mode <m>`
- **Analyzer**: `analyze_stall_sweep.py`, `analyze_busy_yield.py`

## Background

The host engine uses an exponential backoff timer for offline nodes. The timer doubles after each missed poll until it hits 32 seconds. This prevents an offline node from using bus bandwidth.

The backoff timer fails to increase if the `loop()` function stalls, even when the stall is shorter than the reply timeout. The timer gets stuck between 600 ms and 2 s. The offline node receives polls at ~1 Hz forever.

This bug occurred first during the OLED display tests (issue #11). A plain `delay(25)` command in the main loop reproduces the exact failure. Any code that blocks the loop (SD writes, sensor reads) causes this bug.

## Reproduction

The Python harness (`gather_stall_sweep.py` or `gather_busy_yield.py`):

1. Sends `reboot` and waits for the device to re-enumerate on CDC, providing a clean-slate hardware state. It then connects to `TracerHost` and validates the boot message.
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

The `gather_stall_sweep.py` script sweeps the grid (stall duration × stall period) by sending commands to `TracerHost` over the CDC serial port. The raw captures land in `data/results.<today>/` with value-named files (e.g. `s25_p150_yield.log`). The `summary.csv` file records the verdict and the gap statistics for the whole grid. The sweep resumes if interrupted. Existing combos are skipped. This is not an automated agent task — you must run it yourself.


## Resolution
### Rerun performed
   - Flashed updated firmware.
   -  Ran:
       -   gather_single_cycle.py --stall 9 --period 150 --mode yield --traffic "walker loopback" --secs 60 --tag s9_p150_yield_bothua_diag
   -   Artifacts:
       -   data/results_single_cycle/s9_p150_yield_bothua_diag.log
       -  data/results_single_cycle/packets.s9_p150_yield_bothua_diag.Host.raw

  - Reason distribution (from Host.raw)
     - UA96 (phantom): 19 events
         -  initial: 1
         -  miss: 18
         -  accept: 0
         - geometry-mismatch: 0
         -  deadline_action: all arm
         -  32000->32000 cap rearm events: 11
     - UA95 (real node): 2142 events
         -  initial: 16 (arm)
         -  accept: 2126 (disarm)
         -  resets old>0 -> 0: 15
         -  many 0->0 disarm events (visible now by design)

### Correlation test vs fallback hypothesis
Correlated UA96 drop-back gaps with UA95 initial arms:

   -  UA96 drop-back gaps: indices 10..17 (8 total in this run)
   -  All 8/8 had a UA95 initial event inside the same UA96 P->P window
   -  Nearest timestamp offsets were extremely tight: +5 to +7 ms from UA96 gap endpoint

That is strong support for transient mechanism (transient UA95 miss/backoff coinciding with UA96 drop-back cadence), and much stronger than the earlier “dead” read from coarse poll/reply counting.
Confirmed. The 8/8 correlation with +5–7 ms lag is definitive — that's the forced-poll fallback firing within one round-robin visit of every UA=95 transient miss. My earlier "dead" read was wrong because the coarse poll/reply pairing at ≤250 ms couldn't see the misses that the extended DIAG trace now catches.

The arithmetic also reconciles now: 18 miss-and-double events on UA=96 in 60 s (of which 11 are cap-rearms) can't happen from monotone climb-and-hold — that would produce ~8 miss events over 60 s. The extra 10 are forced polls landing while backoff was still armed, each one resetting the wall-clock deadline via pollBackoff_.armIn(nowMs, …) at CMRIHost.cpp:319. That's exactly the forced-poll pattern.

The fix

The bug is at CMRIHost::selectNextNode_ lines 371-378 — the fallback that forces a plain round-robin pick when every enabled node is backed off. The comment above it reads:
``` 
  // Every enabled node is currently backed off (or none is enabled).
  // Fall back to plain round robin rather than stall.
 ```
The reasoning was almost certainly D6 — "the engine never blocks or gives up on a silent Node." That reasoning is misapplied here. pollBackoff_ is the schedule for the silent node. Force-polling ahead of its own backoff violates the backoff's own semantics, resets the wall-clock deadline downstream, and produces the FAIL_CYCLING signature we've been chasing.

Correct behaviour: when every enabled node has an armed-but-not-due backoff, return false and let the caller (tick) return early. The engine still runs — tick() is still called every loop iteration — it just doesn't select a node with nothing legitimately due. That's not blocking; it's the engine correctly recognising it has no scheduled work at this instant.

Concrete change:
``` cpp
bool CMRIHost::selectNextNode_(uint32_t nowMs) {
  for (size_t step = 0; step < nodeCount_; ++step) {
    const size_t candidate = (cursor_ + step) % nodeCount_;
    RemoteNodeHandle& node = nodes_[candidate];
    if (!node.config_.enabled) continue;
    if (node.pollBackoff_.armed() && !node.pollBackoff_.due(nowMs)) continue;
    polledIndex_ = candidate;
    cursor_ = (candidate + 1) % nodeCount_;
    return true;
  }
  // All enabled nodes are backed off. Do not force a poll — let each
  // node's own backoff deadline schedule its next attempt. Returning
  // false lets tick() idle for this iteration without disturbing any
  // node's backoff timeline. VALIDATION: Design v1.1 D6 — the engine
  // still runs and never gives up on a silent Node; the backoff itself
  // is that node's schedule.
  return false;
}
```
The existing caller at line 219-221 already handles selectNextNode_ returning false correctly (returns from runSchedule_, retries next tick). No other change needed there.

The plan-of-record VALIDATION language at CMRIHost.h says "keeps polling the silent Node forever" (interop 2.3.10) and "nothing here blocks or gives up on a silent Node" (D6). Neither requires force-polling ahead of a backoff. "Polling forever" means the backoff never converts to permanent abandonment — a cap of 32 s still qualifies as "polling forever" at 1/32 Hz. "Never blocks or gives up" means tick() always runs and no node is ever removed from the roster. Both are preserved by the fix.

Applied exactly the fix you proposed and verified it on HIL.

Change made
•  Updated src/CMRIHost.cpp in selectNextNode_(uint32_t nowMs):
◦  Removed the fallback loop that force-picked an enabled node when all enabled nodes were backed off.
◦  Now returns false in that case, preserving each node’s own backoff schedule.

Verification run
•  Rebuilt and flashed TracerHost.
•  Ran your specified command:
◦  gather_single_cycle.py --stall 9 --period 150 --mode yield --traffic "walker loopback" --secs 60 --tag s9_p150_yield_fix_fallback

Result
•  Before fix: FAIL_CYCLING
•  After fix: PASS
•  UA=96 summary:
◦  8 TX events (3 I/T, 5+ Ps)
◦  gap sequence became monotonic growth:
▪  521, 779, 1259, 2343, 4254, 8801, 16262
◦  no drop-back pattern
◦  monotone_prefix_max = 16262

This is a clean confirmation that the forced fallback path was driving the cycling signature.  The gap sequence 521, 779, 1259, 2343, 4254, 8801, 16262 is textbook backoff-doubling — you can read the entire ladder in the traces: 250 initial + 250 wait, 500 + 250, 1000 + 250, 2000 + 250, 4000 + 250, then 8801 (that's the 8 s step plus the ~500 ms re-init ladder overhead when missThreshold fires), then 16 s heading toward cap. No drop-back. Exactly what a healthy pollBackoff_ climb should look like.

### Reran full sweep test.  
All 135 cycles PASSED

Full collapse. 123 of 123 active combos PASS. 12 SKIPPED_REDUNDANT as expected. Zero FAIL_CYCLING, zero FAIL_STUCK across the entire grid. Every one of the questions flagged as "watch for these in the rerun":

•  s=250 pure-STUCK behaviour — was 100% FAIL, now the one active combo (p=500) PASSes.
•  s=100 mostly-FAIL — was 100% FAIL, now 100% PASS on the five active periods.
•  p=145 / p=155 aliasing outliers — gone; both periods PASS at every stall.

Before/after on the same 135-combo grid: 31 → 123 PASS, 92 previously-failing cells now pass, 0 failures across the whole map. There is no residual signal to chase.

#47 is fixed.