#!/bin/sh
# Flash the calibration sketch and capture its CDC trace output.
#
# Purpose: verify 2-wire wiring and tooling end to end. The Host
# TXs I -> T -> P to one node and the onTrace listener reports
# every frame it sees on RX. On 2-wire, the Host sees its own
# TX echoed back, so RX trace lines mirror the TX lines. On 4-wire,
# only TX lines appear (no echo).
#
# Usage:
#   calibrate.sh [host_port]
#
#   host_port  /dev/cu.usbmodem* for the Host board
#              (default: resolved from bench.json via the bench CLI)
#
# Prereqs:
#   - extras/bench/setup.sh has been run (creates .venv with pyserial)
#   - arduino-cli is installed with the esp32 core (XIAO_ESP32C6 FQBN)
#   - the RS485 adapter is wired to the Host board (2-wire or 4-wire)
#
# Output: the CDC trace stream — JSON lines the operator reads to
# confirm wiring and tooling:
#   - epoch line (image identity)
#   - trace lines (TX I/T/P, and on 2-wire their RX echoes)
#
# On 2-wire, the RX trace lines mirror the TX lines (self-echo).
# On 4-wire, only TX trace lines appear (no echo on RX).
set -e

BENCH_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$BENCH_DIR/../.." && pwd)"
LIBS_DIR="/Users/jplocher/Dropbox/Arduino/libraries"
SKETCH="XiaoBenchCal"
if [ -n "$1" ]; then
    HOST_PORT="$1"
else
    HOST_PORT="$("$BENCH_DIR/bench" resolve --role Host)"
fi
FQBN="esp32:esp32:XIAO_ESP32C6"
BUILD_DIR="/tmp/${SKETCH}_build"
SKETCH_INO="$REPO_DIR/examples/$SKETCH/$SKETCH.ino"
VENV="$BENCH_DIR/.venv/bin/python"

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

# Pre-flight: the sketch warning gate (issue #93).
echo ">>> pre-flight: sketch warning gate ($SKETCH)"
python3 "$REPO_DIR/extras/sketch_lint.py" "$SKETCH"

echo ">>> [1/3] compile $SKETCH for $FQBN"
rm -rf "$BUILD_DIR"
arduino-cli compile --fqbn "$FQBN" $LIBS --build-path "$BUILD_DIR" "$SKETCH_INO" 2>&1 | tail -2

echo ">>> [2/3] upload to $HOST_PORT"
arduino-cli upload -p "$HOST_PORT" --fqbn "$FQBN" --input-dir "$BUILD_DIR" "$SKETCH_INO" 2>&1 | tail -2

echo ">>> [3/3] boot settle (6s) and CDC capture (15s)"
sleep 6

# Capture the CDC trace stream. On 2-wire, RX lines mirror TX lines.
# On 4-wire, only TX lines appear. Either way, the output
# confirms the wiring and tooling work.
"$VENV" "$BENCH_DIR/tracer_dongle.py" "$HOST_PORT"
