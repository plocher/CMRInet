import pytest
import importlib

# Python doesn't allow importing modules that start with a number directly.
gap_deltas = importlib.import_module("47_gap_deltas")

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
