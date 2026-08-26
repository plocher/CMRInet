#!/bin/sh
# Flash a host sketch, boot, capture all three witnesses, and print a verdict.
# Purpose: reproduce the reply-pair sniffer deafness for issue reporting.
#
# Usage:
#   flash_and_probe.sh [sketch] [host_port]
#
#   sketch    examples/ subdirectory name (default: XiaoHostTracer)
#             known: XiaoHostTracer, SimpleHost
#   host_port /dev/cu.usbmodem* for the Host board
#             (default: resolved from bench.json via the bench CLI)
#
# Prereqs:
#   - extras/bench/setup.sh has been run (creates .venv with pyserial)
#   - arduino-cli is installed with the esp32 core (XIAO_ESP32C6 FQBN)
#   - the two sniffers and dongle are wired to the bus (see README.md)
#
# Output: a VERDICT block a human or agent can read without file analysis:
#   - each witness: PASS (sees frames) or FAIL (silent/frozen)
#   - a one-line overall summary
set -e

BENCH_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$BENCH_DIR/../.." && pwd)"
LIBS_DIR="/Users/jplocher/Dropbox/Arduino/libraries"
SKETCH="${1:-XiaoHostTracer}"
if [ -n "$2" ]; then
    HOST_PORT="$2"
else
    HOST_PORT="$("$BENCH_DIR/bench" resolve --role Host)"
fi
FQBN="esp32:esp32:XIAO_ESP32C6"
BUILD_DIR="/tmp/${SKETCH}_build"
SKETCH_INO="$REPO_DIR/examples/$SKETCH/$SKETCH.ino"
VENV="$BENCH_DIR/.venv/bin/python"

# Library paths the compile step needs. The CMRInet library lives outside the
# default Arduino libraries dir; Adafruit libs are needed for OLED sketches.
LIBS="--library $REPO_DIR"
LIBS="$LIBS --library $LIBS_DIR/Adafruit_GFX_Library"
LIBS="$LIBS --library $LIBS_DIR/Adafruit_SSD1306"
LIBS="$LIBS --library $LIBS_DIR/Adafruit_BusIO"

if [ ! -x "$VENV" ]; then
    echo "ERROR: venv missing. Run: $BENCH_DIR/setup.sh" >&2
    exit 1
fi
if [ ! -f "$SKETCH_INO" ]; then
    echo "ERROR: sketch not found: $SKETCH_INO" >&2
    exit 1
fi

# Pre-flight: the sketch warning gate (issue #93). The ESP32 core suppresses
# warnings with a trailing -w, so a clean arduino-cli compile does not mean a
# sketch is warning-clean. Lint before flashing; `set -e` aborts on failure.
# Compile-only (no hardware, no flash) -- the gate never touches a board.
echo ">>> pre-flight: sketch warning gate ($SKETCH)"
python3 "$REPO_DIR/extras/sketch_lint.py" "$SKETCH"

echo ">>> [1/4] compile $SKETCH for $FQBN"
rm -rf "$BUILD_DIR"
arduino-cli compile --fqbn "$FQBN" $LIBS --build-path "$BUILD_DIR" "$SKETCH_INO" 2>&1 | tail -2

echo ">>> [2/4] upload to $HOST_PORT"
arduino-cli upload -p "$HOST_PORT" --fqbn "$FQBN" --input-dir "$BUILD_DIR" "$SKETCH_INO" 2>&1 | tail -2

echo ">>> [3/4] boot settle (6s)"
sleep 6

echo ">>> [4/4] three-witness capture (15s)"
# Capture probe output, print it, then pipe it to verdict.py for the verdict.
OUT=$("$VENV" "$BENCH_DIR/three.py")
printf '%s\n' "$OUT"

echo ""
echo "=================== VERDICT ($SKETCH) ==================="
printf '%s\n' "$OUT" | "$VENV" "$BENCH_DIR/verdict.py" "$SKETCH"
echo "======================================================"
