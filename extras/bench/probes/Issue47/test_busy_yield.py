import pytest
from unittest.mock import MagicMock, patch
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.absolute()))
import _tracer_client as sweep_47

def test_sync_and_validate_boot_success():
    mock_ser = MagicMock()
    mock_ser.readline.side_effect = [
        b"",
        b"some garbage\n",
        b'{"seq":0,"ts":0,"event":"epoch","image":"tracer_host","version":"0.4.0"}\n'
    ]
    assert sweep_47.sync_and_validate_boot(mock_ser, timeout=1.0) == True

def test_sync_and_validate_boot_wrong_image():
    mock_ser = MagicMock()
    mock_ser.readline.side_effect = [
        b'{"seq":0,"ts":0,"event":"epoch","image":"xiao_sniffer","version":"0.4.0"}\n'
    ]
    with pytest.raises(SystemExit):
        sweep_47.sync_and_validate_boot(mock_ser, timeout=1.0)

def test_sync_and_validate_boot_timeout():
    mock_ser = MagicMock()
    mock_ser.readline.return_value = b""
    assert sweep_47.sync_and_validate_boot(mock_ser, timeout=0.1) == False

def test_run_combo_timeout():
    mock_ser = MagicMock()
    mock_ser.in_waiting = 0
    mock_ser.readline.return_value = b""
    res = sweep_47.run_combo(mock_ser, 25, 150, "yield", "walker", 1, Path("/tmp"), "tag")
    assert res.verdict == "ERROR_TIMEOUT"

def test_run_combo_success(tmp_path):
    mock_ser = MagicMock()
    mock_ser.in_waiting = 0
    # Mock sequence: reset, config nodes, traffic, stall, run
    # For readline, we need it to return END CAPTURE eventually
    mock_ser.readline.side_effect = [
        b"",
        b"BEGIN CAPTURE t=1000\n",
        b"END CAPTURE t=2000 polls=10 its=5 ring_used=5/1000\n",
        b"BEGIN DUMP records=2\n",
        b"PKT t=1000 TX ua=31 mt=P len=3 n=0\n",
        b"PKT t=1500 TX ua=31 mt=P len=3 n=1\n",
        b"END DUMP\n",
        b"", b"", b"", b""
    ]
    
    res = sweep_47.run_combo(mock_ser, 25, 150, "yield", "walker", 1, tmp_path, "test_tag")
    
    assert res.verdict != "ERROR_TIMEOUT"
    assert (tmp_path / "test_tag.log").exists()
    
import pytest
import importlib

# Python doesn't allow importing modules that start with a number directly.
gap_deltas = importlib.import_module("_gap_deltas")

classify_monotonicity = gap_deltas.classify_monotonicity
VERDICT_PASS = gap_deltas.VERDICT_PASS
VERDICT_FAIL_STUCK = gap_deltas.VERDICT_FAIL_STUCK
VERDICT_FAIL_CYCLING = gap_deltas.VERDICT_FAIL_CYCLING
VERDICT_FAIL_TOO_FEW = gap_deltas.VERDICT_FAIL_TOO_FEW

def test_strictly_doubling():
    # 250, 500, 1000, 2000, 4000, 8000, 16000
    gaps = [250, 500, 1000, 2000, 4000, 8000, 16000]
    verdict, m_max = classify_monotonicity(gaps)
    assert verdict == VERDICT_PASS
    assert m_max == 16000

def test_capped():
    # 250, 500, 1000, 8000, 8000, 8000
    # Jitter at cap shouldn't trigger cycling unless it's < 50%
    gaps = [250, 500, 1000, 8000, 7990, 8010]
    verdict, m_max = classify_monotonicity(gaps)
    assert verdict == VERDICT_PASS
    assert m_max == 8000  # monotonic prefix stops at 8000, because 7990 < 8000

def test_cycling_high_water():
    # 250, 500, 1000, 10000, 6000, 1000
    gaps = [250, 500, 1000, 10000, 6000, 1000]
    verdict, m_max = classify_monotonicity(gaps)
    assert verdict == VERDICT_FAIL_CYCLING
    assert m_max == 10000

def test_all_equal():
    # 250, 250, 250
    gaps = [250, 250, 250]
    verdict, m_max = classify_monotonicity(gaps)
    assert verdict == VERDICT_FAIL_STUCK
    assert m_max == 250

def test_too_few():
    verdict, m_max = classify_monotonicity([])
    assert verdict == VERDICT_FAIL_TOO_FEW
    assert m_max == -1

def test_cycling_but_below_threshold():
    # 250, 500, 1000, 4000, 2000, 1000
    gaps = [250, 500, 1000, 4000, 1000]
    verdict, m_max = classify_monotonicity(gaps)
    assert verdict == VERDICT_FAIL_STUCK
    assert m_max == 4000

def test_cycling_ignores_small_drops():
    # 250, 500, 1000, 8000, 6000, 7000
    # drops from 8000 to 6000 (not < 4000), so not cycling
    gaps = [250, 500, 1000, 8000, 6000, 7000]
    verdict, m_max = classify_monotonicity(gaps)
    assert verdict == VERDICT_PASS
    assert m_max == 8000

def test_wire_encoded_input_rejected():
    lines = [
        "PKT t=1000 TX ua=96 mt=P len=3 n=0",
        "PKT t=1500 TX ua=96 mt=P len=3 n=1",
    ]
    result = gap_deltas.analyze_lines(lines, phantom_ua=31)
    assert result.verdict == gap_deltas.VERDICT_FAIL_WRONG_UA_VOCAB
