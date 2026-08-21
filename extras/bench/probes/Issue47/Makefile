.PHONY: all test_tooling gather_data analyze_data

help:
	@echo "Scenario runner for Issue #47"
	@echo "make all to run entire scenario"

all: test_tooling gather_data analyze_data

test_tooling:
	../../.venv/bin/pytest test_tooling.py

gather_data:
	../../.venv/bin/python gather_data.py

analyze_data:
	../../.venv/bin/python analyze_data.py --rescore data/results.20260820
