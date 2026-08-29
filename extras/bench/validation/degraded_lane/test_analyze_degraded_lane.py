import importlib.util
from pathlib import Path
import sys


MODULE_PATH = Path(__file__).with_name("analyze_degraded_lane.py")
SPEC = importlib.util.spec_from_file_location("analyze_degraded_lane", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules["analyze_degraded_lane"] = MODULE
SPEC.loader.exec_module(MODULE)


def _passing_capture() -> list[str]:
    """Healthy UA30 gets 200 polls; degraded UA31 gets 30, UA32 gets 20.

    Total degraded = 50, share = 50/250 = 20% — bounded under the 35%
    cap, matching the unit test's 20% configured Gate A.
    """
    lines = []
    for i in range(200):
        lines.append(f"PKT t={1000 + i * 5} TX UA=30 mt=P len=3 n={i}")
        lines.append(f"PKT t={1001 + i * 5} RX UA=30 mt=R len=3 n={i}")
    for i in range(30):
        lines.append(f"PKT t={1002 + i * 5} TX UA=31 mt=P len=3 n={200 + i}")
        lines.append(f"PKT t={1003 + i * 5} RX UA=31 mt=R len=3 n={200 + i}")
    for i in range(20):
        lines.append(f"PKT t={1004 + i * 5} TX UA=32 mt=P len=0 n={230 + i}")
    return lines


def _passing_manifest() -> dict:
    return {
        "scenario": "degraded_lane_bounding",
        "healthy_ua": 30,
        "degraded_uas": [31, 32],
        "ua_30_in": 7,
        "ua_31_in": 4,
        "ua_32_in": 4,
        "status_snapshot": {
            "degradedGrants": 50,
            "degradedSlotDenials": 30,
            "degradedBandwidthDenials": 5,
            "degradedClampBypasses": 0,
        },
        "node_statuses": {
            "30": {"ua": 30, "state": "ONLINE", "exchanges": 200, "observedIn": 7},
            "31": {"ua": 31, "state": "MISCONFIGURED", "exchanges": 0, "observedIn": 3},
            "32": {"ua": 32, "state": "OFFLINE", "exchanges": 0},
        },
    }


def test_bounded_degraded_lane_passes() -> None:
    result = MODULE.analyze_manifest(_passing_manifest(), _passing_capture())
    assert result.pass_validation
    assert result.verdict == "PASS"
    assert result.failures == []
    assert result.degraded_polls < result.healthy_polls
    assert result.degraded_share_percent < 35.0


def test_inversion_fails() -> None:
    """The #80 condition: the degraded population outpolls the healthy one."""
    manifest = _passing_manifest()
    lines = []
    for i in range(100):
        lines.append(f"PKT t={1000 + i * 5} TX UA=30 mt=P len=3 n={i}")
        lines.append(f"PKT t={1001 + i * 5} RX UA=30 mt=R len=3 n={i}")
    for i in range(300):
        lines.append(f"PKT t={1002 + i * 5} TX UA=31 mt=P len=3 n={100 + i}")
        lines.append(f"PKT t={1003 + i * 5} RX UA=31 mt=R len=3 n={100 + i}")
    for i in range(200):
        lines.append(f"PKT t={1004 + i * 5} TX UA=32 mt=P len=0 n={400 + i}")
    result = MODULE.analyze_manifest(manifest, lines)
    assert not result.pass_validation
    assert any("inversion" in f for f in result.failures)


def test_no_gate_bound_fails() -> None:
    """The ledger shows no denials: nothing was being bounded."""
    manifest = _passing_manifest()
    manifest["status_snapshot"] = {
        "degradedGrants": 50,
        "degradedSlotDenials": 0,
        "degradedBandwidthDenials": 0,
        "degradedClampBypasses": 0,
    }
    result = MODULE.analyze_manifest(manifest, _passing_capture())
    assert not result.pass_validation
    assert any("no gate ever bound" in f for f in result.failures)


def test_missing_status_snapshot_fails_closed() -> None:
    """A null status snapshot fails rather than skipping the ledger check."""
    manifest = _passing_manifest()
    manifest["status_snapshot"] = None
    result = MODULE.analyze_manifest(manifest, _passing_capture())
    assert not result.pass_validation
    assert "status_snapshot missing or null" in result.failures


def test_degraded_node_with_accepted_exchanges_fails() -> None:
    """A degraded node should commit nothing."""
    manifest = _passing_manifest()
    manifest["node_statuses"]["31"] = {
        "ua": 31, "state": "MISCONFIGURED", "exchanges": 5, "observedIn": 3,
    }
    result = MODULE.analyze_manifest(manifest, _passing_capture())
    assert not result.pass_validation
    assert any("should commit nothing" in f for f in result.failures)


def test_silent_node_polls_counted_in_degraded_share() -> None:
    """UA32's polls are valid degraded-lane traffic, not pollution.

    If UA32's polls were excluded, a run where UA32 alone takes 40%
    would pass. With UA32 included in the degraded population, the
    share exceeds 35% and the run fails — proving the silent node is
    counted.
    """
    manifest = _passing_manifest()
    lines = []
    # Healthy: 100 polls. UA31: 10 polls. UA32: 80 polls.
    # Total degraded = 90, share = 90/190 = 47.4% — exceeds 35%.
    for i in range(100):
        lines.append(f"PKT t={1000 + i * 5} TX UA=30 mt=P len=3 n={i}")
        lines.append(f"PKT t={1001 + i * 5} RX UA=30 mt=R len=3 n={i}")
    for i in range(10):
        lines.append(f"PKT t={1002 + i * 5} TX UA=31 mt=P len=3 n={100 + i}")
        lines.append(f"PKT t={1003 + i * 5} RX UA=31 mt=R len=3 n={100 + i}")
    for i in range(80):
        lines.append(f"PKT t={1004 + i * 5} TX UA=32 mt=P len=0 n={110 + i}")
    result = MODULE.analyze_manifest(manifest, lines)
    assert not result.pass_validation
    assert any("cap" in f for f in result.failures)
