# Bench Probes

This directory holds testbeds for exploring specific, deep issues on real hardware, isolated from the rest of the examples. 
## Current usable probes
The current maintained probe suite is `Issue47/` (backoff-under-loop-stall).

Primary gather/analyze entry points:
- `Issue47/gather_stall_sweep.py` + `Issue47/analyze_stall_sweep.py`
  - Grid sweep for stall-period combinations.
- `Issue47/gather_busy_yield.py` + `Issue47/analyze_busy_yield.py`
  - Busy vs yield comparison runs.
- `Issue47/gather_single_cycle.py`
  - One focused HIL cycle for quick sanity checks.
  - Supports configurable topology (`--real-ua`, `--phantom-ua`, and geometry args), so benches where UA31 is real can target a different phantom UA (for example UA32).

Reusable support scripts:
- `Issue47/_tracer_client.py` (shared CDC/harness transport and run orchestration)
- `Issue47/_gap_deltas.py` and `Issue47/analyze_data.py` (gap/backoff analyzers and verdicts)
- `Issue47/capture_sniffers.py` (sniffer capture helper)
- `Issue47/quiesce_host.py` (host quiesce helper)
- `Issue47/gather_sync_test.py` (boot/sync check harness)

Legacy/auxiliary Issue47 scripts (kept for compatibility or ad-hoc use):
- `Issue47/gather_data.py` (older sweep entry point; prefer `gather_stall_sweep.py`)
- `Issue47/sanity_check_1.py`
- `Issue47/refactor.py`

Validation tests for probe tooling:
- `Issue47/test_tooling.py`
- `Issue47/test_stall_sweep.py`
- `Issue47/test_busy_yield.py`
- `Issue47/test_analyzer_vocabulary_contract.py`

## Inventory and lifecycle
Each investigation gets its own directory (`IssueNN/`) so scripts and data do not mingle with other runs.

As of now:
- `Issue47/` is active and maintained.
- `regressions/` currently contains no maintained runnable probe entry points (only cache artifacts from earlier experiments).

## Structure (pattern)

`IssueNN/`
├── `README.md` (Context, run instructions)
├── `Makefile` (optional helper targets)
├── `gather_*.py` (Harness scripts that speak to the board)
├── `analyze_*.py` (Scripts that compute verdicts)
├── `test_*.py` (Unit tests for probe tooling)
└── `data/`
    └── `results.YYYYMMDD/` (Timestamped output so sweeps never overwrite each other)

The directory listing is the catalog; there is no separate index document.

## Sketch Policy

We avoid creating throwaway sketches for each issue. `examples/TracerHost/TracerHost.ino` and `TracerShell` are the durable programmable-testbed surface. 
Verbs added for one investigation stay compatible for all subsequent ones. Any change that would break an old test suite must be backported into an earlier tracer version stored alongside the old investigation's tooling, or the change is not landed. Bump `kVersion` on every additive change.

## Data Directory Convention

Sweep outputs must land in `data/results.YYYYMMDD[.#]/`. The harness must never overwrite an existing directory. The trailing `.#` is a same-day disambiguator if needed.
