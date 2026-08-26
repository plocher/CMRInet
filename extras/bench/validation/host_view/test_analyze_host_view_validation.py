import importlib.util
import sys
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("analyze_host_view_validation.py")
SPEC = importlib.util.spec_from_file_location("analyze_host_view_validation", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules["analyze_host_view_validation"] = MODULE
SPEC.loader.exec_module(MODULE)


def _base_manifest() -> dict:
    return {
        "scenario": "unit-test",
        "node_expectations": {
            "30": {"expectation": "online", "declared_in": 7, "declared_out": 7},
            "31": {"expectation": "online", "declared_in": 4, "declared_out": 4},
        },
        "status_snapshot": {
            "nodes": [
                {"ua": 30, "state": "ONLINE"},
                {"ua": 31, "state": "ONLINE"},
            ]
        },
        "node_statuses": {
            "30": {"ua": 30, "state": "ONLINE", "exchanges": 10, "observedIn": 7},
            "31": {"ua": 31, "state": "ONLINE", "exchanges": 9, "observedIn": 4},
        },
    }


def test_online_nodes_pass() -> None:
    lines = [
        "PKT t=1000 TX ua=30 mt=I len=3 n=0",
        "PKT t=1001 TX ua=30 mt=P len=3 n=1",
        "PKT t=1002 RX ua=30 mt=R len=3 n=2",
        "PKT t=1100 TX ua=31 mt=I len=3 n=3",
        "PKT t=1101 TX ua=31 mt=P len=3 n=4",
        "PKT t=1102 RX ua=31 mt=R len=3 n=5",
    ]
    result = MODULE.analyze_manifest(_base_manifest(), lines)
    assert result.pass_validation
    assert result.verdict == "PASS"
    assert result.failures == []


def test_geometry_mismatch_expectation_passes() -> None:
    manifest = _base_manifest()
    manifest["node_expectations"]["31"] = {
        "expectation": "geometry_mismatch",
        "declared_in": 1,
        "declared_out": 1,
    }
    manifest["status_snapshot"]["nodes"][1]["state"] = "MISCONFIGURED"
    manifest["node_statuses"]["31"] = {
        "ua": 31,
        "state": "MISCONFIGURED",
        "exchanges": 0,
        "fault": {"name": "GEOMETRY_MISMATCH", "expected": 1, "observed": 4},
    }
    lines = [
        "PKT t=1000 TX ua=30 mt=I len=3 n=0",
        "PKT t=1001 TX ua=30 mt=P len=3 n=1",
        "PKT t=1002 RX ua=30 mt=R len=3 n=2",
        "PKT t=1100 TX ua=31 mt=I len=3 n=3",
        "PKT t=1101 TX ua=31 mt=P len=3 n=4",
    ]
    result = MODULE.analyze_manifest(manifest, lines)
    assert result.pass_validation
    assert result.verdict == "PASS_EXPECTED_FAULT"
    assert result.failures == []


def test_offline_expectation_passes_for_phantom_node() -> None:
    manifest = _base_manifest()
    manifest["node_expectations"]["32"] = {
        "expectation": "offline",
        "declared_in": 4,
        "declared_out": 4,
    }
    manifest["status_snapshot"]["nodes"].append({"ua": 32, "state": "OFFLINE"})
    manifest["node_statuses"]["32"] = {
        "ua": 32,
        "state": "OFFLINE",
        "exchanges": 0,
    }
    lines = [
        "PKT t=1000 TX ua=30 mt=I len=3 n=0",
        "PKT t=1001 TX ua=30 mt=P len=3 n=1",
        "PKT t=1002 RX ua=30 mt=R len=3 n=2",
        "PKT t=1100 TX ua=31 mt=I len=3 n=3",
        "PKT t=1101 TX ua=31 mt=P len=3 n=4",
        "PKT t=1102 RX ua=31 mt=R len=3 n=5",
        "PKT t=1200 TX ua=32 mt=I len=3 n=6",
        "PKT t=1201 TX ua=32 mt=P len=3 n=7",
    ]
    result = MODULE.analyze_manifest(manifest, lines)
    assert result.pass_validation
    assert result.verdict == "PASS_EXPECTED_FAULT"
    assert result.failures == []
