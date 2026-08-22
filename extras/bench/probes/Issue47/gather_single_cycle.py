#!/usr/bin/env python3
import argparse
import sys
import time
from pathlib import Path

import _tracer_client
import _gap_deltas

def main() -> int:
    parser = argparse.ArgumentParser(description="Single cycle harness validation")
    parser.add_argument("--port", default=_tracer_client._DEFAULT_HOST_PORT)
    parser.add_argument("--secs", type=int, default=60)
    parser.add_argument("--stall", type=int, default=1)
    parser.add_argument("--period", type=int, default=550)
    parser.add_argument("--mode", choices=["yield", "busy"], default="yield")
    parser.add_argument("--traffic", default="fast slow loopback")
    parser.add_argument("--tag", default="single_cycle")
    args = parser.parse_args()
    s = args.stall
    p = args.period
    mode = args.mode
    traffic = args.traffic
    tag = args.tag
    
    out_dir = Path("data/results_single_cycle")
    out_dir.mkdir(parents=True, exist_ok=True)
    
    print(">>> Single Cycle Validation")
    print(f"    stall   : {s} ms")
    print(f"    period  : {p} ms")
    print(f"    mode    : {mode}")
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
    
    res = None
    cleanup_ok = True
    try:
        res = _tracer_client.run_combo(
            ser, s, p, mode, traffic, args.secs, out_dir, tag,
            capture_sniffers=True
        )
        print(f"  -> {res.verdict} max_gap={res.max_gap}")
        _gap_deltas.print_result_text(res)
    finally:
        print("\n--- Cleaning up ---")
        ser.write(b"reset\n")
        time.sleep(0.5)
        _tracer_client.flush_lines(ser)
        print("Host quiesced.")
        
        # Disable all traffic generators
        ser.write(b"disable fastwalker\n")
        time.sleep(0.1)
        ser.write(b"disable slowwalker\n")
        time.sleep(0.1)
        ser.write(b"disable toggleoutfrominput\n")
        time.sleep(0.1)
        ser.write(b"node disable 30\n")
        time.sleep(0.1)
        ser.write(b"node disable 31\n")
        time.sleep(0.1)
        
        # Verify sustained quietness by observing the line stream.
        print("Waiting for sustained bus quiet...")
        _tracer_client.flush_lines(ser)
        max_wait_s = 30.0
        quiet_window_s = 2.0
        deadline = time.time() + max_wait_s
        quiet_start = None
        activity_samples = 0
        while time.time() < deadline:
            line = ser.readline().decode("utf-8", errors="replace").strip()
            if line:
                if activity_samples < 5:
                    print(f"  activity: {line}")
                    activity_samples += 1
                quiet_start = None
                continue
            if quiet_start is None:
                quiet_start = time.time()
                continue
            if time.time() - quiet_start >= quiet_window_s:
                print("Bus is quiet.")
                break
        else:
            print("ERROR: Bus did not become quiet before timeout.")
            cleanup_ok = False
        ser.close()
        
    if not cleanup_ok:
        return 1
    if res is not None and res.verdict == _gap_deltas.VERDICT_PASS:
        return 0
    else:
        return 1

if __name__ == "__main__":
    sys.exit(main())
