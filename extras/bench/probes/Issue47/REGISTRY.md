# Issue 47 Probe Registry

This document explains the round-2 grid selection and the reasons for pruning the matrix corners.

## Grid Rationale

The round-1 grid was a simple matrix. It produced two types of waste. The first type was redundant cells where the period was less than or equal to the stall time. The injector sets the timer before the delay. When the stall time is greater than or equal to the period, the delay triggers on every loop iteration. The loop cadence becomes locked to the stall time. The period axis has no effect. 

The second type of waste was poor resolution across the failure cliff at approximately 9 milliseconds. Round-1 had a five-point gap between 10 and 15 milliseconds.

To fix these problems, the round-2 grid uses specific defaults:

*   **Stalls**: [1, 3, 5, 7, 8, 9, 10, 11, 12, 15, 20, 30, 50, 100, 250]. This array gives fine resolution near the cliff and coarse resolution above it.
*   **Periods**: [125, 145, 150, 155, 200, 233, 250, 373, 500]. This array includes periods that align with the internal 250 ms timers (125, 250, 500). It also includes periods that do not align (145, 155, 200, 233, 373). This mix tests the timer-aliasing hypothesis. The value 150 gives continuity with round-1.

## Pruning Rules

The harness skips combinations where the period is less than two times the stall time (`p < 2*s`). The harness marks these combinations as `SKIPPED_REDUNDANT` in the `summary.csv` file. 

The harness uses a default capture window of 60 seconds. This window allows you to observe two full cycles if the backoff reaches its cap.

## Data Preservation

The round-1 sweep data under the `data/results.20260820/` directory is preserved. The round-2 file naming uses the same format (e.g., `s{stall}_p{period}.log`). A new directory (e.g., `results.20260821`) is created for each run. If a run output uses the same base directory, the files will overwrite cells that were also in round-1. This behavior is intentional.
