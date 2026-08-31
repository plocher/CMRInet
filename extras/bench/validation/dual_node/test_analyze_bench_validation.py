import importlib.util
from pathlib import Path
import sys


MODULE_PATH = Path(__file__).with_name("analyze_bench_validation.py")
SPEC = importlib.util.spec_from_file_location("analyze_bench_validation", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules["analyze_bench_validation"] = MODULE
SPEC.loader.exec_module(MODULE)


def test_semantic_ua_capture_passes() -> None:
    lines = [
        "PKT t=1000 TX ua=30 mt=I len=3 n=0",
        "PKT t=1001 TX ua=30 mt=P len=3 n=1",
        "PKT t=1002 RX ua=30 mt=R len=3 n=2",
        "PKT t=1100 TX ua=31 mt=I len=3 n=3",
        "PKT t=1101 TX ua=31 mt=P len=3 n=4",
        "PKT t=1102 RX ua=31 mt=R len=3 n=5",
    ]
    status_snapshot = {
        "roster": [
            {"ua": 30, "state": "ONLINE"},
            {"ua": 31, "state": "ONLINE"},
        ]
    }
    node_statuses = {
        "30": {"ua": 30, "state": "ONLINE", "exchanges": 12, "observedIn": 7},
        "31": {"ua": 31, "state": "ONLINE", "exchanges": 11, "observedIn": 4},
    }
    result = MODULE._analyze_lines(
        lines,
        ua_a=30,
        ua_b=31,
        status_snapshot=status_snapshot,
        node_statuses=node_statuses,
        expected_inputs={30: 7, 31: 4},
    )
    assert result.pass_validation
    assert result.failures == []


def test_missing_status_snapshot_fails_closed() -> None:
    lines = [
        "PKT t=1000 TX ua=30 mt=I len=3 n=0",
        "PKT t=1001 TX ua=30 mt=P len=3 n=1",
        "PKT t=1002 RX ua=30 mt=R len=3 n=2",
        "PKT t=1100 TX ua=31 mt=I len=3 n=3",
        "PKT t=1101 TX ua=31 mt=P len=3 n=4",
        "PKT t=1102 RX ua=31 mt=R len=3 n=5",
    ]
    node_statuses = {
        "30": {"ua": 30, "state": "ONLINE", "exchanges": 12, "observedIn": 7},
        "31": {"ua": 31, "state": "ONLINE", "exchanges": 11, "observedIn": 4},
    }
    result = MODULE._analyze_lines(
        lines,
        ua_a=30,
        ua_b=31,
        status_snapshot=None,
        node_statuses=node_statuses,
        expected_inputs={30: 7, 31: 4},
    )
    assert not result.pass_validation
    assert "status_snapshot missing or null" in result.failures


def test_wire_encoded_capture_is_rejected() -> None:
    lines = [
        "PKT t=1000 TX ua=95 mt=I len=3 n=0",
        "PKT t=1001 TX ua=95 mt=P len=3 n=1",
        "PKT t=1002 RX ua=95 mt=R len=3 n=2",
        "PKT t=1100 TX ua=96 mt=I len=3 n=3",
        "PKT t=1101 TX ua=96 mt=P len=3 n=4",
        "PKT t=1102 RX ua=96 mt=R len=3 n=5",
    ]
    status_snapshot = {
        "roster": [
            {"ua": 30, "state": "ONLINE"},
            {"ua": 31, "state": "ONLINE"},
        ]
    }
    node_statuses = {
        "30": {"ua": 30, "state": "ONLINE", "exchanges": 12, "observedIn": 7},
        "31": {"ua": 31, "state": "ONLINE", "exchanges": 11, "observedIn": 4},
    }
    result = MODULE._analyze_lines(
        lines,
        ua_a=30,
        ua_b=31,
        status_snapshot=status_snapshot,
        node_statuses=node_statuses,
        expected_inputs={30: 7, 31: 4},
    )
    assert not result.pass_validation
    assert any("wire-encoded" in failure for failure in result.failures)


def test_geometry_disagreement_is_reported() -> None:
    lines = [
        "PKT t=1000 TX ua=30 mt=I len=3 n=0",
        "PKT t=1001 TX ua=30 mt=P len=3 n=1",
        "PKT t=1002 RX ua=30 mt=R len=3 n=2",
        "PKT t=1100 TX ua=31 mt=I len=3 n=3",
        "PKT t=1101 TX ua=31 mt=P len=3 n=4",
        "PKT t=1102 RX ua=31 mt=R len=3 n=5",
    ]
    status_snapshot = {
        "roster": [
            {"ua": 30, "state": "MISCONFIGURED"},
            {"ua": 31, "state": "ONLINE"},
        ]
    }
    node_statuses = {
        "30": {
            "ua": 30,
            "state": "MISCONFIGURED",
            "exchanges": 0,
            "fault": {"name": "GEOMETRY_MISMATCH", "expected": 7, "observed": 3},
        },
        "31": {"ua": 31, "state": "ONLINE", "exchanges": 11, "observedIn": 4},
    }
    result = MODULE._analyze_lines(
        lines,
        ua_a=30,
        ua_b=31,
        status_snapshot=status_snapshot,
        node_statuses=node_statuses,
        expected_inputs={30: 7, 31: 4},
    )
    assert not result.pass_validation
    assert any("no accepted exchanges recorded" in failure for failure in result.failures)
    assert any("geometry disagreement (expected 7, observed 3)" in failure for failure in result.failures)
