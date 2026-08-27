#!/usr/bin/env python3
"""calibrate — flash XiaoBenchCal, validate boot, capture CDC trace.

Minimal 2-wire bench calibration: flash the calibration
sketch, confirm the image identity over CDC, let the engine run
its I -> T -> P poll cycle, and capture the CDC trace stream.
On 2-wire, RX trace lines mirror the TX lines (self-echo).
On 4-wire, only TX trace lines appear (no echo on RX).

Usage:
    calibrate [--port PORT] [--secs SECS] [--ua UA]

    --port   Host device; default resolves from bench.json
    --secs    capture duration in seconds (default 15)
    --ua     calibration node UA (default 30)

Prereqs:
    - extras/bench/setup.sh has been run (creates .venv with pyserial)
    - arduino-cli is installed with the esp32 core (XIAO_ESP32C6 FQBN)
    - the RS485 adapter is wired to the Host board (2-wire or 4-wire)

Output: the CDC trace stream — JSON lines the operator reads to
confirm wiring and tooling:
    - epoch line (image identity)
    - trace lines (TX I/T/P, and on 2-wire their RX echoes)
"""
from __future__ import annotations

import argparse
import serial
import sys
import time
import threading
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "probes" / "Issue47"))
import _tracer_client

SKETCH = "XiaoBenchCal"
FQBN = "esp32:esp32:XIAO_ESP32C6"
LIBS_DIR = "/Users/jplocher/Dropbox/Arduino/libraries"
REPO_DIR = Path(__file__).resolve().parents[2]


def _compile() -> None:
    import subprocess

    libs = [
        "--library", str(REPO_DIR),
        "--library", f"{LIBS_DIR}/Adafruit_GFX_Library",
        "--library", f"{LIBS_DIR}/Adafruit_SSD1306",
        "--library", f"{LIBS_DIR}/Adafruit_BusIO",
    ]
    build_dir = f"/tmp/{SKETCH}_build"
    sketch = REPO_DIR / "examples" / SKETCH / f"{SKETCH}.ino"
    print(f">>> compile {SKETCH}")
    subprocess.run(
        ["arduino-cli", "compile", "--fqbn", FQBN, *libs,
         "--build-path", build_dir, str(sketch)],
        check=True,
        cwd=str(REPO_DIR),
    )


def _capture_cdc(ser: serial.Serial, secs: int) -> list[str]:
    """Capture CDC trace lines for `secs` seconds."""
    lines: list[str] = []
    deadline = time.time() + secs
    while time.time() < deadline:
        raw = ser.readline()
        if not raw:
            continue
        line = raw.decode("utf-8", errors="replace").strip()
        if line:
            lines.append(line)
    return lines


def main() -> int:
    parser = argparse.ArgumentParser(description="2-wire bench calibration")
    parser.add_argument("--port", default=None,
                        help="Host device; default resolves from bench.json")
    parser.add_argument("--secs", type=int, default=15,
                        help="capture duration in seconds")
    parser.add_argument("--ua", type=int, default=30,
                        help="calibration node UA")
    args = parser.parse_args()

    port = args.port or _tracer_client.host_port()

    # Pre-flight: the sketch warning gate (issue #93).
    print(f">>> pre-flight: sketch warning gate ({SKETCH})")
    import subprocess
    subprocess.run(
        ["python3", str(REPO_DIR / "extras" / "sketch_lint.py"), SKETCH],
        check=True,
        cwd=str(REPO_DIR),
    )

    _compile()

    print(f">>> upload to {port}")
    subprocess.run(
        ["arduino-cli", "upload", "-p", port, "--fqbn", FQBN,
         "--input-dir", f"/tmp/{SKETCH}_build",
         str(REPO_DIR / "examples" / SKETCH / f"{SKETCH}.ino")],
        check=True,
        cwd=str(REPO_DIR),
    )

    print(">>> boot settle (6s)")
    time.sleep(6)

    ser = _tracer_client.reboot_and_reconnect(port)
    if not _tracer_client.sync_and_validate_boot(ser):
        print("ERROR: boot validation failed", file=sys.stderr)
        return 1

    # Configure the calibration node via C&C (the shell holds no
    # compiled-in nodes, so add it at runtime).
    _tracer_client.send_command(ser, f"node add {args.ua} 2 2")
    _tracer_client.flush_lines(ser)

    print(f">>> capture {args.secs}s")
    try:
        ser = serial.Serial(port, 115200, timeout=0.1)
        lines = _capture_cdc(ser, args.secs)
    finally:
        _tracer_client.flush_lines(ser)
        ser.close()

    if not lines:
        print("ERROR: no CDC trace lines captured", file=sys.stderr)
        return 1

    out = Path("data") / f"calibrate.{int(time.time())}.log"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f">>> wrote {out} ({len(lines)} lines)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
