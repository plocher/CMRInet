# Bench probes

Durable reproduction artifacts for filed regressions. Not examples, not
shipped, not compiled by CI. Reference-only, kept in the repo so that
the next investigator to touch a known bug does not have to
reconstruct the setup from chat history.

## Layout

- `RegressionHost/RegressionHost.ino` — a single foundation sketch.
  With no probe defines it behaves identically to
  `examples/SimpleHost/SimpleHost.ino` (the regression baseline).
  Individual bugs are activated by CLI defines; guarded blocks in the
  sketch are named `REGRESSION_<issue-number>_<slug>` so grep points
  straight at the tracker.

- `regressions/REGISTRY.md` — the human-readable list of known
  regressions. One section per issue: activation defines, how to
  reproduce, expected vs observed behavior, and what a correct fix
  should look like.

- `regressions/run.sh <issue-number>` — the harness. Compiles
  `RegressionHost` with the right defines, uploads, captures the CDC
  stream, and (if a per-issue analyzer exists) runs it.

- `regressions/analyzers/<issue>_*.py` — optional per-issue capture
  analyzers. Each prints a PASS/FAIL summary. `run.sh` picks the
  first match for the requested issue.

- `regressions/captures/` — bench-run artifacts, gitignored.

## Reproducing a filed regression

```shell
extras/bench/probes/regressions/run.sh 47
```

Options: `--secs N` (capture window, default 30), `--port /dev/cu.usbmodemXXX`
(default matches the current bench). See `REGISTRY.md` for what a
given issue's PASS/FAIL means.

## Adding a new regression

1. Guard the probe behavior in `RegressionHost.ino` under a
   `#if defined(...)` block. Name the derived guard
   `REGRESSION_<issue-number>_<slug>`.
2. Add a case clause in `regressions/run.sh` that maps the issue
   number to the required `-D` defines.
3. Add a section in `regressions/REGISTRY.md` following the existing
   format.
4. If the regression benefits from automatic PASS/FAIL analysis,
   drop a Python analyzer under `regressions/analyzers/` named
   `<issue>_<slug>.py`; the harness picks it up by filename glob.

Keep the baseline invariant: with no probe defines,
`RegressionHost.ino` must still compile and behave exactly like
`SimpleHost.ino`.
