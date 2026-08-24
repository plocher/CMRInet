#!/usr/bin/env python3
"""Opt-in hardware test for the bench resolver (issue #68).

Power-cycles one bench board's hub port with uhubctl and asserts the
resolver loses the role while the port is off and finds it again after
power-on — the disappear/reappear half of the discovery contract. (The
name-shuffle half is covered by the CLI functional suite; a power cycle
keeps the board on the same hub port, so the location-derived /dev name
cannot shuffle here.)

Never runs in CI. Requires uhubctl (`brew install uhubctl`) and a bench
wired through a per-port-switchable hub (the ROSONWAY RSH-A10). The target
board is HARD POWER CYCLED — pick an idle role.

Usage (from extras/bench/):
    tests/../.venv/bin/python tests/uhubctl_cycle_test.py [--role Sniffer --id TX]

Exit codes: 0 pass (or uhubctl absent -> SKIP, still 0), 1 fail,
2/3 resolver config/missing errors at baseline.
"""
import argparse
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import bench_ports

ENUMERATE_TIMEOUT_S = 20.0


def uhubctl_target(location: str) -> tuple[str, str]:
    """Map a USB location path to the uhubctl (-l hub, -p port) pair."""
    hub, dot, port = location.rpartition(".")
    if not dot:
        raise SystemExit(
            f"location '{location}' has no hub component; "
            "pick a role behind the switchable hub"
        )
    return hub, port


def run_uhubctl(hub: str, port: str, action: str) -> None:
    result = subprocess.run(
        ["uhubctl", "-l", hub, "-p", port, "-a", action],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        raise SystemExit(
            f"uhubctl -l {hub} -p {port} -a {action} failed: "
            f"{result.stderr.strip() or result.stdout.strip()}"
        )


def resolved(role: str, role_id: str | None):
    """Return the BenchDevice, or None when the role does not resolve."""
    try:
        return bench_ports.usb_resolve(role, role_id)
    except bench_ports.BenchError:
        return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--role", default="Sniffer")
    parser.add_argument("--id", dest="role_id", default="TX")
    args = parser.parse_args()

    if shutil.which("uhubctl") is None:
        print("SKIP: uhubctl not installed (brew install uhubctl)")
        return 0

    baseline = resolved(args.role, args.role_id)
    if baseline is None:
        print(f"FAIL: {args.role}/{args.role_id} does not resolve at baseline")
        return 1
    print(f"baseline: {args.role}/{args.role_id} at {baseline.device} "
          f"(location {baseline.live_location})")
    hub, port = uhubctl_target(baseline.live_location)

    failed = False
    try:
        print(f"powering off hub {hub} port {port}...")
        run_uhubctl(hub, port, "off")
        deadline = time.time() + ENUMERATE_TIMEOUT_S
        while resolved(args.role, args.role_id) is not None \
                and time.time() < deadline:
            time.sleep(0.25)
        if resolved(args.role, args.role_id) is not None:
            print("FAIL: role still resolves with the port powered off")
            failed = True
        else:
            print("ok: role does not resolve while the port is off")

        print("powering on...")
        run_uhubctl(hub, port, "on")
        deadline = time.time() + ENUMERATE_TIMEOUT_S
        back = None
        while back is None and time.time() < deadline:
            time.sleep(0.5)
            back = resolved(args.role, args.role_id)
        if back is None:
            print("FAIL: role did not resolve after power-on")
            failed = True
        else:
            print(f"ok: role resolves again at {back.device}")
            if back.serial.lower() != baseline.serial.lower():
                print("FAIL: serial changed across the power cycle")
                failed = True
    finally:
        # Best effort: never leave a bench board powered off.
        run_uhubctl(hub, port, "on")

    print("PASS" if not failed else "FAIL")
    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(main())
