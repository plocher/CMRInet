#!/usr/bin/env python3
import argparse
import sys
import time
from pathlib import Path
import csv

import _tracer_client
import _gap_deltas
import serial

def main():
    parser = argparse.ArgumentParser(description="Single cycle harness validation")
    parser.add_argument("--port", default=_tracer_client._DEFAULT_HOST_PORT)
    parser.add_argument("--secs", type=int, default=60)
    args = parser.parse_args()

    # Hardcoded test case
    s = 25
    p = 1500
    mode = "yield"
    traffic = ""
    tag = "single_cycle"
    
    out_dir = Path("data/results_single_cycle")
    out_dir.mkdir(parents=True, exist_ok=True)
    
    print(">>> Single Cycle Validation")
    print(f"    stall   : {s} ms")
    print(f"    period  : {p} ms")
    print(f"    traffic : {traffic}")
    print(f"    secs    : {args.secs}")
    
    ser = _tracer_client.reboot_and_reconnect(args.port)
    
    if not _tracer_client.sync_and_validate_boot(ser):
        print("ERROR: Boot validation failed.", file=sys.stderr)
        return 1
        
    print("Configuring topology for session...")
    ser.write(b"node add 30 7 7\n")
    time.sleep(0.1)
    ser.write(b"node add 31 4 4\n")
    time.sleep(0.1)
    _tracer_client.flush_lines(ser)
    
    try:
        res = _tracer_client.run_combo(ser, s, p, mode, traffic, args.secs, out_dir, tag)
        print(f"  -> {res.verdict} max_gap={res.max_gap}")
        _gap_deltas.print_result_text(res)
    finally:
        print("\n--- Cleaning up ---")
        ser.write(b"reset\n")
        time.sleep(0.5)
        _tracer_client.flush_lines(ser)
        print("Host quiesced.")
        
    if res.verdict == _gap_deltas.VERDICT_PASS:
        return 0
    else:
        return 1

if __name__ == "__main__":
    sys.exit(main())
