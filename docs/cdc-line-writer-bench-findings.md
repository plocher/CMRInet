# CDC line writer bench findings — 2026-08-26

A factual record of the on-bench validation for issue #99 (the
`writeCdcLine` extraction into `src/testbed/CdcLineWriter.h`). It
captures what the instruments showed. Project-private and experimental.

## What was under test

The `writeCdcLine` chunked-write logic, relocated verbatim from
`examples/TracerHost/TracerHost.ino` into
`src/testbed/CdcLineWriter.h` behind a `CdcConsole` seam. The desktop
tests in `tests/test_cdc_line.cpp` simulate a full buffer with a fake.
The bench question was whether the #86 fix — the terminator's room check
and reserved time slice — survived the extraction on the real USB CDC
stream, where `setTxTimeoutMs(0)` makes a write discard-and-return when
the ring is full.

## Setup

The `rosonway` bench (six roles, resolved from `extras/bench/bench.json`
by USB serial):

- Host — `/dev/cu.usbmodem41101` — `TracerHost` v0.9.0, feature
  branch `feature/issue-99-writecdcline-testable-header`
- Node 30, Node 31 — physical bench nodes
- Sniffer TX, Sniffer RX — `XiaoSniffer`, JSON lines over USB CDC
- Dongle — `/dev/cu.usbserial-BG04ID4L`, raw wire bytes at 28800 8N2

All six roles reported OK before the run (`extras/bench/bench list`).

## Test 1 — flash_and_probe.sh TracerHost (three-witness)

- Command: `extras/bench/flash_and_probe.sh TracerHost`
- Pre-flight sketch-lint: PASS
- Compile and upload: OK (340878 bytes, 26% flash)
- Boot settle: completed, no `setup()` hang

Three-witness capture over 15 s:

- Xiao #1 (poll pair): silent, `framesDecoded` +0
- Xiao #2 (reply pair): silent, `framesDecoded` +0
- Dongle (reply pair): 0 bytes

Verdict: `OVERALL: AMBIGUOUS — poll pair also silent; check bench
wiring/power.`

### Why this is not a #99 regression

The host never polled. `TracerHost.ino` defers `host.begin()` until
the first verb arrives (`lazyBegin()`), and `extras/bench/three.py`
captures only the two sniffers and the dongle — it never opens the Host
CDC port. With no CDC reader, no verb arrives, `lazyBegin()` never
fires, and the host stays off the bus. The silent witnesses are the
deferred-begin behavior, not the change under test.

Issue #99 touched only the CDC output writer (`writeCdcLine` →
`CdcLineWriter.h` + the `XiaoCdcConsole` adapter). It did not touch
`host.begin()`, `lazyBegin()`, the poll schedule, or the wire path. The
informative parts of this run passed: sketch-lint PASS, compile and
upload OK, boot settle with no hang, and the sniffers reported alive
(`image=xiao_sniffer`).

This same AMBIGUOUS verdict is the expected result for
`flash_and_probe.sh` against a deferred-begin sketch that no one drives
— it predates #99.

## Test 2 — CDC backpressure terminator (the #99 test)

- Command: `extras/bench/.venv/bin/python
  extras/bench/validation/cdc_line/gather_cdc_backpressure.py --secs 12
  --chunk 16 --sleep-ms 100`
- Script: `extras/bench/validation/cdc_line/gather_cdc_backpressure.py`

### Method

The script opens the Host CDC port (DTR/RTS asserted, so
`CdcConsole::open()` reads true), validates boot identity, then enables
fastwalker on UA30 to drive trace density. It does not send `run
<secs>`: the sketch's `ourOnTrace` routes packets to the RAM ring while
a run is active, not to CDC. With no run active, every poll's I/T/P/R
trace line flows through `writeCdcLine` to CDC — the path under test.

For 12 s it drains the CDC stream slowly (at most 16 bytes per 100 ms,
about 160 B/s) so the USB CDC ring fills. With `setTxTimeoutMs(0)`,
body writes discard-and-return when the ring is full; the terminator
must still land within its reserved 50 ms slice. It then sends `quit`
and fast-drains until the `final` line appears (adaptive, bounded by a
timeout) so the quit→final smoke is captured even under a large
backlog.

### Pass criteria

1. No merged records: no newline-delimited chunk carries more than one
   `seq` field, and no `}{"seq"` adjacency (a closed object glued to
   the next record's opening with no newline between). Two records on
   one line is the missing-terminator signature. Hard gate.
2. Non-vacuous traffic: at least 100 `seq` fields captured.
3. Backpressure was real: either body truncation observed, or the
   fast-drain backlog ratio (tail/slow) clears 3.0. Dropped body bytes
   are gone and unobservable, so truncation is a weak signal; a large
   tail backlog proves the buffer was congested when the records were
   written, so "0 merges" is not vacuous.
4. The `final` line arrived newline-terminated.

### Result

```
total_bytes            407937
slow_bytes             1792
tail_bytes             406145
backlog_ratio          226.64
newline_count          1810
seq_total              1810
valid_records          1810
truncated_terminated   0
noise_chunks           0
merged_chunks          0
glued_extra_records    0
adjacency_glues        0
final_seen             True
final_terminated       True
VERDICT: PASS
```

Verdict: `PASS`. Under a 227x backlog (1.8 KB drained in the 12 s slow
window vs 406 KB recovered in the tail), all 1810 records came through
as valid, newline-delimited JSON with zero merges. The reserved-
terminator logic survived the extraction on real hardware. The `final`
line arrived newline-terminated through the new `writeCdcLineCb` →
`XiaoCdcConsole` → bound-shell path (the quit→final smoke).

Artifacts: `extras/bench/validation/cdc_line/data/results.20260826.
cdc_backpressure.2/` (`.raw` and `summary.json`; the `data/` tree is
gitignored).

## The honest gap

`truncated_terminated = 0`. The ESP32 HWCDC layer absorbs backlog by
dropping at the ring rather than truncating a body mid-write in a way
the reader can observe, so this run did not produce a visible
body-truncation event. The 227x backlog ratio is the observable proof
the congested regime was reached, so the merge gate is not vacuous —
but the specific full-buffer body-drop path that the desktop fake
simulates (`test_full_buffer_terminator_still_lands`) was not
positively observed on hardware. The merge gate itself — the actual
#86 defect — is unambiguous: zero merged records under heavy
backpressure.

## How to reproduce

One-time setup: create the bench venv (`extras/bench/setup.sh`); the
bench roles must be wired and resolvable (`extras/bench/bench list`).

From the repo root, on the `feature/issue-99-writecdcline-testable-
header` branch:

```shell
# The #99 test: CDC terminator under backpressure.
extras/bench/.venv/bin/python \
  extras/bench/validation/cdc_line/gather_cdc_backpressure.py \
  --secs 12 --chunk 16 --sleep-ms 100

# Expected: VERDICT: PASS, 0 merged records, backlog_ratio well above
# 3.0, final_seen=True.
```

The `--chunk` and `--sleep-ms` flags set the drain rate. A slower drain
(higher `--sleep-ms` or smaller `--chunk`) raises the backlog. If the
verdict is INCONCLUSIVE, the buffer never filled; re-run with a slower
drain.
