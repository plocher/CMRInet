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
    r"^PKT\s+t=(?P<t>\d+)\s+(?P<dir>TX|RX)\s+[uU][aA]=(?P<ua>\d+)\s+mt=(?P<mt>[A-Z])"
)


@dataclass
class NodeEvidence:
    """Validation evidence for one node UA."""

    ua: int
    expected_input_bytes: Optional[int] = None
    tx_i_or_t: int = 0
    tx_polls: int = 0
    rx_replies: int = 0
    status_state: Optional[str] = None
    accepted_exchanges: Optional[int] = None
    observed_input_bytes: Optional[int] = None
    last_fault_name: Optional[str] = None
    last_fault_expected: Optional[int] = None
    last_fault_observed: Optional[int] = None


@dataclass
class ValidationResult:
    """Top-level analyzer output."""

    pass_validation: bool
    ua_a: NodeEvidence
    ua_b: NodeEvidence
    failures: list[str]


def _extract_node_states(status_snapshot: Optional[dict]) -> tuple[dict[int, str], list[str]]:
    """Extract per-UA state from status snapshot and return validation failures.

    The live table is `roster` (a list). `nodes` is the integer live-count
    on the host counters line — never the membership list.
    """
    if status_snapshot is None:
        return {}, ["status_snapshot missing or null"]
    nodes = status_snapshot.get("roster")
    if not isinstance(nodes, list):
        # Accept legacy fixtures that still used "nodes" as a list.
        legacy = status_snapshot.get("nodes")
        nodes = legacy if isinstance(legacy, list) else None
    if not isinstance(nodes, list):
        return {}, ["status_snapshot missing roster list"]
    out: dict[int, str] = {}
    for item in nodes:
        if not isinstance(item, dict):
            continue
        ua = item.get("ua")
        state = item.get("state")
        if isinstance(ua, int) and isinstance(state, str):
            out[ua] = state
    return out, []

def _extract_node_statuses(node_statuses: Optional[dict]) -> tuple[dict[int, dict], list[str]]:
    """Extract per-UA node status payloads keyed by semantic UA."""
    if node_statuses is None:
        return {}, ["node_statuses missing from manifest"]
    if not isinstance(node_statuses, dict):
        return {}, ["node_statuses is not a JSON object"]
    out: dict[int, dict] = {}
    for key, value in node_statuses.items():
        try:
            ua = int(key)
        except (TypeError, ValueError):
            continue
        if isinstance(value, dict):
            out[ua] = value
    return out, []


def _analyze_lines(
    lines: list[str],
    ua_a: int,
    ua_b: int,
    status_snapshot: Optional[dict],
    node_statuses: Optional[dict] = None,
    expected_inputs: Optional[dict[int, int]] = None,
) -> ValidationResult:
    """Analyze capture lines for required initialization and poll/reply activity."""
    evidence = {
        ua_a: NodeEvidence(
            ua=ua_a,
            expected_input_bytes=(expected_inputs or {}).get(ua_a),
        ),
        ua_b: NodeEvidence(
            ua=ua_b,
            expected_input_bytes=(expected_inputs or {}).get(ua_b),
        ),
    }
    seen_uas: set[int] = set()

    for line in lines:
        match = PKT_RE.search(line)
        if not match:
            continue
        addr = int(match.group("ua"))
        seen_uas.add(addr)
        if addr not in evidence:
            continue
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

    node_states, status_failures = _extract_node_states(status_snapshot)
    parsed_node_statuses, node_status_failures = _extract_node_statuses(node_statuses)
    for addr, node in evidence.items():
        node.status_state = node_states.get(addr)
    failures: list[str] = list(status_failures)
    failures.extend(node_status_failures)
    for addr in (ua_a, ua_b):
        node = evidence[addr]
        status_doc = parsed_node_statuses.get(addr)
        if status_doc is None:
            failures.append(f"UA{addr}: per-node status missing from node_statuses")
        else:
            state_value = status_doc.get("state")
            if isinstance(state_value, str):
                node.status_state = state_value
            exchanges_value = status_doc.get("exchanges")
            if isinstance(exchanges_value, int):
                node.accepted_exchanges = exchanges_value
            observed_input_value = status_doc.get("observedIn")
            if isinstance(observed_input_value, int):
                node.observed_input_bytes = observed_input_value
            fault_doc = status_doc.get("fault")
            if isinstance(fault_doc, dict):
                fault_name = fault_doc.get("name")
                if isinstance(fault_name, str):
                    node.last_fault_name = fault_name
                fault_expected = fault_doc.get("expected")
                if isinstance(fault_expected, int):
                    node.last_fault_expected = fault_expected
                fault_observed = fault_doc.get("observed")
                if isinstance(fault_observed, int):
                    node.last_fault_observed = fault_observed
        if node.tx_i_or_t == 0:
            failures.append(f"UA{addr}: no TX I/T frames captured")
        if node.tx_polls == 0:
            failures.append(f"UA{addr}: no TX poll frames captured")
        # Secondary wire-activity floor, not the primary scorer: the
        # semantic path (accepted_exchanges below) is what #88 required.
        # This catches "no traffic on the wire at all" as an independent
        # signal; it can no longer carry a node by itself.
        if node.rx_replies == 0:
            failures.append(f"UA{addr}: no RX reply frames captured")
        if node.status_state is None:
            failures.append(f"UA{addr}: state missing from status_snapshot")
        if node.accepted_exchanges is None:
            failures.append(f"UA{addr}: accepted exchange count missing in per-node status")
        elif node.accepted_exchanges == 0:
            failures.append(f"UA{addr}: no accepted exchanges recorded")
        if node.status_state in ("UNINITIALIZED", "OFFLINE"):
            failures.append(f"UA{addr}: status state is {node.status_state}")
        if node.expected_input_bytes is not None:
            observed_geometry = node.observed_input_bytes
            if observed_geometry is None and node.last_fault_name == "GEOMETRY_MISMATCH":
                observed_geometry = node.last_fault_observed
            if observed_geometry is not None and observed_geometry != node.expected_input_bytes:
                failures.append(
                    f"UA{addr}: geometry disagreement (expected {node.expected_input_bytes}, observed {observed_geometry})"
                )
        wire_ua = addr + ord("A")
        if wire_ua in seen_uas and addr not in seen_uas:
            failures.append(
                f"UA{addr}: capture appears wire-encoded (saw ua={wire_ua}); semantic UA required"
            )

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
    node_statuses: Optional[dict] = None
    capture_path: Path
    ua_a = args.ua_a
    ua_b = args.ua_b
    expected_inputs: dict[int, int] = {}
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
        expected_inputs = {
            ua_a: int(manifest.get("ua_a_in", 0)),
            ua_b: int(manifest.get("ua_b_in", 0)),
        }
        status_snapshot = manifest.get("status_snapshot")
        node_statuses = manifest.get("node_statuses")
        if args.write_summary:
            summary_path = target / "summary.json"
    else:
        capture_path = target

    if not capture_path.exists():
        print(f"ERROR: capture file not found: {capture_path}", file=sys.stderr)
        return 2

    lines = capture_path.read_text(encoding="utf-8", errors="replace").splitlines()
    result = _analyze_lines(
        lines,
        ua_a=ua_a,
        ua_b=ua_b,
        status_snapshot=status_snapshot,
        node_statuses=node_statuses,
        expected_inputs=expected_inputs,
    )

    print(f"Capture: {capture_path}")
    print(
        f"UA{result.ua_a.ua} -> I/T TX={result.ua_a.tx_i_or_t}, "
        f"P TX={result.ua_a.tx_polls}, R RX={result.ua_a.rx_replies}, "
        f"state={result.ua_a.status_state}, exchanges={result.ua_a.accepted_exchanges}, "
        f"observedIn={result.ua_a.observed_input_bytes}"
    )
    print(
        f"UA{result.ua_b.ua} -> I/T TX={result.ua_b.tx_i_or_t}, "
        f"P TX={result.ua_b.tx_polls}, R RX={result.ua_b.rx_replies}, "
        f"state={result.ua_b.status_state}, exchanges={result.ua_b.accepted_exchanges}, "
        f"observedIn={result.ua_b.observed_input_bytes}"
    )
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
