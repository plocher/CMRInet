#!/usr/bin/env python3
"""gather_echo_cancel — Phase B echo-cancel characterization capture.

Flashes XiaoBenchEchoCancel, toggles echo-cancel over CDC, runs a bounded
capture against the phantom UA, dumps the ring, and reads host/node status.
Produces one results dir per (mode, run). Pair with analyze_echo_cancel.py.

Two scenarios, one variable each (no reflash between them):
  B1 (cancel OFF): --echocancel off --tag echo_cancel_off
  B2 (cancel ON):  --echocancel on  --tag echo_cancel_on

Usage:
    gather_echo_cancel.py [--echocancel on|off] [--secs N] [--tag T]
                          [--port PORT] [--no-flash]
"""
from __future__ import annotations

import argparse
import datetime
import json
import subprocess
import sys
import time
from pathlib import Path
from typing import Optional

BENCH_DIR = Path(__file__).resolve().parents[2]        # extras/bench
REPO_DIR = BENCH_DIR.parents[1]                         # CMRInet
sys.path.insert(0, str(BENCH_DIR / "probes" / "Issue47"))
import _tracer_client  # noqa: E402

SKETCH = "XiaoBenchEchoCancel"
FQBN = "esp32:esp32:XIAO_ESP32C6"
IMAGE = "xiao_bench_echo_cancel"
LIBS_DIR = "/Users/jplocher/Dropbox/Arduino/libraries"


def _compile() -> None:
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
    print(f">>> upload to {port}")
    subprocess.run(
        ["arduino-cli", "upload", "-p", port, "--fqbn", FQBN,
         "--input-dir", f"/tmp/{SKETCH}_build",
         str(REPO_DIR / "examples" / SKETCH / f"{SKETCH}.ino")],
        check=True, cwd=str(REPO_DIR),
    )


def _next_results_dir(root: Path, tag: str) -> Path:
    today = datetime.datetime.now().strftime("%Y%m%d")
    base = root / f"results.{today}.{tag}"
    candidate = base
    suffix = 1
    while candidate.exists():
        candidate = Path(f"{base}.{suffix}")
        suffix += 1
    return candidate


def _await_line_prefix(ser, prefix: str, timeout_s: float = 15.0) -> Optional[str]:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        line = ser.readline().decode("utf-8", errors="replace").strip()
        if line.startswith(prefix):
            return line
    return None


def _validate_boot(ser, timeout_s: float = 15.0) -> bool:
    """Send `status` and confirm the probe image (not the shared tracer image).

    The shared _tracer_client.sync_and_validate_boot hard-codes the expected
    image as 'xiao_host_tracer'; this probe is a different image, so it
    needs its own identity check.
    """
    import re
    ser.write(b"status\n")
    time.sleep(0.5)
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        line = ser.readline().decode("utf-8", errors="replace").strip()
        if not line:
            continue
        if not line.startswith("{"):
            continue
        try:
            doc = json.loads(line)
        except json.JSONDecodeError:
            if '"image":"' + IMAGE + '"' not in line:
                continue
            print(f"ERROR: Expected image '{IMAGE}', got a truncated line.",
                  file=sys.stderr)
            return False
        image = doc.get("image")
        if image == IMAGE:
            print(f"Verified boot: {image} v{doc.get('version')}")
            return True
        print(f"ERROR: Expected image '{IMAGE}', got '{image}'. Check flash.",
              file=sys.stderr)
        return False
    return False


def _read_status_snapshot(ser, timeout_s: float = 3.0) -> Optional[dict]:
    ser.write(b"status\n")
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        line = ser.readline().decode("utf-8", errors="replace").strip()
        if not line.startswith("{"):
            continue
        try:
            doc = json.loads(line)
        except json.JSONDecodeError:
            continue
        if doc.get("event") == "status":
            return doc
    return None


def _collect_dump_lines(ser, timeout_s: float = 15.0) -> list[str]:
    ser.write(b"dump\n")
    deadline = time.time() + timeout_s
    in_dump = False
    dump_lines: list[str] = []
    while time.time() < deadline:
        line = ser.readline().decode("utf-8", errors="replace").strip()
        if not line:
            continue
        if line.startswith("BEGIN DUMP"):
            in_dump = True
            continue
        if line == "END DUMP":
            return dump_lines
        if in_dump:
            dump_lines.append(line)
    return dump_lines


def main() -> int:
    p = argparse.ArgumentParser(description="Phase B echo-cancel gather")
    p.add_argument("--echocancel", choices=("on", "off", "auto"), default="on",
                   help="echo-cancel state for this capture (the one variable): "
                        "on=AlwaysOn, off=AlwaysOff, auto=Auto (default)")
    p.add_argument("--secs", type=int, default=20, help="capture run time in seconds")
    p.add_argument("--tag", default=None,
                   help="results tag; default echo_cancel_{on|off}")
    p.add_argument("--port", default=None,
                   help="Host device; default resolves from bench.json")
    p.add_argument("--no-flash", action="store_true",
                   help="skip compile/upload; assume the probe is already flashed")
    p.add_argument("--ua", type=int, default=32, help="phantom node UA")
    p.add_argument("--node", default=None,
                   help="real node to poll instead of the phantom: "
                        "'UA IN OUT' (e.g. '30 7 7'). Disables the "
                        "compiled-in phantom UA and adds+enables the real node.")
    args = p.parse_args()

    tag = args.tag or f"echo_cancel_{args.echocancel}"
    port = args.port or _tracer_client.host_port()

    if not args.no_flash:
        _compile()
        _upload(port)
        print(">>> boot settle (6s)")
        time.sleep(6)

    ser = _tracer_client.reboot_and_reconnect(port)
    cleanup_ok = True
    try:
        if not _validate_boot(ser):
            print("ERROR: boot validation failed", file=sys.stderr)
            return 1

        # Force begin() with the compiled-in phantom, then configure the
        # node topology. If --node is given, disable the phantom and
        # add+enable the real node with its geometry.
        _tracer_client.send_command(ser, f"node enable {args.ua}")
        _tracer_client.flush_lines(ser)

        if args.node:
            parts = args.node.split()
            if len(parts) != 3:
                print(f"ERROR: --node expects 'UA IN OUT', got '{args.node}'",
                      file=sys.stderr)
                return 1
            node_ua, node_in, node_out = parts
            _tracer_client.send_command(ser, f"node disable {args.ua}")
            _tracer_client.flush_lines(ser)
            _tracer_client.send_command(
                ser, f"node add {node_ua} {node_in} {node_out}")
            _tracer_client.flush_lines(ser)
            _tracer_client.send_command(ser, f"node enable {node_ua}")
            _tracer_client.flush_lines(ser)
            active_ua = int(node_ua)
            print(f"Real node: UA={node_ua} NI={node_in} NO={node_out}")
        else:
            active_ua = args.ua

        # Toggle echo-cancel (the one variable). Must be after begin(): the
        # probe's lazyBegin fires on the first node/traffic verb, and
        # begin() resets the flag to true. Sending 'node enable' first
        # forces begin(), then 'echocancel' sets the desired state.
        _tracer_client.send_command(ser, f"echocancel {args.echocancel}")
        ec_line = _await_line_prefix(ser, '{"event":"echocancel"', timeout_s=4.0)
        if not ec_line:
            print(f"ERROR: echocancel {args.echocancel} not confirmed",
                  file=sys.stderr)
            return 1
        print(f"Echo-cancel: {ec_line}")
        _tracer_client.flush_lines(ser)

        print(f">>> run {args.secs}s")
        _tracer_client.send_command(ser, f"run {args.secs}")
        if not _await_line_prefix(ser, "END CAPTURE", timeout_s=args.secs + 5.0):
            print("ERROR: END CAPTURE not seen", file=sys.stderr)
            return 1

        # Quiesce + dump (the tracer-client pattern).
        _tracer_client.send_command(ser, f"node disable {active_ua}")
        _tracer_client.flush_lines(ser)
        dump_lines = _collect_dump_lines(ser)
        if not dump_lines:
            print("ERROR: no dump lines captured", file=sys.stderr)
            return 1

        status_snapshot = _read_status_snapshot(ser)

        root = Path(__file__).resolve().parent / "data"
        out_dir = _next_results_dir(root, tag)
        out_dir.mkdir(parents=True, exist_ok=True)
        log_path = out_dir / f"{tag}.log"
        with log_path.open("w", encoding="utf-8") as h:
            h.write("# CMRI Phase B Echo-Cancel Capture\n")
            h.write(f"# echocancel: {args.echocancel}\n")
            h.write(f"# ua: {args.ua}\n")
            h.write(f"# secs: {args.secs}\n")
            h.write(f"# timestamp: {datetime.datetime.now().isoformat()}\n")
            for line in dump_lines:
                h.write(line + "\n")
        print(f"Wrote capture: {log_path}")

        manifest = {
            "scenario": "two_wire_echo_cancel",
            "tag": tag,
            "port": port,
            "secs": args.secs,
            "echocancel": args.echocancel,
            "ua": args.ua,
            "node": args.node,
            "capture_file": log_path.name,
            "image": IMAGE,
            "status_snapshot": status_snapshot,
            "timestamp": datetime.datetime.now().isoformat(),
        }
        (out_dir / "manifest.json").write_text(
            json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        print(f"Wrote manifest: {out_dir / 'manifest.json'}")
    finally:
        cleanup_ok = _tracer_client.shutdown_and_verify_quiet(
            ser, node_addresses=(args.ua,))
        try:
            ser.close()
        except Exception:
            pass
    return 0 if cleanup_ok else 1


if __name__ == "__main__":
    sys.exit(main())
