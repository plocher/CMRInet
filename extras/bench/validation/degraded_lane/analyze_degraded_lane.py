#!/usr/bin/env python3
"""Analyze degraded-lane bounding bench validation captures (#87/#88).

The on-hardware analog of test_degraded_participation_is_bounded in
tests/test_host.cpp. That unit test refutes the #80 baseline — a node
declared NI=4 answering with 3 bytes took 1316 polls to a healthy
node's 765, serviced 1.7x MORE than the node doing real work while
committing nothing — against a mock transport. This analyzer does the
same against a real RS485 bench capture.

The unit test's population is three nodes, and so is this one:
- UA30 (healthy): conforming, with generator load for contention.
- UA31 (degraded, misconfigured): a real alive node declared 4/4
  against its physical 3/3 — its replies carry the wrong geometry and
  are rejected. This is the #80 condition.
- UA32 (degraded, silent): the runtime-added phantom via node add — a nonexistent
  node that never replies. Its polls are valid degraded-lane traffic,
  not pollution: the 80/20 rule bounds ALL imperfect nodes, and a
  silent node is the other failure mode the gates exist to bound.

What it scores, and from which seam:
- Poll distribution: TX P frames per UA, counted from the ring dump
  (the trace seam). Counting polls sent is wire truth, which is exactly
  the question here — "how many times did the Host poll each node."
  This is not the bug #88 was filed about (that was scoring accepted
  exchanges from R frames); poll distribution is a different question
  and the trace seam is correct for it.
- Degraded-lane ledger: degradedGrants, degradedSlotDenials,
  degradedBandwidthDenials, degradedClampBypasses from the host status
  snapshot (the semantic seam). These prove a gate actually bound.
- Per-node accepted exchanges and state: from per-node status (the
  semantic seam), proving the healthy node committed data and the
  degraded nodes did not.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Optional

# Bounding thresholds, matching the unit test defaults. The degraded
# share cap is 35% (the unit test uses the same value against the 20%
# configured Gate A); the inversion check is simply degraded < healthy.
DEFAULT_DEGRADED_SHARE_PERCENT = 35
DEFAULT_MIN_HEALTHY_EXCHANGES = 10

PKT_RE = re.compile(
    r"^PKT\s+t=(?P<t>\d+)\s+(?P<dir>TX|RX)\s+[uU][aA]=(?P<ua>\d+)\s+mt=(?P<mt>[A-Z])"
)


@dataclass
class NodeEvidence:
    """Poll and status evidence for one UA."""

    ua: int
    declared_in: Optional[int] = None
    tx_polls: int = 0
    rx_replies: int = 0
    status_state: Optional[str] = None
    accepted_exchanges: Optional[int] = None
    observed_input_bytes: Optional[int] = None


@dataclass
class DegradedLedger:
    """Host-scope degraded-lane counters from the status snapshot."""

    degraded_grants: Optional[int] = None
    degraded_slot_denials: Optional[int] = None
    degraded_bandwidth_denials: Optional[int] = None
    degraded_clamp_bypasses: Optional[int] = None


@dataclass
class ValidationResult:
    """Top-level analyzer output."""

    pass_validation: bool
    verdict: str
    healthy_ua: int
    degraded_uas: list[int]
    healthy_polls: int
    degraded_polls: int
    degraded_share_percent: float
    ledger: DegradedLedger
    healthy: NodeEvidence
    degraded: list[NodeEvidence] = field(default_factory=list)
    failures: list[str] = field(default_factory=list)


def _count_polls(lines: list[str], uas: list[int]) -> dict[int, NodeEvidence]:
    """Count TX P (and RX R) per UA from capture lines."""
    evidence = {ua: NodeEvidence(ua=ua) for ua in uas}
    for line in lines:
        match = PKT_RE.search(line)
        if not match:
            continue
        ua = int(match.group("ua"))
        if ua not in evidence:
            continue
        node = evidence[ua]
        direction = match.group("dir")
        mt = match.group("mt")
        if direction == "TX" and mt == "P":
            node.tx_polls += 1
        elif direction == "RX" and mt == "R":
            node.rx_replies += 1
    return evidence


def _extract_ledger(status_snapshot: Optional[dict]) -> tuple[DegradedLedger, list[str]]:
    """Read the host-scope degraded-lane counters from the status snapshot."""
    if status_snapshot is None:
        return DegradedLedger(), ["status_snapshot missing or null"]
    ledger = DegradedLedger(
        degraded_grants=_opt_int(status_snapshot, "degradedGrants"),
        degraded_slot_denials=_opt_int(status_snapshot, "degradedSlotDenials"),
        degraded_bandwidth_denials=_opt_int(status_snapshot, "degradedBandwidthDenials"),
        degraded_clamp_bypasses=_opt_int(status_snapshot, "degradedClampBypasses"),
    )
    return ledger, []


def _opt_int(doc: dict, key: str) -> Optional[int]:
    value = doc.get(key)
    return value if isinstance(value, int) else None


def _apply_node_status(
    evidence: dict[int, NodeEvidence],
    node_statuses: Optional[dict],
    failures: list[str],
) -> None:
    """Populate per-node evidence from the manifest's node_statuses map."""
    if node_statuses is None:
        failures.append("node_statuses missing from manifest")
        return
    if not isinstance(node_statuses, dict):
        failures.append("node_statuses is not a JSON object")
        return
    for ua, node in evidence.items():
        status_doc = node_statuses.get(str(ua))
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


def analyze_manifest(
    manifest: dict,
    lines: list[str],
    degraded_share_percent: int = DEFAULT_DEGRADED_SHARE_PERCENT,
    min_healthy_exchanges: int = DEFAULT_MIN_HEALTHY_EXCHANGES,
) -> ValidationResult:
    """Analyze one degraded-lane scenario manifest + capture lines.

    The manifest identifies one healthy UA and a list of degraded UAs.
    The degraded population is the SUM of all degraded nodes' polls,
    matching the unit test's `degradedPolls = nodes[1].polls +
    nodes[2].polls`. A silent node (UA32, the phantom) is a valid
    degraded node — its polls are bounded traffic, not pollution.
    """
    healthy_ua = int(manifest.get("healthy_ua", 30))
    degraded_uas_raw = manifest.get("degraded_uas", [31, 32])
    degraded_uas = [int(ua) for ua in degraded_uas_raw]
    all_uas = [healthy_ua] + degraded_uas

    evidence = _count_polls(lines, all_uas)
    # Carry declared geometry from the manifest into the evidence.
    for ua in all_uas:
        key = f"ua_{ua}_in"
        if key in manifest:
            evidence[ua].declared_in = int(manifest[key])

    ledger, ledger_failures = _extract_ledger(manifest.get("status_snapshot"))
    node_status_failures: list[str] = []
    _apply_node_status(evidence, manifest.get("node_statuses"), node_status_failures)

    failures: list[str] = list(ledger_failures)
    failures.extend(node_status_failures)

    healthy = evidence[healthy_ua]
    degraded_nodes = [evidence[ua] for ua in degraded_uas]
    degraded_polls = sum(n.tx_polls for n in degraded_nodes)
    total_polls = healthy.tx_polls + degraded_polls
    degraded_share = (
        (degraded_polls * 100.0 / total_polls) if total_polls > 0 else 100.0
    )

    # 1. The #80 inversion is gone: the degraded population no longer
    #    outpolls the healthy node. On the baseline, broken=1316 vs
    #    healthy=765.
    if total_polls == 0:
        failures.append("no TX poll frames captured for any node")
    else:
        if degraded_polls >= healthy.tx_polls:
            failures.append(
                f"degraded population ({degraded_polls} polls across "
                f"UA{degraded_uas}) outpolled or tied UA{healthy_ua} "
                f"({healthy.tx_polls} polls); the #80 inversion is still present"
            )
        # 2. Bounded, not merely smaller: the degraded share stays under
        #    the configured cap (35% by default, matching the unit test).
        if degraded_polls * 100 >= total_polls * degraded_share_percent:
            failures.append(
                f"degraded share {degraded_share:.1f}% >= {degraded_share_percent}% "
                f"cap (degraded={degraded_polls}, healthy={healthy.tx_polls})"
            )

    # 3. A gate actually bound: the ledger has grants AND denials. Without
    #    denials, nothing was being bounded (the #80 condition).
    if ledger.degraded_grants is not None and ledger.degraded_grants == 0:
        failures.append("degraded class was never served (degradedGrants=0)")
    if (
        ledger.degraded_slot_denials is not None
        and ledger.degraded_bandwidth_denials is not None
        and ledger.degraded_slot_denials + ledger.degraded_bandwidth_denials == 0
    ):
        failures.append(
            "no gate ever bound (degradedSlotDenials + degradedBandwidthDenials = 0); "
            "nothing was being bounded"
        )

    # 4. The healthy node committed data.
    if healthy.accepted_exchanges is None:
        failures.append(f"UA{healthy_ua}: accepted exchange count missing")
    elif healthy.accepted_exchanges < min_healthy_exchanges:
        failures.append(
            f"UA{healthy_ua}: only {healthy.accepted_exchanges} accepted exchanges "
            f"(need {min_healthy_exchanges}); healthy node never got going"
        )
    if healthy.status_state in (None, "UNINITIALIZED", "OFFLINE"):
        failures.append(f"UA{healthy_ua}: expected online, got {healthy.status_state}")

    # 5. Each degraded node committed nothing.
    for node in degraded_nodes:
        if node.accepted_exchanges is None:
            failures.append(f"UA{node.ua}: accepted exchange count missing")
        elif node.accepted_exchanges != 0:
            failures.append(
                f"UA{node.ua}: {node.accepted_exchanges} accepted exchanges; "
                f"a degraded node should commit nothing"
            )

    pass_validation = len(failures) == 0
    return ValidationResult(
        pass_validation=pass_validation,
        verdict="PASS" if pass_validation else "FAIL",
        healthy_ua=healthy_ua,
        degraded_uas=degraded_uas,
        healthy_polls=healthy.tx_polls,
        degraded_polls=degraded_polls,
        degraded_share_percent=round(degraded_share, 1),
        ledger=ledger,
        healthy=healthy,
        degraded=degraded_nodes,
        failures=failures,
    )


def _load_manifest(results_dir: Path) -> dict:
    """Load manifest.json from a gather output directory."""
    manifest_path = results_dir / "manifest.json"
    if not manifest_path.exists():
        raise FileNotFoundError(f"manifest not found: {manifest_path}")
    return json.loads(manifest_path.read_text(encoding="utf-8"))


def main() -> int:
    """Analyze one degraded-lane results directory."""
    parser = argparse.ArgumentParser(description="Analyze degraded-lane bench validation")
    parser.add_argument("target", help="Results directory from gather_degraded_lane.py")
    parser.add_argument(
        "--degraded-share-percent", type=int, default=DEFAULT_DEGRADED_SHARE_PERCENT,
        help=f"cap on degraded poll share (default {DEFAULT_DEGRADED_SHARE_PERCENT})",
    )
    parser.add_argument(
        "--min-healthy-exchanges", type=int, default=DEFAULT_MIN_HEALTHY_EXCHANGES,
        help=f"floor on healthy accepted exchanges (default {DEFAULT_MIN_HEALTHY_EXCHANGES})",
    )
    parser.add_argument(
        "--write-summary", action="store_true",
        help="Write summary.json beside manifest for downstream use",
    )
    args = parser.parse_args()

    target = Path(args.target)
    if not target.is_dir():
        print(f"ERROR: target must be a results directory: {target}", file=sys.stderr)
        return 2

    try:
        manifest = _load_manifest(target)
    except (FileNotFoundError, json.JSONDecodeError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 2

    capture_name = manifest.get("capture_file")
    if not isinstance(capture_name, str):
        print("ERROR: manifest missing capture_file", file=sys.stderr)
        return 2
    capture_path = target / capture_name
    if not capture_path.exists():
        print(f"ERROR: capture file not found: {capture_path}", file=sys.stderr)
        return 2

    lines = capture_path.read_text(encoding="utf-8", errors="replace").splitlines()
    result = analyze_manifest(
        manifest, lines,
        degraded_share_percent=args.degraded_share_percent,
        min_healthy_exchanges=args.min_healthy_exchanges,
    )

    print(f"Scenario: degraded-lane bounding")
    print(f"Verdict:  {result.verdict}")
    print(
        f"UA{result.healthy_ua} (healthy):  {result.healthy_polls} polls, "
        f"state={result.healthy.status_state}, "
        f"exchanges={result.healthy.accepted_exchanges}"
    )
    for node in result.degraded:
        print(
            f"UA{node.ua} (degraded): {node.tx_polls} polls, "
            f"state={node.status_state}, "
            f"exchanges={node.accepted_exchanges}"
        )
    print(f"Degraded share: {result.degraded_share_percent}%")
    print(
        f"Ledger: grants={result.ledger.degraded_grants}, "
        f"slotDenials={result.ledger.degraded_slot_denials}, "
        f"bwDenials={result.ledger.degraded_bandwidth_denials}, "
        f"clampBypasses={result.ledger.degraded_clamp_bypasses}"
    )
    if result.pass_validation:
        print("PASS: degraded lane is bounded; the #80 inversion is gone")
    else:
        print("FAIL:")
        for failure in result.failures:
            print(f"  - {failure}")

    if args.write_summary:
        summary_path = target / "summary.json"
        summary_path.write_text(json.dumps(asdict(result), indent=2) + "\n", encoding="utf-8")
        print(f"Wrote summary: {summary_path}")

    return 0 if result.pass_validation else 1


if __name__ == "__main__":
    sys.exit(main())
