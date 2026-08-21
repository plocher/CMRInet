.PHONY: all test_stall_sweep gather_stall_sweep analyze_stall_sweep test_busy_yield gather_busy_yield analyze_busy_yield

help:
	@echo "Scenario runner for Issue #47"
	@echo "make all to run entire scenario"
	@echo "make stall_sweep to run the stall sweep scenario"
	@echo "make busy_yield to run the busy vs yield scenario"

all: stall_sweep busy_yield

stall_sweep: test_stall_sweep gather_stall_sweep analyze_stall_sweep

busy_yield: test_busy_yield gather_busy_yield analyze_busy_yield

test_stall_sweep:
	../../.venv/bin/pytest test_stall_sweep.py

gather_stall_sweep:
	../../.venv/bin/python gather_stall_sweep.py

analyze_stall_sweep:
	../../.venv/bin/python analyze_stall_sweep.py --rescore data/results.20260820

test_busy_yield:
	../../.venv/bin/pytest test_busy_yield.py

gather_busy_yield:
	../../.venv/bin/python gather_busy_yield.py

analyze_busy_yield:
	../../.venv/bin/python analyze_busy_yield.py
