import pytest
from unittest.mock import MagicMock, patch
import sys
from pathlib import Path

sys.path.insert(0, str(Path("extras/bench/probes/regressions").absolute()))
import sweep_47

def test_sync_and_validate_boot_success():
    mock_ser = MagicMock()
    mock_ser.readline.side_effect = [
        b"",
        b"some garbage\n",
        b'{"seq":0,"ts":0,"event":"epoch","image":"xiao_host_tracer","version":"0.4.0"}\n'
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
    res = sweep_47.run_combo(mock_ser, 25, 150, "yield", "fast", 1, Path("/tmp"), "tag")
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
        b"PKT t=1000 TX ua=96 mt=P len=3 n=0\n",
        b"PKT t=1500 TX ua=96 mt=P len=3 n=1\n",
        b"END DUMP\n",
        b"", b"", b"", b""
    ]
    
    res = sweep_47.run_combo(mock_ser, 25, 150, "yield", "fast", 1, tmp_path, "test_tag")
    
    assert res.verdict != "ERROR_TIMEOUT"
    assert (tmp_path / "test_tag.log").exists()
    
