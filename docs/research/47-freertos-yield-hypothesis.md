# Issue #47 — FreeRTOS yield hypothesis follow-up
This companion note records the confirmed causal chain after the earlier "hypothesis busted" checkpoint.
## Final root cause
The cycling signature was not caused by FreeRTOS yield behavior itself.
The confirmed mechanism was scheduler-side:
- Under loop stall, UA=95 can incur transient misses.
- That transient miss arms backoff on both nodes.
- The old `CMRIHost::selectNextNode_` fallback force-selected an enabled node even when every enabled node was still backoff-armed.
- That forced selection poll-hit UA=96 before its own deadline, and `armIn(nowMs, ...)` reset UA=96's wall-clock backoff schedule from "now."
- Repeating that pattern produced apparent `FAIL_CYCLING` cadence on wire captures.
The functional fix is to return `false` when all enabled nodes are currently backoff-armed, letting the scheduler idle until a real due deadline exists.
## Hypothesis chain summary
- Early hypothesis: yield jitter alone explained the cliff.
- Counter-evidence: forced-poll predictions did not match one run, so that branch was discarded.
- Refined hypothesis: fallback force-poll path was perturbing backoff deadlines.
- Validation: dual-UA diagnostics showed tight temporal correlation between UA=95 transient backoff activity and UA=96 drop-backs.
- Confirmation: removing forced fallback changed `s=9,p=150` from `FAIL_CYCLING` to `PASS` with monotonic growth.
## Round-1 reconciliation (retro)
Round-1 runs also exercised this fallback path during transient UA=95 misses, but the round-1 analyzer (v1) did not classify `FAIL_CYCLING`; the observed "s≈9 cliff" was therefore a mix of the real scheduling bug and analyzer under-diagnosis.
