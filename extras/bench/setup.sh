#!/bin/sh
# Create the bench probe venv inside the repo so it survives reboots.
# The venv is gitignored; this script recreates it from scratch.
# Idempotent: safe to re-run.
set -e

BENCH_DIR="$(cd "$(dirname "$0")" && pwd)"
VENV="$BENCH_DIR/.venv"

if [ -x "$VENV/bin/python" ]; then
    echo "venv already exists at $VENV"
else
    echo "creating venv at $VENV"
    python3 -m venv "$VENV"
fi

echo "installing pyserial"
"$VENV/bin/pip" install -q --upgrade pyserial

echo "verifying"
"$VENV/bin/python" -c "import serial; print('pyserial', serial.__version__, 'ready at', '$VENV')"

echo "done. Run probes with:"
echo "  $VENV/bin/python $BENCH_DIR/<script>.py"
