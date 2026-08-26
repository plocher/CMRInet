import importlib
import sys
from pathlib import Path


sys.path.insert(0, str(Path(__file__).parent.absolute()))
gap_deltas = importlib.import_module("_gap_deltas")
analyze_data = importlib.import_module("analyze_data")


def test_gap_deltas_accepts_semantic_ua() -> None:
    lines = [
        "PKT t=1000 TX ua=31 mt=P len=3 n=0",
        "PKT t=12000 TX ua=31 mt=P len=3 n=1",
    ]
    result = gap_deltas.analyze_lines(lines, phantom_ua=31)
    assert result.verdict == gap_deltas.VERDICT_PASS


def test_gap_deltas_rejects_wire_ua_vocab() -> None:
    lines = [
        "PKT t=1000 TX ua=96 mt=P len=3 n=0",
        "PKT t=12000 TX ua=96 mt=P len=3 n=1",
    ]
    result = gap_deltas.analyze_lines(lines, phantom_ua=31)
    assert result.verdict == gap_deltas.VERDICT_FAIL_WRONG_UA_VOCAB


def test_analyze_data_accepts_semantic_ua() -> None:
    lines = [
        "PKT t=1000 TX ua=31 mt=P len=3 n=0",
        "PKT t=12000 TX ua=31 mt=P len=3 n=1",
    ]
    result = analyze_data.analyze_lines(lines, phantom_ua=31)
    assert result.verdict == analyze_data.VERDICT_PASS


def test_analyze_data_rejects_wire_ua_vocab() -> None:
    lines = [
        "PKT t=1000 TX ua=96 mt=P len=3 n=0",
        "PKT t=12000 TX ua=96 mt=P len=3 n=1",
    ]
    result = analyze_data.analyze_lines(lines, phantom_ua=31)
    assert result.verdict == analyze_data.VERDICT_FAIL_WRONG_UA_VOCAB
