#!/bin/sh
# sweep_47.sh — grid-sweep the #47 backoff-under-loop-stall regression.
#
# Reproduces #47 across a (stall_ms × stall_period_ms) grid: for every
# combination it recompiles extras/bench/probes/RegressionHost with the
# matching -D defines, uploads to the Host board, captures the CDC trace,
# and runs analyzers/47_gap_deltas.py. Each combo's raw CDC capture and
# full run transcript are saved under sweep_results/ with value-named
# files, and a running summary.csv records the verdict and gap stats.
#
# This is a long HIL run: compile + upload + 6 s boot settle + N s
# capture per combo. With the default 13 x 10 = 130 grid at 30 s/capture
# that is roughly 90 minutes, unattended. The sweep is resumable: a combo
# whose transcript already exists is skipped unless --force is given, so
# an interrupt does not lose progress and a re-run does not redo it.
#
# Run it:
#   ./sweep_47.sh
#   ./sweep_47.sh --port /dev/cu.usbmodem282201 --secs 30
#   ./sweep_47.sh --stalls "25 50 100" --periods "150 300" --force
#
# Usage:
#   sweep_47.sh [--secs N] [--port /dev/cu.usbmodemXXX]
#              [--stalls "a b c"] [--periods "x y z"] [--force]
#
# Outputs (under extras/bench/probes/regressions/):
#   sweep_results/summary.csv         running verdict + gap stats
#   sweep_results/s${s}_p${p}.txt     full run.sh transcript per combo
#   sweep_results/s${s}_p${p}.log     raw CDC capture per combo
#   captures/47-s${s}_p${p}.log       raw CDC capture (run.sh's copy)
#

set -u

# ---- grid (edit here or override with --stalls / --periods) ----------
# Stall magnitudes to probe (ms) — the #47 investigation set.
STALLS="1 2 3 4 5 6 7 8 9 10 15 20 25 30 35 50 100 150 250 300 500 1000"
# Stall periods to probe (ms). The known trigger is 150; add fine
# resolution around it (140/160) and wider points to find the edges.
PERIODS="50 100 140 145 150 155 160 200 250 300 500 1000"

SECS=30
PORT="/dev/cu.usbmodem282201"
FORCE=0

# ---- args -----------------------------------------------------------
while [ $# -gt 0 ]; do
    case "$1" in
        --secs) SECS="$2"; shift 2 ;;
        --port) PORT="$2"; shift 2 ;;
        --stalls) STALLS="$2"; shift 2 ;;
        --periods) PERIODS="$2"; shift 2 ;;
        --force) FORCE=1; shift ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
RESULTS_DIR="$SCRIPT_DIR/sweep_results"
CAPTURES_DIR="$SCRIPT_DIR/captures"
RUN_SH="$SCRIPT_DIR/run.sh"

if [ ! -x "$RUN_SH" ]; then
    echo "ERROR: run.sh not found at $RUN_SH" >&2
    exit 1
fi
if [ ! -e "$PORT" ]; then
    echo "ERROR: port $PORT not found. Connected USB-CDC ports:" >&2
    ls -1 /dev/cu.usbmodem* 2>/dev/null >&2 || echo "  (none)" >&2
    exit 1
fi

mkdir -p "$RESULTS_DIR" "$CAPTURES_DIR"

SUMMARY="$RESULTS_DIR/summary.csv"
if [ ! -f "$SUMMARY" ]; then
    echo "stall_ms,period_ms,verdict,tx_events,max_gap_ms,median_gap_ms,p90_gap_ms,capture,transcript" \
        > "$SUMMARY"
fi

set -- $STALLS; n_stalls=$#
set -- $PERIODS; n_periods=$#
total=$((n_stalls * n_periods))

echo ">>> #47 grid sweep"
echo "    stalls  : $STALLS"
echo "    periods : $PERIODS"
echo "    combos  : $total"
echo "    secs    : $SECS"
echo "    port    : $PORT"
echo "    results : $RESULTS_DIR"
echo "    force   : $FORCE"
echo

i=0
for s in $STALLS; do
  for p in $PERIODS; do
    i=$((i + 1))
    tag="s${s}_p${p}"
    transcript="$RESULTS_DIR/${tag}.txt"
    capture="$CAPTURES_DIR/47-${tag}.log"

    if [ -f "$transcript" ] && [ "$FORCE" -eq 0 ]; then
        echo "[$i/$total] skip $tag (transcript exists; use --force to redo)"
        continue
    fi

    echo "[$i/$total] stall=${s}ms period=${p}ms ..."
    # run.sh exits non-zero when the analyzer reports FAIL (bug
    # reproduced) or on a compile/upload error. Either is expected during
    # a sweep, so do not let it abort the grid.
    "$RUN_SH" 47 \
        --stall-ms "$s" --stall-period-ms "$p" --tag "$tag" \
        --secs "$SECS" --port "$PORT" > "$transcript" 2>&1
    rc=$?

    # Keep a copy of the raw CDC capture beside the transcript so the
    # results directory is self-contained for later analysis.
    if [ -f "$capture" ]; then
        cp "$capture" "$RESULTS_DIR/${tag}.log"
    fi

    # ---- parse the analyzer output for the summary row ----------------
    if grep -q '^PASS: max gap' "$transcript"; then
        verdict=PASS
    elif grep -q '^FAIL: max gap' "$transcript"; then
        verdict=FAIL_STUCK
    elif grep -q '^FAIL: no DIAG_TRACE' "$transcript"; then
        verdict=FAIL_NO_TRACE
    elif grep -q '^FAIL: only' "$transcript"; then
        verdict=FAIL_TOO_FEW
    else
        verdict=ERROR
    fi
    tx=$(sed -n 's/^phantom UA *: *[0-9][0-9]* *(\([0-9][0-9]*\) TX events).*/\1/p' "$transcript" | head -1)
    max_gap=$(sed -n 's/^gaps (ms), max *: *\([0-9][0-9]*\).*/\1/p' "$transcript" | head -1)
    med_gap=$(sed -n 's/^gaps (ms), median *: *\([0-9][0-9]*\).*/\1/p' "$transcript" | head -1)
    p90_gap=$(sed -n 's/^gaps (ms), p90 *: *\([0-9][0-9]*\).*/\1/p' "$transcript" | head -1)

    echo "$s,$p,$verdict,${tx:-},${max_gap:-},${med_gap:-},${p90_gap:-},${tag}.log,${tag}.txt" \
        >> "$SUMMARY"
    echo "        rc=$rc verdict=$verdict max_gap=${max_gap:-?}ms (see ${tag}.txt)"
  done
done

echo
echo ">>> sweep complete: $SUMMARY"
awk -F, 'NR>1 {c[$3]++} END {for (v in c) printf "  %-14s %d\n", v, c[v]}' "$SUMMARY"
