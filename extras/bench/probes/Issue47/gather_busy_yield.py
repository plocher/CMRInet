#!/usr/bin/env python3
"""
Harness for #47 backoff-under-loop-stall sweep, using XiaoHostTracer over CDC.
Replaces the old run.sh / sweep_47.sh which used compile-time configuration.
"""
import argparse
import sys
import csv
import time
import json
from pathlib import Path
import importlib.util
import serial
import datetime

import _tracer_client
import _gap_deltas
def _connect_and_configure(port: str):
    ser = _tracer_client.reboot_and_reconnect(port)
    if not _tracer_client.sync_and_validate_boot(ser):
        raise RuntimeError("Boot validation failed (timeout or wrong image).")
    print("Configuring topology for session...")
    ser.write(b"node add 30 7 7\n")
    time.sleep(0.1)
    ser.write(b"node add 31 4 4\n")
    time.sleep(0.1)
    _tracer_client.flush_lines(ser)
    return ser

def main():
    parser = argparse.ArgumentParser(description="Sweep #50 busy vs yield")
    parser.add_argument("--port", default=None,
                        help="Host device; default resolves from bench.json")
    parser.add_argument("--secs", type=int, default=60)
    parser.add_argument("--stalls", default="6 7 8 9 10 11 12")
    parser.add_argument("--periods", default="150")
    parser.add_argument("--traffic", default="")
    parser.add_argument("--out", default="sweep_results")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    args.port = args.port or _tracer_client.host_port()
    
    stalls = [int(x) for x in args.stalls.replace("*", "").split() if x.strip()]
    periods = [int(x) for x in args.periods.replace("*", "").split() if x.strip()]
    
    import datetime
    out_dir = Path(args.out)
    
    if str(out_dir) == "sweep_results":
        today = datetime.datetime.now().strftime("%Y%m%d")
        base = Path(__file__).parent / "data" / f"results.{today}.busy_yield"
        candidate = base
        count = 1
        while candidate.exists():
            candidate = Path(str(base) + f".{count}")
            count += 1
        out_dir = candidate
        
    print(">>> #50 busy-vs-yield sweep")
    print(f"    stalls  : {stalls}")
    print(f"    periods : {periods}")
    
    # Calculate non-skipped combos for dry-run
    active_combos = len(stalls) * len(periods) * 2 # yield and busy
                
    print(f"    combos  : {active_combos} total")
    print(f"    secs    : {args.secs}")
    print(f"    port    : {args.port}")
    print(f"    traffic : '{args.traffic}'")
    
    if args.dry_run:
        print("\nDry run complete. Matrix:")
        for s in stalls:
            for p in periods:
                for mode in ["yield", "busy"]:
                    tag = f"s{s}_p{p}_{mode}"
                    print(f"  {tag:15s} -> ACTIVE")
        return 0
        
    out_dir.mkdir(parents=True, exist_ok=True)
    summary_csv = out_dir / "summary.csv"
    
    write_header = not summary_csv.exists()
    
    ser = None
    cleanup_ok = True
    try:
        with summary_csv.open("a", newline="") as f:
            writer = csv.writer(f)
            if write_header:
                writer.writerow([
                    "stall_ms", "period_ms", "mode", "verdict", "tx_events", "max_gap_ms",
                    "median_gap_ms", "p90_gap_ms", "capture", "transcript",
                    "sniffer_tx", "sniffer_rx",
                    "verdict_v2", "monotone_prefix_max_ms", "it_count", "poll_count"
                ])
                
            i = 1
            total = len(stalls) * len(periods) * 2
            
            for s in stalls:
                for p in periods:
                    for mode in ["yield", "busy"]:
                        tag = f"s{s}_p{p}_{mode}"
                        print(f"[{i}/{total}] {tag}")
                        try:
                            if ser is not None:
                                ser.close()
                            ser = _connect_and_configure(args.port)
                        except RuntimeError as exc:
                            print(f"ERROR: {exc}", file=sys.stderr)
                            return 1
                        
                        res = _tracer_client.run_combo(
                            ser, s, p, mode, args.traffic, args.secs, out_dir, tag,
                            capture_sniffers=True
                        )
                        
                        # Keep round-1 compat output plus new columns
                        writer.writerow([
                            s, p, mode, res.verdict, res.poll_count, res.max_gap, 
                            res.median_gap, res.p90_gap, f"{tag}.log",
                            f"packets.{tag}.Host.raw",
                            f"packets.{tag}.TX.raw",
                            f"packets.{tag}.RX.raw",
                            res.verdict, res.monotone_prefix_max, res.it_count, res.poll_count
                        ])
                        f.flush()
                        
                        print(f"  -> {res.verdict} max_gap={res.max_gap}")
                        i += 1
        
    finally:
        print("\n--- Cleaning up ---")
        if ser is not None:
            cleanup_ok = _tracer_client.shutdown_and_verify_quiet(ser)

    # Write manifest
    manifest_path = out_dir / "manifest.json"
    if not manifest_path.exists():
        import subprocess
        try:
            sha = subprocess.check_output(["git", "rev-parse", "HEAD"]).decode("utf-8").strip()
        except:
            sha = "unknown"
            
        manifest = {
            "scenario": "busy_yield",
            "stalls": stalls,
            "periods": periods,
            "traffic": args.traffic,
            "secs": args.secs,
            "port": args.port,
            "git_sha": sha,
            "timestamp": datetime.datetime.now().isoformat()
        }
        with manifest_path.open("w") as mf:
            json.dump(manifest, mf, indent=2)
            
    if not cleanup_ok:
        return 1
    return 0

if __name__ == "__main__":
    sys.exit(main())
