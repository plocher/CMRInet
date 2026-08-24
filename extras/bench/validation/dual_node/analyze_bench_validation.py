#!/usr/bin/env python3
"""Analyze dual-node bench validation captures."""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Optional

PKT_RE = re.compile(
    r"^PKT\s+t=(?P<t>\d+)\s+(?P<dir>TX|RX)\s+ua=(?P<ua>\d+)\s+mt=(?P<mt>[A-Z])"
)


@dataclass
class NodeEvidence:
    """Validation evidence for one node address."""

    address: int
    wire_ua: int
    tx_i_or_t: int = 0
    tx_polls: int = 0
    rx_replies: int = 0
    status_state: Optional[str] = None


@dataclass
class ValidationResult:
    """Top-level analyzer output."""

    pass_validation: bool
    ua_a: NodeEvidence
    ua_b: NodeEvidence
    failures: list[str]


def _wire_ua(address: int) -> int:
    """Convert node address to on-wire UA."""
    return address + 65


def _extract_node_states(status_snapshot: Optional[dict]) -> dict[int, str]:
    """Extract per-UA state from the status snapshot if present."""
    if not status_snapshot:
        return {}
    nodes = status_snapshot.get("nodes")
    if not isinstance(nodes, list):
        return {}
    out: dict[int, str] = {}
    for item in nodes:
        if not isinstance(item, dict):
            continue
        ua = item.get("ua")
        state = item.get("state")
        if isinstance(ua, int) and isinstance(state, str):
            out[ua] = state
    return out


def _analyze_lines(
    lines: list[str],
    ua_a: int,
    ua_b: int,
    status_snapshot: Optional[dict],
) -> ValidationResult:
    """Analyze capture lines for required initialization and poll/reply activity."""
    evidence = {
        ua_a: NodeEvidence(address=ua_a, wire_ua=_wire_ua(ua_a)),
        ua_b: NodeEvidence(address=ua_b, wire_ua=_wire_ua(ua_b)),
    }
    wire_to_address = {
        _wire_ua(ua_a): ua_a,
        _wire_ua(ua_b): ua_b,
    }

    for line in lines:
        match = PKT_RE.search(line)
        if not match:
            continue
        wire_ua = int(match.group("ua"))
        if wire_ua not in wire_to_address:
            continue
        addr = wire_to_address[wire_ua]
        node = evidence[addr]
        direction = match.group("dir")
        mt = match.group("mt")
        if direction == "TX":
            if mt in ("I", "T"):
                node.tx_i_or_t += 1
            if mt == "P":
                node.tx_polls += 1
        elif direction == "RX" and mt == "R":
            node.rx_replies += 1

    node_states = _extract_node_states(status_snapshot)
    for addr, node in evidence.items():
        node.status_state = node_states.get(addr)

    failures: list[str] = []
    for addr in (ua_a, ua_b):
        node = evidence[addr]
        if node.tx_i_or_t == 0:
            failures.append(f"UA{addr}: no TX I/T frames captured")
        if node.tx_polls == 0:
            failures.append(f"UA{addr}: no TX poll frames captured")
        if node.rx_replies == 0:
            failures.append(f"UA{addr}: no RX reply frames captured")
        if node.status_state in ("UNINITIALIZED", "OFFLINE"):
            failures.append(f"UA{addr}: status state is {node.status_state}")

    return ValidationResult(
        pass_validation=(len(failures) == 0),
        ua_a=evidence[ua_a],
        ua_b=evidence[ua_b],
        failures=failures,
    )


def _load_manifest(results_dir: Path) -> dict:
    """Load manifest.json from a gather output directory."""
    manifest_path = results_dir / "manifest.json"
    if not manifest_path.exists():
        raise FileNotFoundError(f"manifest not found: {manifest_path}")
    return json.loads(manifest_path.read_text(encoding="utf-8"))


def main() -> int:
    """Analyze one results directory or one capture file."""
    parser = argparse.ArgumentParser(description="Analyze dual-node bench validation")
    parser.add_argument("target", help="Results directory or capture log")
    parser.add_argument("--ua-a", type=int, default=30)
    parser.add_argument("--ua-b", type=int, default=31)
    parser.add_argument("--write-summary", action="store_true", help="Write summary.json for directory targets")
    args = parser.parse_args()

    target = Path(args.target)
    status_snapshot: Optional[dict] = None
    capture_path: Path
    ua_a = args.ua_a
    ua_b = args.ua_b
    summary_path: Optional[Path] = None

    if target.is_dir():
        manifest = _load_manifest(target)
        capture_name = manifest.get("capture_file")
        if not isinstance(capture_name, str):
            print("ERROR: manifest missing capture_file", file=sys.stderr)
            return 2
        capture_path = target / capture_name
        ua_a = int(manifest.get("ua_a", ua_a))
        ua_b = int(manifest.get("ua_b", ua_b))
        status_snapshot = manifest.get("status_snapshot")
        if args.write_summary:
            summary_path = target / "summary.json"
    else:
        capture_path = target

    if not capture_path.exists():
        print(f"ERROR: capture file not found: {capture_path}", file=sys.stderr)
        return 2

    lines = capture_path.read_text(encoding="utf-8", errors="replace").splitlines()
    result = _analyze_lines(lines, ua_a=ua_a, ua_b=ua_b, status_snapshot=status_snapshot)

    print(f"Capture: {capture_path}")
    print(f"UA{result.ua_a.address} (wire {result.ua_a.wire_ua}) -> I/T TX={result.ua_a.tx_i_or_t}, P TX={result.ua_a.tx_polls}, R RX={result.ua_a.rx_replies}, state={result.ua_a.status_state}")
    print(f"UA{result.ua_b.address} (wire {result.ua_b.wire_ua}) -> I/T TX={result.ua_b.tx_i_or_t}, P TX={result.ua_b.tx_polls}, R RX={result.ua_b.rx_replies}, state={result.ua_b.status_state}")
    if result.pass_validation:
        print("PASS: both nodes initialized and responded to polls in this capture")
    else:
        print("FAIL:")
        for failure in result.failures:
            print(f"  - {failure}")
    print("Manual check still required: visually confirm both node bitwalkers are operating.")

    if summary_path is not None:
        summary_path.write_text(
            json.dumps(asdict(result), indent=2) + "\n",
            encoding="utf-8",
        )
        print(f"Wrote summary: {summary_path}")

    return 0 if result.pass_validation else 1


if __name__ == "__main__":
    sys.exit(main())
