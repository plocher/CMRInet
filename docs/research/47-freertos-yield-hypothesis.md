# Issue 47: FreeRTOS Yield Hypothesis

## Background
Issue #47 describes a failure where the host's exponential backoff timer for offline nodes fails to accumulate correctly when the `loop()` stalls for short periods (~9 ms or more). One hypothesis was that `delay()` (which yields control to the underlying FreeRTOS scheduler) causes side effects, task starvation, or timer skew compared to a pure busy-wait.

## Experiment
To test this hypothesis, we captured a targeted 7×2 sweep using `gather_busy_yield.py` (part of the #50 scenario):
- **Stalls (`s`)**: 6, 7, 8, 9, 10, 11, 12 ms
- **Period (`p`)**: 150 ms
- **Traffic**: off
- **Modes**: `yield` (using `delay()`) vs `busy` (blocking CPU loop)

If the FreeRTOS-yield hypothesis was correct, the `busy` mode would accumulate backoff successfully (`PASS`), while the `yield` mode would stall (`FAIL_STUCK`).

## Results

| Stall (ms) | Period (ms) | Mode | Verdict | Max Gap (ms) |
| --- | --- | --- | --- | --- |
| 6 | 150 | yield | `FAIL_STUCK` | 1651 |
| 6 | 150 | busy | `FAIL_STUCK` | 2552 |
| 7 | 150 | yield | `FAIL_STUCK` | 2405 |
| 7 | 150 | busy | `FAIL_STUCK` | 4204 |
| 8 | 150 | yield | `FAIL_STUCK` | 3302 |
| 8 | 150 | busy | `FAIL_STUCK` | 2545 |
| 9 | 150 | yield | `FAIL_STUCK` | 3548 |
| 9 | 150 | busy | `FAIL_STUCK` | 2602 |
| 10 | 150 | yield | `FAIL_STUCK` | 2602 |
| 10 | 150 | busy | `FAIL_STUCK` | 2070 |
| 11 | 150 | yield | `FAIL_STUCK` | 2329 |
| 11 | 150 | busy | `FAIL_STUCK` | 2108 |
| 12 | 150 | yield | `FAIL_STUCK` | 2358 |
| 12 | 150 | busy | `FAIL_STUCK` | 2018 |

## Interpretation
The results definitively **bust the FreeRTOS-yield hypothesis**. The `FAIL_STUCK` behavior manifests identically in both `yield` and `busy` modes around the 6-12 ms stall range. The max gap generally hovers between 1500 ms and 4500 ms, never accumulating toward the 32-second cap, regardless of how the CPU is stalled.

Because yielding to FreeRTOS is not the culprit, the bug must reside directly in the `CMRIHost` tick and backoff timing logic (e.g., how `nowMs` differences are calculated or how scheduled time boundaries interact with loop execution delays).

## Follow-up
The investigation into `CMRIHost.cpp` should focus on the exact timer logic used for calculating backoff durations, independent of RTOS task-switching mechanics.
