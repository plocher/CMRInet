import os
import shutil

# Read files
with open("extras/bench/probes/Issue47/gather_data.py") as f:
    gather_data = f.read()

with open("extras/bench/probes/Issue47/analyze_data.py") as f:
    analyze_data = f.read()

with open("extras/bench/probes/Issue47/test_tooling.py") as f:
    test_tooling = f.read()

# 1. Create _gap_deltas.py
# Take everything up to print_result_text
_gap_deltas = analyze_data.split("def rescore(")[0]
with open("extras/bench/probes/Issue47/_gap_deltas.py", "w") as f:
    f.write(_gap_deltas)

# 2. Create analyze_stall_sweep.py
# Take imports, then rescore and main
analyze_stall_sweep = analyze_data.split("\nVERDICT_PASS")[0] + "\nimport _gap_deltas\n\n" + "def rescore(" + analyze_data.split("def rescore(")[1]
analyze_stall_sweep = analyze_stall_sweep.replace("analyze_lines(", "_gap_deltas.analyze_lines(")
analyze_stall_sweep = analyze_stall_sweep.replace("print_result_text(", "_gap_deltas.print_result_text(")
analyze_stall_sweep = analyze_stall_sweep.replace("VERDICT_ERROR", "_gap_deltas.VERDICT_ERROR")
analyze_stall_sweep = analyze_stall_sweep.replace("VERDICT_PASS", "_gap_deltas.VERDICT_PASS")

with open("extras/bench/probes/Issue47/analyze_stall_sweep.py", "w") as f:
    f.write(analyze_stall_sweep)

# 3. Create _tracer_client.py
# We will extract sync_and_validate_boot, flush_lines, run_combo
tracer_client = """import sys
import time
import json
import serial
from pathlib import Path
import _gap_deltas

_DEFAULT_HOST_PORT = "/dev/cu.usbmodem282201"

def sync_and_validate_boot(ser, timeout=15.0):
""" + gather_data.split("def sync_and_validate_boot(ser, timeout=15.0):")[1].split("def main():")[0]
with open("extras/bench/probes/Issue47/_tracer_client.py", "w") as f:
    f.write(tracer_client)

# 4. Create gather_stall_sweep.py
gather_stall_sweep = gather_data.split("import serial\n")[0] + """import serial
import datetime

import _tracer_client
import _gap_deltas

def main():
""" + gather_data.split("def main():")[1]
# Fix references inside gather_stall_sweep
gather_stall_sweep = gather_stall_sweep.replace("_DEFAULT_HOST_PORT", "_tracer_client._DEFAULT_HOST_PORT")
gather_stall_sweep = gather_stall_sweep.replace("sync_and_validate_boot", "_tracer_client.sync_and_validate_boot")
gather_stall_sweep = gather_stall_sweep.replace("flush_lines", "_tracer_client.flush_lines")
gather_stall_sweep = gather_stall_sweep.replace("run_combo", "_tracer_client.run_combo")

with open("extras/bench/probes/Issue47/gather_stall_sweep.py", "w") as f:
    f.write(gather_stall_sweep)

# 5. Create test_stall_sweep.py
test_stall_sweep = test_tooling.replace("gather_data as sweep_47", "_tracer_client as sweep_47")
test_stall_sweep = test_stall_sweep.replace("import analyze_data", "import _gap_deltas")
test_stall_sweep = test_stall_sweep.replace("analyze_data", "_gap_deltas")
with open("extras/bench/probes/Issue47/test_stall_sweep.py", "w") as f:
    f.write(test_stall_sweep)

