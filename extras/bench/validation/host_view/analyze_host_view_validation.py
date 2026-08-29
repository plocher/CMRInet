#!/usr/bin/env python3
"""Analyze host-view validation captures."""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Optional

EXPECT_ONLINE = "online"
EXPECT_GEOMETRY_MISMATCH = "geometry_mismatch"
EXPECT_OFFLINE = "offline"

PKT_RE = re.compile(
    r"^PKT\s+t=(?P<t>\d+)\s+(?P<dir>TX|RX)\s+[uU][aA]=(?P<ua>\d+)\s+mt=(?P<mt>[A-Z])"
)


@dataclass
class NodeExpectation:
    """Expected outcome for one UA in a host-view validation scenario."""

    ua: int
    expectation: str
    declared_in: int
    declared_out: int


@dataclass
class NodeEvidence:
    """Observed packet/status evidence for one UA."""

    ua: int
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
    verdict: str
    scenario: str
    scenario_has_expected_faults: bool
    expectations: dict[str, str]
    failures: list[str]
    nodes: dict[str, NodeEvidence]


def _load_manifest(results_dir: Path) -> dict:
    """Load manifest.json from a gather output directory."""
    manifest_path = results_dir / "manifest.json"
    if not manifest_path.exists():
        raise FileNotFoundError(f"manifest not found: {manifest_path}")
    return json.loads(manifest_path.read_text(encoding="utf-8"))


def _load_capture_lines(results_dir: Path, manifest: dict) -> list[str]:
    """Load capture lines from the manifest-declared capture file."""
    capture_name = manifest.get("capture_file")
    if not isinstance(capture_name, str):
        raise ValueError("manifest missing capture_file")
    capture_path = results_dir / capture_name
    if not capture_path.exists():
        raise FileNotFoundError(f"capture not found: {capture_path}")
    return capture_path.read_text(encoding="utf-8", errors="replace").splitlines()


def _parse_expectations(expectations_doc: dict) -> list[NodeExpectation]:
    """Parse node expectations from manifest JSON."""
    expectations: list[NodeExpectation] = []
    for key, raw in expectations_doc.items():
        if not isinstance(raw, dict):
            continue
        try:
            ua = int(key)
        except (TypeError, ValueError):
            continue
        expectation = raw.get("expectation")
        declared_in = raw.get("declared_in")
        declared_out = raw.get("declared_out")
        if (
            isinstance(expectation, str)
            and isinstance(declared_in, int)
            and isinstance(declared_out, int)
        ):
            expectations.append(
                NodeExpectation(
                    ua=ua,
                    expectation=expectation,
                    declared_in=declared_in,
                    declared_out=declared_out,
                )
            )
    return expectations


def _extract_packet_evidence(
    lines: list[str], expectations: list[NodeExpectation]
) -> tuple[dict[int, NodeEvidence], set[int]]:
    """Build per-UA packet counters from capture lines."""
    evidence = {exp.ua: NodeEvidence(ua=exp.ua) for exp in expectations}
    seen_uas: set[int] = set()
    for line in lines:
        match = PKT_RE.search(line)
        if not match:
            continue
        ua = int(match.group("ua"))
        seen_uas.add(ua)
        if ua not in evidence:
            continue
        node = evidence[ua]
        direction = match.group("dir")
        message_type = match.group("mt")
        if direction == "TX":
            if message_type in ("I", "T"):
                node.tx_i_or_t += 1
            if message_type == "P":
                node.tx_polls += 1
        elif direction == "RX" and message_type == "R":
            node.rx_replies += 1
    return evidence, seen_uas


def _extract_node_state_map(status_snapshot: Optional[dict]) -> tuple[dict[int, str], list[str]]:
    """Extract status snapshot state map keyed by semantic UA."""
    if status_snapshot is None:
        return {}, []
    nodes = status_snapshot.get("nodes")
    if not isinstance(nodes, list):
        return {}, []
    out: dict[int, str] = {}
    for node in nodes:
        if not isinstance(node, dict):
            continue
        ua = node.get("ua")
        state = node.get("state")
        if isinstance(ua, int) and isinstance(state, str):
            out[ua] = state
    return out, []


def _extract_node_status_map(node_statuses: Optional[dict]) -> tuple[dict[int, dict], list[str]]:
    """Extract per-UA status payloads keyed by semantic UA."""
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


def _apply_status_evidence(
    evidence: dict[int, NodeEvidence],
    status_states: dict[int, str],
    node_statuses: dict[int, dict],
    failures: list[str],
) -> None:
    """Populate node evidence from status snapshots and per-node status docs."""
    for ua, node in evidence.items():
        node.status_state = status_states.get(ua)
        status_doc = node_statuses.get(ua)
        if status_doc is None:
            failures.append(f"UA{ua}: per-node status missing from node_statuses")
            continue
        state_value = status_doc.get("state")
        if isinstance(state_value, str):
            node.status_state = state_value
        exchanges_value = status_doc.get("exchanges")
        if isinstance(exchanges_value, int):
            node.accepted_exchanges = exchanges_value
        observed_input = status_doc.get("observedIn")
        if isinstance(observed_input, int):
            node.observed_input_bytes = observed_input
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


def _validate_online(exp: NodeExpectation, node: NodeEvidence, failures: list[str]) -> None:
    """Validate the evidence for one expected-online node."""
    if node.tx_i_or_t == 0:
        failures.append(f"UA{exp.ua}: no TX I/T frames captured")
    if node.tx_polls == 0:
        failures.append(f"UA{exp.ua}: no TX poll frames captured")
    # Secondary wire-activity floor, not the primary scorer: the
    # semantic path (accepted_exchanges below) is what #88 required.
    # This catches "no traffic on the wire at all" as an independent
    # signal; it can no longer carry a node by itself.
    if node.rx_replies == 0:
        failures.append(f"UA{exp.ua}: no RX reply frames captured")
    if node.status_state in (None, "UNINITIALIZED", "OFFLINE", "MISCONFIGURED"):
        failures.append(f"UA{exp.ua}: expected online state, got {node.status_state}")
    if node.accepted_exchanges is None:
        failures.append(f"UA{exp.ua}: accepted exchange count missing in per-node status")
    elif node.accepted_exchanges == 0:
        failures.append(f"UA{exp.ua}: no accepted exchanges recorded")
    observed_geometry = node.observed_input_bytes
    if observed_geometry is None and _is_geometry_mismatch_fault(node):
        observed_geometry = node.last_fault_observed
    if observed_geometry is not None and observed_geometry != exp.declared_in:
        failures.append(
            f"UA{exp.ua}: geometry disagreement (expected {exp.declared_in}, observed {observed_geometry})"
        )

def _is_geometry_mismatch_fault(node: NodeEvidence) -> bool:
    """True when the node's last fault string indicates geometry mismatch."""
    if node.last_fault_name is None:
        return False
    lowered = node.last_fault_name.lower()
    return "geometry" in lowered and "mismatch" in lowered


def _validate_geometry_mismatch(
    exp: NodeExpectation, node: NodeEvidence, failures: list[str]
) -> None:
    """Validate the evidence for one expected geometry-mismatch node."""
    if node.tx_i_or_t == 0:
        failures.append(f"UA{exp.ua}: no TX I/T frames captured")
    if node.tx_polls == 0:
        failures.append(f"UA{exp.ua}: no TX poll frames captured")
    if not _is_geometry_mismatch_fault(node):
        failures.append(f"UA{exp.ua}: expected GEOMETRY_MISMATCH fault")
    if node.accepted_exchanges is None:
        failures.append(f"UA{exp.ua}: accepted exchange count missing in per-node status")
    elif node.accepted_exchanges != 0:
        failures.append(
            f"UA{exp.ua}: expected zero accepted exchanges under geometry mismatch, got {node.accepted_exchanges}"
        )
    if node.status_state not in ("MISCONFIGURED", "OFFLINE", "UNINITIALIZED"):
        failures.append(
            f"UA{exp.ua}: expected misconfigured/offline state under mismatch, got {node.status_state}"
        )
    if node.last_fault_expected is not None and node.last_fault_expected != exp.declared_in:
        failures.append(
            f"UA{exp.ua}: fault expected={node.last_fault_expected}, declared_in={exp.declared_in}"
        )


def _validate_offline(exp: NodeExpectation, node: NodeEvidence, failures: list[str]) -> None:
    """Validate the evidence for one expected-offline node."""
    if node.tx_i_or_t == 0:
        failures.append(f"UA{exp.ua}: expected offline probe traffic, but no TX I/T frames")
    if node.tx_polls == 0:
        failures.append(f"UA{exp.ua}: expected offline probe traffic, but no TX polls")
    if node.rx_replies != 0:
        failures.append(f"UA{exp.ua}: expected no replies for offline node, got {node.rx_replies}")
    if node.accepted_exchanges is None:
        failures.append(f"UA{exp.ua}: accepted exchange count missing in per-node status")
    elif node.accepted_exchanges != 0:
        failures.append(
            f"UA{exp.ua}: expected zero accepted exchanges for offline node, got {node.accepted_exchanges}"
        )
    if node.status_state not in ("OFFLINE", "UNINITIALIZED"):
        failures.append(f"UA{exp.ua}: expected OFFLINE/UNINITIALIZED state, got {node.status_state}")


def _validate_vocab(seen_uas: set[int], exp: NodeExpectation, failures: list[str]) -> None:
    """Fail if capture appears wire-encoded for an expected semantic UA."""
    wire_ua = exp.ua + ord("A")
    if wire_ua in seen_uas and exp.ua not in seen_uas:
        failures.append(
            f"UA{exp.ua}: capture appears wire-encoded (saw ua={wire_ua}); semantic UA required"
        )

def _classify_verdict(pass_validation: bool, has_expected_faults: bool) -> str:
    """Return explicit scenario-relative verdict text."""
    if pass_validation:
        return "PASS_EXPECTED_FAULT" if has_expected_faults else "PASS"
    return "FAIL_EXPECTED_FAULT_SCENARIO" if has_expected_faults else "FAIL"


def analyze_manifest(manifest: dict, lines: list[str]) -> ValidationResult:
    """Analyze one host-view scenario manifest + capture lines."""
    scenario = str(manifest.get("scenario", "unknown"))
    expectations_doc = manifest.get("node_expectations")
    if not isinstance(expectations_doc, dict):
        return ValidationResult(
            pass_validation=False,
            verdict="ERROR_INVALID_MANIFEST",
            scenario=scenario,
            scenario_has_expected_faults=False,
            expectations={},
            failures=["manifest missing node_expectations object"],
            nodes={},
        )
    expectations = _parse_expectations(expectations_doc)
    expectation_map = {str(exp.ua): exp.expectation for exp in expectations}
    has_expected_faults = any(
        exp.expectation in (EXPECT_GEOMETRY_MISMATCH, EXPECT_OFFLINE)
        for exp in expectations
    )
    evidence, seen_uas = _extract_packet_evidence(lines, expectations)

    status_states, status_failures = _extract_node_state_map(manifest.get("status_snapshot"))
    per_node_status, node_status_failures = _extract_node_status_map(
        manifest.get("node_statuses")
    )
    failures: list[str] = list(status_failures)
    failures.extend(node_status_failures)
    _apply_status_evidence(evidence, status_states, per_node_status, failures)

    for exp in expectations:
        node = evidence[exp.ua]
        _validate_vocab(seen_uas, exp, failures)
        if exp.expectation == EXPECT_ONLINE:
            _validate_online(exp, node, failures)
        elif exp.expectation == EXPECT_GEOMETRY_MISMATCH:
            _validate_geometry_mismatch(exp, node, failures)
        elif exp.expectation == EXPECT_OFFLINE:
            _validate_offline(exp, node, failures)
        else:
            failures.append(f"UA{exp.ua}: unsupported expectation '{exp.expectation}'")

    pass_validation = len(failures) == 0
    return ValidationResult(
        pass_validation=pass_validation,
        verdict=_classify_verdict(pass_validation, has_expected_faults),
        scenario=scenario,
        scenario_has_expected_faults=has_expected_faults,
        expectations=expectation_map,
        failures=failures,
        nodes={str(ua): node for ua, node in evidence.items()},
    )


def main() -> int:
    """Analyze one host-view gather output directory."""
    parser = argparse.ArgumentParser(description="Analyze host-view bench validation")
    parser.add_argument("target", help="Results directory from gather_host_view_validation.py")
    parser.add_argument(
        "--write-summary",
        action="store_true",
        help="Write summary.json beside manifest for downstream use",
    )
    args = parser.parse_args()

    target = Path(args.target)
    if not target.is_dir():
        print(f"ERROR: target must be a results directory: {target}", file=sys.stderr)
        return 2

    try:
        manifest = _load_manifest(target)
        lines = _load_capture_lines(target, manifest)
    except (FileNotFoundError, ValueError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 2

    result = analyze_manifest(manifest, lines)
    print(
        f"Scenario: {result.scenario} "
        f"(has_expected_faults={result.scenario_has_expected_faults})"
    )
    print(f"Verdict: {result.verdict}")
    if result.expectations:
        print("Expectations:")
        for key in sorted(result.expectations.keys(), key=lambda value: int(value)):
            print(f"  UA{key}: {result.expectations[key]}")
    for key in sorted(result.nodes.keys(), key=lambda value: int(value)):
        node = result.nodes[key]
        print(
            f"UA{node.ua} -> I/T TX={node.tx_i_or_t}, P TX={node.tx_polls}, "
            f"R RX={node.rx_replies}, state={node.status_state}, "
            f"exchanges={node.accepted_exchanges}, observedIn={node.observed_input_bytes}, "
            f"fault={node.last_fault_name}"
        )
    if result.pass_validation:
        print(f"{result.verdict}: scenario expectations satisfied")
    else:
        print(f"{result.verdict}:")
        for failure in result.failures:
            print(f"  - {failure}")

    if args.write_summary:
        summary_path = target / "summary.json"
        summary_path.write_text(json.dumps(asdict(result), indent=2) + "\n", encoding="utf-8")
        print(f"Wrote summary: {summary_path}")

    return 0 if result.pass_validation else 1


if __name__ == "__main__":
    sys.exit(main())
