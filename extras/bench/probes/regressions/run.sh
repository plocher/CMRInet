#!/bin/sh
# run.sh — reproduce a filed regression end to end.
#
# Usage:
#   run.sh <issue-number> [--secs N] [--port /dev/cu.usbmodemXXX]
#
# Given an issue number listed in REGISTRY.md, this script:
#   1. Compiles extras/bench/probes/RegressionHost/RegressionHost.ino
#      with the -D defines that activate the probe for that issue.
#   2. Uploads the build to the Host board.
#   3. Captures the USB CDC stream for --secs seconds (default 30) into
#      captures/<issue>-<timestamp>.log.
#   4. If analyzers/<issue>_*.py exists, runs it against the capture and
#      prints its PASS/FAIL summary. Otherwise prints where the capture
#      landed and a reminder to interpret manually per REGISTRY.md.
#
# The registry of known regressions is a `case` block below. Adding a
# regression means adding a case clause here plus a section in
# REGISTRY.md; the .ino gains a new `#if defined(...)` guard for its
# probe behavior.
#
# Precedent: extras/bench/flash_and_probe.sh, whose LIBS assembly and
# arduino-cli invocation this script follows.
set -e

REGRESSIONS_DIR="$(cd "$(dirname "$0")" && pwd)"
PROBES_DIR="$(cd "$REGRESSIONS_DIR/.." && pwd)"
BENCH_DIR="$(cd "$PROBES_DIR/.." && pwd)"
REPO_DIR="$(cd "$BENCH_DIR/../.." && pwd)"
LIBS_DIR="/Users/jplocher/Dropbox/Arduino/libraries"

SKETCH_DIR="$PROBES_DIR/RegressionHost"
SKETCH_INO="$SKETCH_DIR/RegressionHost.ino"
FQBN="esp32:esp32:XIAO_ESP32C6"
BUILD_DIR="/tmp/RegressionHost_build"
VENV="$BENCH_DIR/.venv/bin/python"
CAPTURES_DIR="$REGRESSIONS_DIR/captures"

# ---- args
if [ $# -lt 1 ]; then
    echo "usage: $0 <issue-number> [--secs N] [--port /dev/cu.usbmodemXXX]" >&2
    echo "       [--stall-ms N] [--stall-period-ms N] [--tag STRING]" >&2
    exit 2
fi
ISSUE="$1"
shift
SECS=30
PORT="/dev/cu.usbmodem282201"
# Per-issue probe overrides (only meaningful for #47 today). Empty
# unless set on the CLI; the case block applies issue-specific defaults.
STALL_MS=""
STALL_PERIOD_MS=""
TAG=""
while [ $# -gt 0 ]; do
    case "$1" in
        --secs) SECS="$2"; shift 2 ;;
        --port) PORT="$2"; shift 2 ;;
        --stall-ms) STALL_MS="$2"; shift 2 ;;
        --stall-period-ms) STALL_PERIOD_MS="$2"; shift 2 ;;
        --tag) TAG="$2"; shift 2 ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done

# ---- issue → defines
# One clause per regression. Keep in sync with REGISTRY.md.
# Per-issue defaults are set here; CLI overrides (--stall-ms etc.)
# apply on top so the harness can sweep a grid without editing source.
case "$ISSUE" in
    47)
        : "${STALL_MS:=25}"
        : "${STALL_PERIOD_MS:=150}"
        DEFINES="-DDIAG_FAKE_STALL_MS=$STALL_MS -DDIAG_FAKE_STALL_PERIOD_MS=$STALL_PERIOD_MS -DDIAG_TRACE"
        ;;
    *)
        echo "ERROR: no registry entry for issue #$ISSUE" >&2
        echo "  known issues: 47" >&2
        echo "  add one: edit run.sh (case block) and REGISTRY.md" >&2
        exit 2
        ;;
esac

# ---- preflight
if [ ! -x "$VENV" ]; then
    echo "ERROR: venv missing. Run: $BENCH_DIR/setup.sh" >&2
    exit 1
fi
if [ ! -f "$SKETCH_INO" ]; then
    echo "ERROR: sketch not found: $SKETCH_INO" >&2
    exit 1
fi

# The compile-time defines must ride on `build.defines`, NOT on
# `compiler.cpp.extra_flags`, on the esp32 core. The extra_flags path
# clobbers `-DARDUINO_USB_CDC_ON_BOOT=1` and USB CDC never enumerates.
BUILD_PROPS="build.defines=$DEFINES"

LIBS="--library $REPO_DIR"
LIBS="$LIBS --library $LIBS_DIR/Adafruit_GFX_Library"
LIBS="$LIBS --library $LIBS_DIR/Adafruit_SSD1306"
LIBS="$LIBS --library $LIBS_DIR/Adafruit_BusIO"

mkdir -p "$CAPTURES_DIR"
if [ -n "$TAG" ]; then
    SUFFIX="$TAG"
else
    SUFFIX="$(date +%Y%m%d-%H%M%S)"
fi
CAPTURE="$CAPTURES_DIR/${ISSUE}-${SUFFIX}.log"

echo ">>> regression #$ISSUE"
echo "    defines : $DEFINES"
echo "    port    : $PORT"
echo "    capture : $CAPTURE"
echo "    secs    : $SECS"

echo ">>> [1/4] compile RegressionHost"
rm -rf "$BUILD_DIR"
COMPILE_LOG="/tmp/RegressionHost_compile.log"
if ! arduino-cli compile \
    --fqbn "$FQBN" \
    $LIBS \
    --build-property "$BUILD_PROPS" \
    --build-path "$BUILD_DIR" \
    "$SKETCH_INO" > "$COMPILE_LOG" 2>&1; then
    echo "ERROR: compile failed (last 20 lines of $COMPILE_LOG):" >&2
    tail -20 "$COMPILE_LOG" >&2
    exit 1
fi
tail -2 "$COMPILE_LOG"

echo ">>> [2/4] upload to $PORT"
UPLOAD_LOG="/tmp/RegressionHost_upload.log"
if ! arduino-cli upload -p "$PORT" --fqbn "$FQBN" --input-dir "$BUILD_DIR" \
    "$SKETCH_INO" > "$UPLOAD_LOG" 2>&1; then
    echo "ERROR: upload failed (last 20 lines of $UPLOAD_LOG):" >&2
    tail -20 "$UPLOAD_LOG" >&2
    exit 1
fi
tail -2 "$UPLOAD_LOG"

echo ">>> [3/4] boot settle (6s), then capture ${SECS}s"
sleep 6
"$VENV" - "$PORT" "$SECS" "$CAPTURE" <<'PY'
import serial, sys, time
port, secs, out = sys.argv[1], float(sys.argv[2]), sys.argv[3]
s = serial.Serial(port, 115200, timeout=0.5)
deadline = time.time() + secs
with open(out, "w") as f:
    while time.time() < deadline:
        chunk = s.read(4096)
        if chunk:
            f.write(chunk.decode(errors="replace"))
            f.flush()
PY

echo ">>> [4/4] analyze"
ANALYZER=$(ls "$REGRESSIONS_DIR"/analyzers/${ISSUE}_*.py 2>/dev/null | head -1 || true)
if [ -n "$ANALYZER" ]; then
    echo "    running: $(basename "$ANALYZER")"
    "$VENV" "$ANALYZER" "$CAPTURE"
else
    echo "    no analyzer for #$ISSUE. Interpret $CAPTURE per REGISTRY.md."
fi
