#!/usr/bin/env python3
"""gather_echo_probe — flash XiaoBenchEcho and capture the self-echo timeline.

Minimal Phase-A gather: resolve the bench Host, compile + upload the
library-free probe, request its epoch (identity + marker), then capture
N seconds of burst-event JSON lines into a timestamped results directory
beside a manifest. Pair with analyze_echo_probe.py.

Usage:
    gather_echo_probe.py [--port PORT] [--secs N] [--tag TAG] [--out DIR]
                         [--no-flash]
"""
from __future__ import annotations

import argparse
import datetime
import json
import re
import subprocess
import sys
import time
from pathlib import Path
from typing import Optional

BENCH_DIR = Path(__file__).resolve().parents[2]        # extras/bench
REPO_DIR = BENCH_DIR.parents[1]                         # CMRInet
sys.path.insert(0, str(BENCH_DIR))
import bench_ports  # noqa: E402

SKETCH = "XiaoBenchEcho"
FQBN = "esp32:esp32:XIAO_ESP32C6"
IMAGE = "xiao_bench_echo"
LIBS_DIR = "/Users/jplocher/Dropbox/Arduino/libraries"


def _resolve_host() -> str:
    """Resolve the bench Host role to a live device path."""
    cli = BENCH_DIR / "bench"
    try:
        out = subprocess.run(
            [str(cli), "resolve", "--role", "Host"],
            check=True, capture_output=True, text=True,
        ).stdout.strip()
        if out:
            return out
    except Exception:
        pass
    return bench_ports.resolve_or_exit("Host")


def _compile() -> None:
    """Compile the probe sketch to a /tmp build directory."""
    build = f"/tmp/{SKETCH}_build"
    sketch = REPO_DIR / "examples" / SKETCH / f"{SKETCH}.ino"
    print(f">>> compile {SKETCH}")
    subprocess.run(
        ["arduino-cli", "compile", "--fqbn", FQBN,
         "--library", str(REPO_DIR),
         "--library", f"{LIBS_DIR}/Adafruit_GFX_Library",
         "--library", f"{LIBS_DIR}/Adafruit_SSD1306",
         "--library", f"{LIBS_DIR}/Adafruit_BusIO",
         "--build-path", build, str(sketch)],
        check=True, cwd=str(REPO_DIR),
    )


def _upload(port: str) -> None:
    """Upload the compiled probe binary to the given port."""
    print(f">>> upload to {port}")
    subprocess.run(
        ["arduino-cli", "upload", "-p", port, "--fqbn", FQBN,
         "--input-dir", f"/tmp/{SKETCH}_build",
         str(REPO_DIR / "examples" / SKETCH / f"{SKETCH}.ino")],
        check=True, cwd=str(REPO_DIR),
    )


def _next_results_dir(root: Path, tag: str) -> Path:
    """Return a non-colliding timestamped output directory."""
    today = datetime.datetime.now().strftime("%Y%m%d")
    base = root / f"results.{today}.{tag}"
    candidate = base
    suffix = 1
    while candidate.exists():
        candidate = Path(f"{base}.{suffix}")
        suffix += 1
    return candidate


def _request_and_validate_epoch(ser, timeout_s: float = 8.0) -> Optional[dict]:
    """Ask the probe for its epoch line (any CDC byte) and validate image."""
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        ser.reset_input_buffer()
        ser.write(b"?\n")
        time.sleep(0.1)
        line = ser.readline().decode("utf-8", errors="replace").strip()
        if not line.startswith("{"):
            continue
        try:
            doc = json.loads(line)
        except json.JSONDecodeError:
            m = re.search(r'"image":"([^"]+)"', line)
            if not m or m.group(1) != IMAGE:
                continue
            return {"image": m.group(1)}
        if doc.get("e") == "epoch" and doc.get("image") == IMAGE:
            return doc
    return None


def _parse_marker(spec: str) -> list[int]:
    """Parse a space-separated hex marker spec into a byte list."""
    out: list[int] = []
    for tok in spec.split():
        if len(tok) not in (1, 2):
            raise ValueError(f"bad marker byte '{tok}' (expected 1-2 hex digits)")
        try:
            out.append(int(tok, 16))
        except ValueError:
            raise ValueError(f"bad marker byte '{tok}' (not hex)")
    if not out:
        raise ValueError("marker spec is empty")
    if len(out) > 16:
        raise ValueError(f"marker too long ({len(out)} bytes; max 16)")
    return out


def _set_marker(ser, marker: list[int], timeout_s: float = 6.0) -> Optional[dict]:
    """Send 'marker HH HH ...' and drain-read until the confirming epoch.

    The probe services CDC once per loop cycle, so the epoch it emits in
    reply is interleaved with burst-timeline lines. Send the command once,
    then keep readline-ing until the epoch with our marker arrives. Do NOT
    reset_input_buffer after sending — that discards the reply.
    """
    cmd = "marker " + " ".join(f"{b:02X}" for b in marker) + "\n"
    ser.write(cmd.encode("utf-8"))
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        line = ser.readline().decode("utf-8", errors="replace").strip()
        if not line.startswith("{"):
            continue
        try:
            doc = json.loads(line)
        except json.JSONDecodeError:
            continue
        if doc.get("e") == "epoch" and doc.get("image") == IMAGE:
            return doc
    return None


def _set_mode(ser, mode: str, timeout_s: float = 6.0) -> Optional[dict]:
    """Send 'mode deonly'/'mode normal' and drain-read until the confirming epoch.

    Same drain pattern as _set_marker: the epoch is interleaved with
    burst-timeline lines, so send once and read until the epoch with the
    requested mode arrives.
    """
    cmd = f"mode {mode}\n"
    ser.write(cmd.encode("utf-8"))
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        line = ser.readline().decode("utf-8", errors="replace").strip()
        if not line.startswith("{"):
            continue
        try:
            doc = json.loads(line)
        except json.JSONDecodeError:
            continue
        if (doc.get("e") == "epoch" and doc.get("image") == IMAGE
                and doc.get("mode") == mode):
            return doc
    return None


def main() -> int:
    """Flash the probe and capture a self-echo timeline."""
    p = argparse.ArgumentParser(description="Gather XiaoBenchEcho self-echo timeline")
    p.add_argument("--port", default=None,
                   help="Host device; default resolves from bench.json")
    p.add_argument("--secs", type=int, default=20, help="capture duration in seconds")
    p.add_argument("--tag", default="echo_probe")
    p.add_argument("--out", default="auto")
    p.add_argument("--no-flash", action="store_true",
                   help="skip compile/upload; assume the probe is already flashed")
    p.add_argument("--marker", default=None,
                   help="runtime marker as space-separated hex, e.g. '5A' "
                        "(single-byte deassertion test) or 'AA 55 AA 55'")
    p.add_argument("--mode", default=None, choices=("normal", "deonly"),
                   help="burst mode: 'normal' (write marker) or 'deonly' "
                        "(cycle TXEN with NO bytes written — DE-transition "
                        "isolation test)")
    args = p.parse_args()

    port = args.port or _resolve_host()

    if not args.no_flash:
        _compile()
        _upload(port)
        print(">>> boot settle (6s)")
        time.sleep(6)

    import serial
    ser = serial.Serial(port, 115200, timeout=0.2)
    epoch = _request_and_validate_epoch(ser)
    if not epoch:
        print(f"ERROR: did not see epoch image={IMAGE}", file=sys.stderr)
        ser.close()
        return 1
    print(f"Verified boot: {epoch.get('image')} v{epoch.get('version')} "
          f"marker={epoch.get('marker')}")

    if args.marker:
        try:
            marker_bytes = _parse_marker(args.marker)
        except ValueError as exc:
            print(f"ERROR: {exc}", file=sys.stderr)
            ser.close()
            return 2
        epoch = _set_marker(ser, marker_bytes)
        if not epoch:
            print(f"ERROR: marker set '{args.marker}' not confirmed", file=sys.stderr)
            ser.close()
            return 1
        print(f"Marker set: {epoch.get('marker')}")

    if args.mode:
        epoch = _set_mode(ser, args.mode)
        if not epoch:
            print(f"ERROR: mode set '{args.mode}' not confirmed", file=sys.stderr)
            ser.close()
            return 1
        print(f"Mode set: {epoch.get('mode')}")

    root = Path(__file__).resolve().parent / "data"
    out_dir = _next_results_dir(root, args.tag) if args.out == "auto" else Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    log_path = out_dir / f"{args.tag}.log"

    print(f">>> capture {args.secs}s")
    lines: list[str] = []
    deadline = time.time() + args.secs
    while time.time() < deadline:
        raw = ser.readline()
        if not raw:
            continue
        line = raw.decode("utf-8", errors="replace").strip()
        if line:
            lines.append(line)

    ser.close()
    log_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    manifest = {
        "scenario": "two_wire_echo_probe",
        "tag": args.tag,
        "port": port,
        "secs": args.secs,
        "capture_file": log_path.name,
        "image": IMAGE,
        "epoch": epoch,
        "requested_marker": args.marker,
        "requested_mode": args.mode,
        "timestamp": datetime.datetime.now().isoformat(),
    }
    (out_dir / "manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f">>> wrote {log_path} ({len(lines)} lines)")
    print(f">>> wrote {out_dir / 'manifest.json'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
