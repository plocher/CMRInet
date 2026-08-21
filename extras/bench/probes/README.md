# Bench Probes

This directory holds testbeds for exploring specific, deep issues on real hardware, isolated from the rest of the examples. 

## Structure

Each investigation gets its own directory (`IssueNN/`) so its scripts and data do not mingle with other runs.

`IssueNN/`
├── `README.md` (Context, run instructions)
├── `Makefile` (test_tooling, gather_data, analyze_data, all)
├── `gather_data.py` (The harness that speaks to the board)
├── `analyze_data.py` (The script that computes a verdict)
├── `test_tooling.py` (Unit tests for the python scripts)
└── `data/`
    └── `results.YYYYMMDD/` (Timestamped output so sweeps never overwrite each other)

The directory listing is the catalog; there is no separate index document.

## Sketch Policy

We avoid creating throwaway sketches for each issue. `examples/XiaoHostTracer/XiaoHostTracer.ino` and `TracerShell` are the durable programmable-testbed surface. 
Verbs added for one investigation stay compatible for all subsequent ones. Any change that would break an old test suite must be backported into an earlier tracer version stored alongside the old investigation's tooling, or the change is not landed. Bump `kVersion` on every additive change.

## Data Directory Convention

Sweep outputs must land in `data/results.YYYYMMDD[.#]/`. The harness must never overwrite an existing directory. The trailing `.#` is a same-day disambiguator if needed.
